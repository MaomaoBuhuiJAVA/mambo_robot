# Mambo Robot

一个集成视觉识别、语音对话和Web遥控的智能机器人项目。

## 项目结构

```
mambo_robot/
├── build.sh                  # 一键编译运行脚本
├── CMakeLists.txt            # CMake 构建配置文件
├── models/                   # 存放所有模型文件
│   ├── yolov5.nb
│   ├── face_detection_yunet_2023mar.onnx
│   ├── face_recognition_sface_2021dec.onnx
│   └── emotion-ferplus-8.onnx
├── faces/                    # 存放人脸底库图片
├── third_party/              # 第三方单头文件库
│   ├── httplib.h             # Web服务器库
│   └── json.hpp              # nlohmann/json 库
├── src/                      # C++ 源代码目录
│   ├── config.hpp            # 全局配置
│   ├── utils.hpp             # HTTP请求与串口通信工具类
│   ├── vector_eyes.hpp       # 动态眼神 UI 渲染引擎
│   ├── vision_engine.hpp     # 视觉引擎
│   ├── dialog_system.hpp     # 语音对话引擎
│   ├── web_server.hpp        # Web 遥控器服务器
│   └── main.cpp              # 主程序入口
└── esp32/                    # ESP32 源码
    └── esp32_mambo.ino
```

## 功能模块

1. **视觉引擎** - NPU YOLO目标检测 + OpenCV人脸识别和情绪分析
2. **语音对话** - 百度ASR/TTS + DeepSeek对话生成
3. **动态眼神** - 矢量图形渲染的表情系统
4. **Web遥控** - HTTP服务器提供远程控制接口
5. **ESP32控制** - 串口通信控制舵机和电机

## 依赖安装

### 第三方库
请下载以下单头文件库并放置在 `third_party/` 目录：
- [httplib.h](https://github.com/yhirose/cpp-httplib)
- [json.hpp](https://github.com/nlohmann/json)

### 系统依赖
```bash
# OpenCV
sudo apt-get install libopencv-dev

# 其他依赖
sudo apt-get install cmake build-essential
```

## 编译运行

```bash
chmod +x build.sh
./build.sh
```

## 配置

在 `src/config.hpp` 中配置：
- 百度API密钥
- DeepSeek API密钥
- 模型文件路径
- 串口设备路径
- Web服务器端口

## ESP32烧录

使用Arduino IDE打开 `esp32/esp32_mambo.ino` 并烧录到ESP32开发板。
