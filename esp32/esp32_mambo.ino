// ESP32 Mambo Robot Controller
// 接收来自主控的串口命令，控制舵机和电机

#include <Arduino.h>

// 串口配置
#define SERIAL_BAUDRATE 115200

// 舵机引脚定义
#define SERVO_PIN_1 12
#define SERVO_PIN_2 13
#define SERVO_PIN_3 14
#define SERVO_PIN_4 15

// 电机引脚定义
#define MOTOR_LEFT_PWM 25
#define MOTOR_LEFT_DIR 26
#define MOTOR_RIGHT_PWM 27
#define MOTOR_RIGHT_DIR 33

void setup() {
  Serial.begin(SERIAL_BAUDRATE);
  
  // 初始化舵机引脚
  pinMode(SERVO_PIN_1, OUTPUT);
  pinMode(SERVO_PIN_2, OUTPUT);
  pinMode(SERVO_PIN_3, OUTPUT);
  pinMode(SERVO_PIN_4, OUTPUT);
  
  // 初始化电机引脚
  pinMode(MOTOR_LEFT_PWM, OUTPUT);
  pinMode(MOTOR_LEFT_DIR, OUTPUT);
  pinMode(MOTOR_RIGHT_PWM, OUTPUT);
  pinMode(MOTOR_RIGHT_DIR, OUTPUT);
  
  Serial.println("ESP32 Mambo Ready!");
}

void loop() {
  // 接收串口命令
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    processCommand(command);
  }
}

void processCommand(String cmd) {
  // TODO: 解析并执行命令
  // 格式示例: "SERVO:1:90" 或 "MOTOR:L:255:1"
  Serial.println("Received: " + cmd);
}
