# Mambo Robot

一个集成视觉识别、语音对话、视频流接入和 Web 遥控的智能机器人项目。

**硬件制作与接线**见 [docs/制作说明.md](docs/制作说明.md)（ESP32 引脚、串口协议、香橙派配置与烧录）。

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

## App 端接口（完整）

以下接口可直接给 App/小程序/上位机使用，默认地址前缀：

```text
http://<robot-ip>:8080
```

---

### 1) 状态接口

#### 1.1 全量状态（推荐）

```text
GET /api/v1/app/state
```

返回字段（核心）：
- `status`：机器人主状态（fps、聊天状态、人脸情绪、识别物体、esp32 数据、dialog_events）
- `eye`：眼神坐标与表情
- `video`：视频流元信息

返回示例：

```json
{
  "ok": true,
  "status": {
    "fps": 24,
    "state": "listening",
    "backend_selected": "local",
    "backend_effective": "local",
    "muted": false,
    "face": { "name": "csh", "emotion": "ZhongXing", "score": 0.98 },
    "objects": [{ "label": "Ren", "prob": 0.79 }],
    "dialog_events": [
      { "user": "星宝你好", "assistant": "你好呀，我在这里陪着你。" }
    ],
    "esp32": {
      "ax": 0.02, "ay": -0.01, "az": 0.98,
      "gx": 1.2, "gy": -0.8, "gz": 0.3,
      "cliff": 0, "radar": 1, "act": "stop",
      "radar_dist": 73, "radar_energy": 21
    }
  },
  "eye": { "x": 0.12, "y": -0.08, "emotion": "ZhongXing" },
  "video": {
    "ready": true,
    "width": 640,
    "height": 480,
    "capture_fps": 30,
    "stream_fps": 15,
    "jpeg_quality": 65,
    "frame_id": 1024,
    "timestamp_ms": 1760000000000
  }
}
```

#### 1.2 轻量状态快照

```text
GET /api/v1/status
```

返回与 `/status` SSE 中单条 `data` 相同的 JSON。

#### 1.3 状态流（SSE）

```text
GET /api/v1/status/stream
Content-Type: text/event-stream
```

前端示例：

```js
const es = new EventSource("http://<robot-ip>:8080/api/v1/status/stream");
es.onmessage = (e) => {
  const data = JSON.parse(e.data);
  console.log(data.state, data.face, data.esp32);
};
```

#### 1.4 眼神流（SSE）

```text
GET /api/v1/eyes/stream
Content-Type: text/event-stream
```

---

### 2) 视频接口

#### 2.1 MJPEG 实时流

```text
GET /api/v1/video/stream
Content-Type: multipart/x-mixed-replace; boundary=frame
```

#### 2.2 单帧 JPEG

```text
GET /api/v1/video/frame
Content-Type: image/jpeg
```

#### 2.3 视频元信息

```text
GET /api/v1/video/meta
Content-Type: application/json
```

---

### 3) 控制接口

#### 3.1 电机控制（推荐）

```text
GET  /api/v1/control/motor?action=forward&duration_ms=300
POST /api/v1/control/motor
Body: {"action":"left","duration_ms":500}
```

参数：
- `action`：`forward` / `backward` / `left` / `right` / `stop` / `speed:180`
- `duration_ms`：可选，>0 时到时自动补发 `stop`
- 安全策略：当用户语音中**没有明确运动指令**（前进/后退/左转/右转/转圈等）时，后端会强制 `action=stop`，避免误动作

#### 3.2 通用控制（兼容旧版）

```text
GET  /api/v1/control/cmd?act=forward
POST /api/v1/control/cmd
Body: {"act":"stop"}
```

---

### 4) 对话链路与静音

#### 4.1 后端查询/切换

```text
GET  /api/v1/backend
GET  /api/v1/backend?mode=local
GET  /api/v1/backend?mode=baidu
GET  /api/v1/backend?mode=toggle
POST /api/v1/backend
Body: {"mode":"local"}
```

返回示例：

```json
{
  "ok": true,
  "backend_selected": "local",
  "backend_effective": "local",
  "backend_selected_asr_host": "124.222.205.168",
  "backend_selected_llm_url": "https://124.222.205.168/llm/v1/chat/completions",
  "backend_selected_tts_url": "https://124.222.205.168/tts/"
}
```

#### 4.2 静音控制

```text
GET  /api/v1/mute?mode=on|off|toggle
POST /api/v1/mute
Body: {"mode":"toggle"}
```

#### 4.3 清除对话记忆

```text
GET  /api/v1/memory/clear
POST /api/v1/memory/clear
```

说明：
- 清空内存中的对话记忆与落盘文件 `./data/dialog_memory.jsonl`
- 清除后，新的对话会从空记忆重新累计
- 典型返回：`{"ok":true,"message":"memory_cleared"}`

---

### 5) 诊断接口

```text
GET /api/v1/diag/baidu_deepseek
```

用于检测百度 token / 百度 ASR / DeepSeek 是否联通（云链路诊断）。

---

### 6) 旧接口兼容映射

当前系统仍保留以下旧接口（与 `/api/v1` 等价或兼容）：
- `/status` -> 等价 `/api/v1/status/stream`
- `/eyes` -> 等价 `/api/v1/eyes/stream`
- `/cmd` -> 兼容 `/api/v1/control/cmd`
- `/backend` -> 兼容 `/api/v1/backend`
- `/mute` -> 兼容 `/api/v1/mute`

---

### 7) App 调用建议

- 实时 UI：优先订阅 `/api/v1/status/stream`（SSE），断线后自动重连
- 首屏初始化：启动后调用一次 `/api/v1/app/state` 获取全量快照
- 视频显示：`<img src="/api/v1/video/stream">` 或原生播放器组件拉 MJPEG
- 电机操控：按下发送方向，按住每 100~200ms 重发一次，松开发 `stop`
- 语音安全：只有用户明确说了运动指令才会真正下发运动动作，否则自动 `stop`
- 会话展示：从 `status.dialog_events` 读取“用户/星宝”对话，支持开机记忆展示

## ESP32烧录

使用Arduino IDE打开 `esp32/esp32_mambo.ino` 并烧录到ESP32开发板。
