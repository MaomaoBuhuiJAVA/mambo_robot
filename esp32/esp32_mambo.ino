// ESP32-S3 Mambo 机器人控制器
// 外设: LD2402雷达(Serial2), L298N电机, MPU6050陀螺仪, INA226电压, 3x红外跌落检测
// 串口: Serial(USB调试) / Serial1(香橙派通信 RX=16 TX=17) / Serial2(LD2402 RX=18 TX=19)

#include <Wire.h>
#include <INA226.h>

// ================= 引脚配置 =================
const int SDA_PIN = 8,  SCL_PIN = 9;
const int RX_PIN  = 16, TX_PIN  = 17;
const int RADAR_RX = 18, RADAR_TX = 19;
const int CLIFF_L = 5, CLIFF_M = 6, CLIFF_R = 7;
const int ENA = 10, IN1 = 11, IN2 = 12, IN3 = 13, IN4 = 14, ENB = 15;

// ================= 全局变量 =================
INA226 ina(0x40);
const float R_SHUNT = 0.01f;
#define MPU_ADDR 0x68

unsigned long last_send_time = 0;
unsigned long last_cmd_time  = 0;
const unsigned long CMD_TIMEOUT = 300;

int    motor_speed    = 220;
String current_action = "stop";

// 警报只触发一次的标志（离开触发条件后自动重置）
bool cliff_alerted    = false;
bool fall_alerted     = false;
bool agitated_alerted = false;

// 待发警报（下次上报时带上，发完清空）
String pending_alert = "";

// ================= LD2402 文本行解析 =================
// LD2402 实际输出格式：每行 "distance:XX\r\n"（纯文本，非工程模式二进制帧）
struct RadarData {
    uint16_t dist_cm  = 0;   // 最新距离（cm）
    float    dist_std = 0.0f; // 滑动窗口标准差（烦躁判断）
    bool     valid    = false;
} radar_data;

// 滑动窗口（10个样本）计算标准差
const int  RADAR_WIN = 10;
float      radar_win[RADAR_WIN] = {};
int        radar_win_idx = 0;
int        radar_win_cnt = 0;

// 烦躁检测：std > 阈值持续 3 秒触发一次
const float        AGITATED_STD_THRESH   = 15.0f;
const unsigned long AGITATED_DURATION_MS = 3000;
unsigned long agitated_start = 0;

// LD2402 串口行缓冲
char     ld_line_buf[32];
uint8_t  ld_line_len = 0;

// 从 Serial2 读取文本行，解析 "distance:XX"
void ldReadSerial() {
    while (Serial2.available()) {
        char c = (char)Serial2.read();
        if (c == '\n') {
            ld_line_buf[ld_line_len] = '\0';
            // 去掉末尾 \r
            if (ld_line_len > 0 && ld_line_buf[ld_line_len - 1] == '\r')
                ld_line_buf[--ld_line_len] = '\0';

            // 解析 "distance:XX"
            if (strncmp(ld_line_buf, "distance:", 9) == 0) {
                int d = atoi(ld_line_buf + 9);
                if (d > 0 && d < 800) {
                    radar_data.dist_cm = (uint16_t)d;
                    radar_data.valid   = true;

                    // 更新滑动窗口
                    radar_win[radar_win_idx] = (float)d;
                    radar_win_idx = (radar_win_idx + 1) % RADAR_WIN;
                    if (radar_win_cnt < RADAR_WIN) radar_win_cnt++;

                    // 计算标准差
                    if (radar_win_cnt >= 3) {
                        float sum = 0;
                        for (int i = 0; i < radar_win_cnt; i++) sum += radar_win[i];
                        float mean = sum / radar_win_cnt;
                        float var  = 0;
                        for (int i = 0; i < radar_win_cnt; i++) {
                            float diff = radar_win[i] - mean;
                            var += diff * diff;
                        }
                        radar_data.dist_std = sqrt(var / radar_win_cnt);
                    }
                    Serial.printf("[LD2402] dist=%dcm std=%.1f\n", d, radar_data.dist_std);
                }
            }
            ld_line_len = 0;
        } else if (c != '\r') {
            if (ld_line_len < sizeof(ld_line_buf) - 1)
                ld_line_buf[ld_line_len++] = c;
            else
                ld_line_len = 0; // 行太长，丢弃
        }
    }
}

