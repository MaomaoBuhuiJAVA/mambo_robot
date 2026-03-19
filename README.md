# Mambo Robot

一个集成视觉识别、语音对话、视频流接入和 Web 遥控的智能机器人项目。

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
4. **视频流服务** - HTTP MJPEG 实时视频流和单帧抓图接口
5. **Web遥控** - HTTP服务器提供远程控制接口
6. **ESP32控制** - 串口通信控制舵机和电机

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
- 摄像头与视频流参数

## 视频流接入

项目现在提供标准化的视频流接口，供软件开发工程师直接接入：

### 1. MJPEG 实时视频流

```text
GET /api/v1/video/stream
Content-Type: multipart/x-mixed-replace; boundary=frame
```

适用场景：
- 浏览器 `<img>` 直接显示
- OpenCV `VideoCapture(url)` 拉流
- VLC、上位机、巡检面板、调试工具

### 2. 单帧 JPEG 抓图

```text
GET /api/v1/video/frame
Content-Type: image/jpeg
```

适用场景：
- 健康检查
- 周期截图
- AI/规则引擎按需拉取最新画面

### 3. 视频流元信息

```text
GET /api/v1/video/meta
Content-Type: application/json
```

返回字段示例：

```json
{
  "ready": true,
  "width": 640,
  "height": 480,
  "capture_fps": 30,
  "stream_fps": 15,
  "jpeg_quality": 88,
  "frame_id": 1024,
  "timestamp_ms": 1760000000000
}
```

### 接入示例

浏览器：

```html
<img src="http://<robot-ip>:8080/api/v1/video/stream" alt="mambo video stream" />
```

OpenCV：

```cpp
cv::VideoCapture cap("http://<robot-ip>:8080/api/v1/video/stream");
cv::Mat frame;
cap >> frame;
```

curl 抓图：

```bash
curl http://<robot-ip>:8080/api/v1/video/frame --output latest.jpg
```

### 性能策略

- 摄像头采集、视觉推理、JPEG 编码、HTTP 分发已解耦
- 服务端只编码“最新一帧”，避免多客户端重复编码
- MJPEG 对接简单、兼容性强，适合机器人调试和上位机快速集成
- 质量和帧率可在 `src/config.hpp` 中通过 `kVideoJpegQuality`、`kVideoStreamFps` 调整

## ESP32烧录

使用Arduino IDE打开 `esp32/esp32_mambo.ino` 并烧录到ESP32开发板。
