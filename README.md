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

### 需要准备的文件（新机器最容易漏）

- **模型文件（必须）**：`models/`
  - `yolov5.nb`（NPU YOLO）
  - `face_detection_yunet_2023mar.onnx`（YuNet）
  - `face_recognition_sface_2021dec.onnx`（SFace）
  - `emotion-ferplus-8.onnx`（当前默认禁用，但建议保留文件）
- **人脸底库（可选）**：`faces/` 或 `src/config.hpp` 里配置的 `kBaseImgDir`

说明：
- `third_party/` 已随仓库提供（`httplib.h`、`json.hpp` 等），一般**不需要额外下载**。

### OrangePi（推荐部署环境）系统依赖清单

在新刷的 OrangePi/Ubuntu/Armbian 系统上，建议直接装齐下面这些（一次到位）：

```bash
sudo apt update
sudo apt install -y \
  git cmake build-essential pkg-config \
  libopencv-dev \
  libcurl4-openssl-dev \
  v4l-utils \
  ffmpeg \
  alsa-utils pulseaudio-utils pipewire-audio \
  xdg-utils
```

常见用途：
- `libopencv-dev`：YuNet/SFace/OpenCV（含 DNN）
- `libcurl4-openssl-dev`：HTTP 请求（对话/上报等）
- `v4l-utils`：摄像头排查（`v4l2-ctl`）
- `alsa-utils/pulseaudio-utils/pipewire-audio`：音量/播放链路排查
- `xdg-utils`：脚本里拉起浏览器（若用到）

### 开发电脑（用于改代码/提交 GitHub）

如果只是改代码 + 推送 GitHub：只需要 **Git**。

如果要在电脑本地也能编译（Linux 电脑推荐）：

```bash
sudo apt update
sudo apt install -y git cmake build-essential libopencv-dev libcurl4-openssl-dev
```

Windows 本机通常不作为运行环境；如果需要在 Windows 上编译，建议使用 WSL2（Ubuntu）按上面的 Linux 方式装依赖。

## 新 OrangePi 快速部署（从 0 到可运行）

### 1) 拉代码

```bash
git clone <your-github-repo-url>
cd mambo_robot
```

### 2) 放模型文件

把模型拷贝到 `./models/`（确保文件名与 `src/config.hpp` 中一致）。

快速自检：

```bash
ls -lh models/
```

### 3) 配置关键参数

编辑 `src/config.hpp`：
- **串口**：`kSerialPort`（例如 `/dev/ttyS7`、`/dev/ttyUSB0` 等）
- **摄像头**：`kCameraIndex`、分辨率、帧率等
- **云端 key**：百度/DeepSeek 等（如果启用云链路）

### 4) 串口权限（非常常见的“部署后无法打开串口”原因）

```bash
ls -l /dev/ttyS7
groups
sudo usermod -aG dialout $USER
```

执行完 `usermod` 后需要**重新登录**一次（或重启）让组权限生效。

如果你的设备不是 `/dev/ttyS7`，请用下面命令找真实设备名：

```bash
ls /dev/ttyS* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

### 5) 摄像头自检

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
```

### 6) 一键编译运行

```bash
chmod +x build.sh run.sh
./build.sh
./run.sh
```

提示：
- `build.sh` 会**删除 build 目录并全量重编译**，适合首次部署/大改动后使用
- `run.sh` 默认会走“快速启动”，如果你刚改了代码又想马上生效，优先跑 `./build.sh`

## OpenCV 升级（OrangePi 上推荐 4.8/4.9，用于稳定 YuNet）

当前系统仓库自带的 OpenCV 4.5.4 在部分 DNN ONNX（例如 `face_detection_yunet_2023mar.onnx`）上可能出现不稳定（例如 layer id=-1）。推荐在 OrangePi 上升级到 **OpenCV 4.8/4.9**。

下面给出一套“源码编译安装到 `/usr/local`”的流程（通用、最稳），项目的 `CMakeLists.txt` 已做了 `/usr/local` 优先查找的兼容。

### 1) 安装编译依赖

```bash
sudo apt update
sudo apt install -y \
  git cmake build-essential pkg-config \
  libjpeg-dev libpng-dev libtiff-dev \
  libavcodec-dev libavformat-dev libswscale-dev \
  libv4l-dev libxvidcore-dev libx264-dev \
  libgtk-3-dev \
  libatlas-base-dev gfortran \
  python3-dev python3-numpy
```

### 2) 获取 OpenCV 源码（以 4.9.0 为例）

```bash
cd ~
git clone --depth 1 --branch 4.9.0 https://github.com/opencv/opencv.git
git clone --depth 1 --branch 4.9.0 https://github.com/opencv/opencv_contrib.git
```

### 3) 编译并安装到 `/usr/local`

```bash
cd ~/opencv
mkdir -p build && cd build

cmake -D CMAKE_BUILD_TYPE=Release \
      -D CMAKE_INSTALL_PREFIX=/usr/local \
      -D OPENCV_EXTRA_MODULES_PATH=~/opencv_contrib/modules \
      -D BUILD_EXAMPLES=OFF \
      -D BUILD_TESTS=OFF \
      -D BUILD_PERF_TESTS=OFF \
      -D BUILD_opencv_python3=OFF \
      -D WITH_GSTREAMER=ON \
      -D WITH_V4L=ON \
      -D WITH_OPENGL=OFF \
      -D WITH_TBB=OFF \
      ..

make -j"$(nproc)"
sudo make install
sudo ldconfig
```

编译时间说明：
- OrangePi 上第一次编译可能需要 20~60 分钟（取决于存储/散热/CPU 降频）。

### 4) 验证版本

```bash
pkg-config --modversion opencv4 || true
python3 - <<'PY'
import cv2
print(cv2.__version__)
PY
```

### 5) 让本项目使用新 OpenCV

一般来说安装到 `/usr/local` 后，`cmake` 会优先找到新版本（本项目也已在 `CMakeLists.txt` 里对 `/usr/local` 做了优先路径）。

如果仍然链接到了旧的 `/usr` 版本，可以在构建本项目时显式指定：

```bash
cd ~/Desktop/mambo_robot
rm -rf build
OpenCV_DIR=/usr/local/lib/cmake/opencv4 ./build.sh
```

### 6) 常见冲突处理（可选）

如果系统里同时存在 `libopencv-dev`（旧版）和 `/usr/local`（新版），通常没问题；但若你想避免混用，可以卸载旧 dev 包：

```bash
sudo apt remove -y libopencv-dev
sudo apt autoremove -y
```

（注意：卸载可能影响系统里其它依赖 OpenCV 的工具，按需选择。）

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
    "dialog_turn_count": 18,
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
其中 `dialog_turn_count` 表示累计对话轮次（每次“用户一句 + 星宝一句”记为 1）。

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
- Web 控制台中，“清空日志/记忆”按钮位于右侧“星宝日志”卡片头部

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