// 悬崖后退结束时间（0=未激活）
unsigned long cliff_back_until = 0;
const unsigned long CLIFF_BACK_MS = 1000;

// MPU6050 原始数据
int16_t ax, ay, az, gx, gy, gz;

// ================= MPU6050 =================
void mpuInit() {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x6B); Wire.write(0);
    Wire.endTransmission();
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x1C); Wire.write(0x00); // ±2g
    Wire.endTransmission();
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x1B); Wire.write(0x00); // ±250°/s
    Wire.endTransmission();
}

void mpuRead() {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 14, true);
    ax = Wire.read() << 8 | Wire.read();
    ay = Wire.read() << 8 | Wire.read();
    az = Wire.read() << 8 | Wire.read();
    Wire.read(); Wire.read(); // 温度跳过
    gx = Wire.read() << 8 | Wire.read();
    gy = Wire.read() << 8 | Wire.read();
    gz = Wire.read() << 8 | Wire.read();
}

float accelG(int16_t raw)  { return raw / 16384.0f; }
float gyroDps(int16_t raw) { return raw / 131.0f; }

// ================= 电机驱动 =================
void setMotor(int l1, int l2, int r1, int r2, int s) {
    digitalWrite(IN1, l1); digitalWrite(IN2, l2); analogWrite(ENA, s);
    digitalWrite(IN3, r1); digitalWrite(IN4, r2); analogWrite(ENB, s);
}

bool isSafe() {
    return !(digitalRead(CLIFF_L) || digitalRead(CLIFF_M) || digitalRead(CLIFF_R));
}

void handleCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;
    if (cmd != "stop") last_cmd_time = millis();

    if (!isSafe() && (cmd == "forward" || cmd == "left" || cmd == "right")) {
        setMotor(0, 0, 0, 0, 0);
        current_action = "blocked";
        return;
    }

    if      (cmd == "forward")  { setMotor(1, 0, 1, 0, motor_speed); current_action = "forward"; }
    else if (cmd == "backward") { setMotor(0, 1, 0, 1, motor_speed); current_action = "backward"; }
    else if (cmd == "left")     { setMotor(0, 1, 1, 0, motor_speed); current_action = "left"; }
    else if (cmd == "right")    { setMotor(1, 0, 0, 1, motor_speed); current_action = "right"; }
    else if (cmd == "stop")     { setMotor(0, 0, 0, 0, 0);           current_action = "stop"; }
    else if (cmd.startsWith("speed:")) {
        motor_speed = constrain(cmd.substring(6).toInt(), 0, 255);
    }
}

// ================= setup =================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== ESP32-S3 Mambo 启动 ===");

    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

    // LD2402 串口（纯文本模式，无需发配置命令）
    Serial2.begin(115200, SERIAL_8N1, RADAR_RX, RADAR_TX);
    Serial.println("[OK] LD2402 串口已启动（文本模式）");

    Wire.begin(SDA_PIN, SCL_PIN, 400000);

    // INA226
    if (ina.begin()) {
        ina.setMaxCurrentShunt(5.0, R_SHUNT);
        ina.setAverage(INA226_4_SAMPLES);
        Serial.println("[OK] INA226");
    } else {
        Serial.println("[FAIL] INA226");
    }

    // MPU6050
    mpuInit();
    Wire.beginTransmission(MPU_ADDR);
    Serial.println(Wire.endTransmission() == 0 ? "[OK] MPU6050" : "[FAIL] MPU6050");

    // 引脚
    pinMode(CLIFF_L, INPUT_PULLDOWN);
    pinMode(CLIFF_M, INPUT_PULLDOWN);
    pinMode(CLIFF_R, INPUT_PULLDOWN);
    int motorPins[] = {ENA, IN1, IN2, IN3, IN4, ENB};
    for (int p : motorPins) pinMode(p, OUTPUT);
    setMotor(0, 0, 0, 0, 0);

    Serial.println("=== 初始化完成，开始运行 ===\n");
}

