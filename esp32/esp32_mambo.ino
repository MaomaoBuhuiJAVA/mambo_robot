// ESP32-S3 Mambo Robot Controller
// 外设: LD2402雷达, L298N电机, MPU6050陀螺仪, INA226电压, 3x红外跌落检测
// 串口: Serial(USB调试) / Serial1(香橙派通信, RX=16 TX=17)

#include <Wire.h>
#include <INA226.h>

// ================= 引脚配置 =================
const int SDA_PIN = 8,  SCL_PIN = 9;
const int RX_PIN  = 16, TX_PIN  = 17;
const int RADAR_PIN = 4;
const int CLIFF_L = 5, CLIFF_M = 6, CLIFF_R = 7;
const int ENA = 10, IN1 = 11, IN2 = 12, IN3 = 13, IN4 = 14, ENB = 15;

// ================= 全局变量 =================
INA226 ina(0x40);
const float R_SHUNT = 0.01;

#define MPU_ADDR 0x68

unsigned long last_send_time = 0;
unsigned long last_cmd_time  = 0;
const unsigned long CMD_TIMEOUT = 300;

bool   radar_triggered = false;
int    motor_speed     = 220;
String current_action  = "stop";

// 警报冷却
unsigned long cliff_alert_time = 0;
unsigned long fall_alert_time  = 0;
const unsigned long ALERT_COOLDOWN = 3000; // 3秒冷却

// 待发警报（下次上报时带上）
String pending_alert = "";

// 警报只触发一次的标志
bool cliff_alerted = false;
bool fall_alerted  = false;

// 悬崖后退结束时间（0表示未激活）
unsigned long cliff_back_until = 0;
const unsigned long CLIFF_BACK_MS = 1000; // 后退持续时间

// MPU6050 原始数据
int16_t ax, ay, az, gx, gy, gz;

// ================= MPU6050 =================
void mpuInit() {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x6B); Wire.write(0); // 唤醒
    Wire.endTransmission();
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x1C); Wire.write(0x00); // 加速度 ±2g
    Wire.endTransmission();
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x1B); Wire.write(0x00); // 陀螺仪 ±250°/s
    Wire.endTransmission();
}

void mpuRead() {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B); // ACCEL_XOUT_H
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 14, true);
    ax = Wire.read() << 8 | Wire.read();
    ay = Wire.read() << 8 | Wire.read();
    az = Wire.read() << 8 | Wire.read();
    Wire.read(); Wire.read(); // 温度（跳过）
    gx = Wire.read() << 8 | Wire.read();
    gy = Wire.read() << 8 | Wire.read();
    gz = Wire.read() << 8 | Wire.read();
}

// 转换为实际单位
float accelG(int16_t raw)  { return raw / 16384.0f; }  // ±2g
float gyroDps(int16_t raw) { return raw / 131.0f; }    // ±250°/s

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
    Serial.println("=== ESP32-S3 Mambo Boot ===");

    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

    Wire.begin(SDA_PIN, SCL_PIN, 400000);

    // INA226
    if (ina.begin()) {
        ina.setMaxCurrentShunt(5.0, R_SHUNT);
        ina.setAverage(INA226_4_SAMPLES);
        Serial.println("[OK] INA226");
    } else {
        Serial.println("[FAIL] INA226 未检测到");
    }

    // MPU6050
    mpuInit();
    // 验证连接
    Wire.beginTransmission(MPU_ADDR);
    byte err = Wire.endTransmission();
    Serial.println(err == 0 ? "[OK] MPU6050" : "[FAIL] MPU6050 未检测到");

    // 雷达 & 跌落
    pinMode(RADAR_PIN, INPUT_PULLDOWN);
    pinMode(CLIFF_L,   INPUT_PULLDOWN);
    pinMode(CLIFF_M,   INPUT_PULLDOWN);
    pinMode(CLIFF_R,   INPUT_PULLDOWN);

    // 电机
    int motorPins[] = {ENA, IN1, IN2, IN3, IN4, ENB};
    for (int p : motorPins) pinMode(p, OUTPUT);
    setMotor(0, 0, 0, 0, 0);

    Serial.println("=== 初始化完成，开始运行 ===\n");
}

// ================= loop =================
void loop() {
    // 超时自动停止（后退保护期间不干预）
    if (current_action != "stop" && current_action != "blocked" && cliff_back_until == 0) {
        if (millis() - last_cmd_time > CMD_TIMEOUT) {
            setMotor(0, 0, 0, 0, 0);
            current_action = "stop";
        }
    }

    // 悬崖检测：触发一次后退固定时长，不受悬崖状态持续影响
    unsigned long now_ms = millis();
    if (cliff_back_until > 0) {
        // 后退进行中
        if (now_ms < cliff_back_until) {
            setMotor(0, 1, 0, 1, motor_speed);
            current_action = "emergency_back";
            last_cmd_time  = now_ms; // 防止超时停止
        } else {
            // 后退结束，停车
            setMotor(0, 0, 0, 0, 0);
            current_action = "stop";
            cliff_back_until = 0;
        }
    } else if (!isSafe() && !cliff_alerted) {
        // 首次触发（未报警过）：启动后退计时
        cliff_back_until = now_ms + CLIFF_BACK_MS;
        cliff_alerted = true;
        pending_alert = "cliff";
        setMotor(0, 1, 0, 1, motor_speed);
        current_action = "emergency_back";
        last_cmd_time  = now_ms;
    }
    if (isSafe()) {
        cliff_alerted = false; // 真正离开悬崖才重置
    }

    // 跌落检测：Z轴加速度接近0（自由落体），停车 + 发警报（只报一次）
    mpuRead();
    float az_g = accelG(az);
    if (abs(az_g) < 0.2f) {
        if (current_action != "stop") {
            setMotor(0, 0, 0, 0, 0);
            current_action = "stop";
        }
        if (!fall_alerted) {
            fall_alerted = true;
            pending_alert = "fall";
        }
    } else {
        fall_alerted = false; // 恢复正常后重置
    }


    // 接收香橙派指令
    if (Serial1.available() > 0) {
        String cmd = Serial1.readStringUntil('\n');
        handleCommand(cmd);
    }

    // 雷达触发锁存
    if (digitalRead(RADAR_PIN) == HIGH) radar_triggered = true;

    // 定时上报（400ms）
    unsigned long now = millis();
    if (now - last_send_time > 400) {
        last_send_time = now;

        // 读传感器（跌落检测已在上面读过，这里补读电压电流）
        float v = ina.getBusVoltage();
        float c = abs(ina.getCurrent_mA()) / 1000.0f;

        // 转换（ax/ay/az/gx/gy/gz 已由上面 mpuRead() 更新）
        float ax_g  = accelG(ax),  ay_g  = accelG(ay),  az_g2 = accelG(az);
        float gx_d  = gyroDps(gx), gy_d  = gyroDps(gy), gz_d  = gyroDps(gz);
        bool  cliff = !isSafe();
        bool  radar = radar_triggered;
        radar_triggered = false;

        // USB串口调试打印（人类可读）
        Serial.printf("V=%.2fV  I=%.1fmA  |  "
                      "A=[%.2f,%.2f,%.2f]g  G=[%.1f,%.1f,%.1f]dps  |  "
                      "Cliff=%s  Radar=%s  Act=%s\n",
                      v, c * 1000,
                      ax_g, ay_g, az_g2,
                      gx_d, gy_d, gz_d,
                      cliff ? "YES" : "no",
                      radar ? "YES" : "no",
                      current_action.c_str());

        // Serial1 发给香橙派（JSON）
        Serial1.printf("{\"v\":%.2f,\"c\":%.1f,"
                       "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                       "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f,"
                       "\"cliff\":%d,\"radar\":%d,\"act\":\"%s\",\"alert\":\"%s\"}\n",
                       v, c,
                       ax_g, ay_g, az_g2,
                       gx_d, gy_d, gz_d,
                       cliff ? 1 : 0,
                       radar ? 1 : 0,
                       current_action.c_str(),
                       pending_alert.c_str());
        pending_alert = ""; // 发完清空
    }
}