// ================= loop =================
void loop() {
    unsigned long now_ms = millis();

    // 超时自动停止
    if (current_action != "stop" && current_action != "blocked" && cliff_back_until == 0) {
        if (now_ms - last_cmd_time > CMD_TIMEOUT) {
            setMotor(0, 0, 0, 0, 0);
            current_action = "stop";
        }
    }

    // 悬崖检测
    if (cliff_back_until > 0) {
        if (now_ms < cliff_back_until) {
            setMotor(0, 1, 0, 1, motor_speed);
            current_action = "emergency_back";
            last_cmd_time  = now_ms;
        } else {
            setMotor(0, 0, 0, 0, 0);
            current_action   = "stop";
            cliff_back_until = 0;
        }
    } else if (!isSafe() && !cliff_alerted) {
        cliff_back_until = now_ms + CLIFF_BACK_MS;
        cliff_alerted    = true;
        pending_alert    = "cliff";
        setMotor(0, 1, 0, 1, motor_speed);
        current_action = "emergency_back";
        last_cmd_time  = now_ms;
    }
    if (isSafe()) cliff_alerted = false;

    // 读 LD2402 文本数据
    ldReadSerial();

    // 烦躁检测：std > 阈值持续 3 秒触发一次
    if (radar_data.valid && radar_data.dist_std > AGITATED_STD_THRESH) {
        if (agitated_start == 0) agitated_start = now_ms;
        if (!agitated_alerted && now_ms - agitated_start > AGITATED_DURATION_MS) {
            agitated_alerted = true;
            if (pending_alert.isEmpty()) pending_alert = "agitated";
        }
    } else {
        agitated_start   = 0;
        agitated_alerted = false;
    }

    // 读 MPU6050
    mpuRead();
    float ax_g  = accelG(ax),  ay_g = accelG(ay),  az_g = accelG(az);
    float gx_d  = gyroDps(gx), gy_d = gyroDps(gy), gz_d = gyroDps(gz);

    // 晕眩检测：陀螺仪任意轴 > 50°/s 持续 200ms
    static unsigned long dizzy_start  = 0;
    static bool          dizzy_alerted = false;
    float gyro_max = max(abs(gx_d), max(abs(gy_d), abs(gz_d)));
    if (gyro_max > 50.0f) {
        if (dizzy_start == 0) dizzy_start = now_ms;
        if (!dizzy_alerted && now_ms - dizzy_start > 200) {
            dizzy_alerted = true;
            if (pending_alert.isEmpty()) pending_alert = "dizzy";
        }
    } else {
        dizzy_start   = 0;
        dizzy_alerted = false;
    }

    // 跌落检测：合力 < 0.3g 且无晕眩
    float accel_total = sqrt(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
    if (accel_total < 0.3f && !dizzy_alerted) {
        if (current_action != "stop") { setMotor(0, 0, 0, 0, 0); current_action = "stop"; }
        if (!fall_alerted) {
            fall_alerted = true;
            if (pending_alert.isEmpty()) pending_alert = "fall";
        }
    } else {
        fall_alerted = false;
    }

    // 接收香橙派指令
    if (Serial1.available() > 0) {
        String cmd = Serial1.readStringUntil('\n');
        handleCommand(cmd);
    }

    // 定时上报（400ms）
    if (now_ms - last_send_time > 400) {
        last_send_time = now_ms;

        float v = ina.getBusVoltage();
        float c = abs(ina.getCurrent_mA()) / 1000.0f;
        bool  cliff = !isSafe();

        // USB 调试打印
        Serial.printf("V=%.2fV I=%.1fmA | A=[%.2f,%.2f,%.2f]g G=[%.1f,%.1f,%.1f]dps | "
                      "悬崖=%s 动作=%s | LD2402: dist=%dcm std=%.1f\n",
                      v, c * 1000,
                      ax_g, ay_g, az_g,
                      gx_d, gy_d, gz_d,
                      cliff ? "是" : "否",
                      current_action.c_str(),
                      (int)radar_data.dist_cm,
                      radar_data.dist_std);

        // Serial1 发给香橙派（JSON）
        // radar_energy 传 std*10 整数化，兼容旧字段名
        Serial1.printf("{\"v\":%.2f,\"c\":%.4f,"
                       "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                       "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f,"
                       "\"cliff\":%d,\"radar\":1,\"act\":\"%s\",\"alert\":\"%s\","
                       "\"radar_dist\":%d,\"radar_energy\":%d}\n",
                       v, c,
                       ax_g, ay_g, az_g,
                       gx_d, gy_d, gz_d,
                       cliff ? 1 : 0,
                       current_action.c_str(),
                       pending_alert.c_str(),
                       (int)radar_data.dist_cm,
                       (int)(radar_data.dist_std * 10));
        pending_alert = "";
    }
}
