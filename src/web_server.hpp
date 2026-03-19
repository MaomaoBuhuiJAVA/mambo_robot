#pragma once
#include "../third_party/httplib.h"
#include "config.hpp"
#include "utils.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mambo {

class WebServer {
    httplib::Server svr;
    std::thread server_thread;
    std::thread encoder_thread;
    std::atomic<bool> running_{false};

    // /eyes SSE（眼睛UI）
    std::mutex eye_mtx_;
    std::condition_variable eye_cv_;
    std::string eye_data_ = "{\"x\":0,\"y\":0,\"emotion\":\"neutral\"}";
    uint64_t    eye_seq_  = 0;

    // /status SSE（app状态）
    std::mutex status_mtx_;
    std::string status_data_ = "{}";

    // 视频：原始帧（视觉线程写）
    std::mutex              video_raw_mtx_;
    std::condition_variable video_raw_cv_;
    cv::Mat                 latest_raw_frame_;
    uint64_t                latest_raw_seq_      = 0;
    int                     latest_frame_width_  = 0;
    int                     latest_frame_height_ = 0;
    long long               latest_frame_ts_ms_  = 0;

    // 视频：编码后 JPEG（编码线程写，流线程读）
    std::mutex                                video_jpeg_mtx_;
    std::shared_ptr<const std::vector<uchar>> latest_jpeg_;
    uint64_t                                  latest_jpeg_seq_ = 0;

public:
    // ── 数据推送接口 ─────────────────────────────────────────────

    void PushEyeData(float nx, float ny, const std::string& emotion) {
        {
            std::lock_guard<std::mutex> lk(eye_mtx_);
            eye_data_ = "{\"x\":" + std::to_string(nx)
                      + ",\"y\":" + std::to_string(ny)
                      + ",\"emotion\":\"" + emotion + "\"}";
            ++eye_seq_;
        }
        eye_cv_.notify_all();
    }

    // 视觉线程每帧调用（异步编码，不阻塞视觉线程）
    void PushVideoFrame(const cv::Mat& frame) {
        if (frame.empty()) return;
        {
            std::lock_guard<std::mutex> lk(video_raw_mtx_);
            frame.copyTo(latest_raw_frame_);
            latest_frame_width_  = frame.cols;
            latest_frame_height_ = frame.rows;
            latest_frame_ts_ms_  = NowMs();
            ++latest_raw_seq_;
        }
        video_raw_cv_.notify_one();
    }

    void PushStatus(const std::string& json) {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_data_ = json;
    }

    // ── 启动 ─────────────────────────────────────────────────────

    void Start(SerialManager* serial) {
        running_ = true;

        svr.set_mount_point("/", "./eyes-ui/dist");

        // CORS 预检
        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_content("", "text/plain");
        });

        // ── 眼睛UI SSE /eyes ─────────────────────────────────────
        svr.Get("/eyes", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            uint64_t last_seq = 0;
            res.set_chunked_content_provider("text/event-stream",
                [this, last_seq](size_t, httplib::DataSink& sink) mutable {
                    std::string data;
                    {
                        std::unique_lock<std::mutex> lk(eye_mtx_);
                        eye_cv_.wait_for(lk, std::chrono::milliseconds(100),
                            [this, &last_seq]() { return !running_ || eye_seq_ != last_seq; });
                        if (!running_) return false;
                        data     = eye_data_;
                        last_seq = eye_seq_;
                    }
                    std::string msg = "data: " + data + "\n\n";
                    return sink.write(msg.c_str(), msg.size());
                });
        });

        // ── App 状态 SSE /status ─────────────────────────────────
        svr.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("text/event-stream",
                [this](size_t, httplib::DataSink& sink) {
                    std::string data;
                    { std::lock_guard<std::mutex> lk(status_mtx_); data = status_data_; }
                    std::string msg = "data: " + data + "\n\n";
                    if (!sink.write(msg.c_str(), msg.size())) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    return running_.load();
                });
        });

        // ── MJPEG 视频流 /api/v1/video/stream ────────────────────
        svr.Get("/api/v1/video/stream", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=frame",
                [this](size_t, httplib::DataSink& sink) {
                    uint64_t last_sent_seq = 0;
                    const auto interval = std::chrono::milliseconds(
                        1000 / std::max(1, AppConfig::kVideoStreamFps));
                    while (running_) {
                        uint64_t seq = 0;
                        auto frame = GetLatestEncodedFrame(seq);
                        if (!frame || frame->empty()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            continue;
                        }
                        if (seq == last_sent_seq) {
                            std::this_thread::sleep_for(interval);
                            continue;
                        }
                        std::string header =
                            "--frame\r\n"
                            "Content-Type: image/jpeg\r\n"
                            "Content-Length: " + std::to_string(frame->size()) + "\r\n"
                            "X-Frame-Id: " + std::to_string(seq) + "\r\n\r\n";
                        if (!sink.write(header.c_str(), header.size())) return false;
                        if (!sink.write(reinterpret_cast<const char*>(frame->data()), frame->size())) return false;
                        if (!sink.write("\r\n", 2)) return false;
                        last_sent_seq = seq;
                    }
                    return true;
                });
        });

        // 兼容旧路径 /stream
        svr.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=frame",
                [this](size_t, httplib::DataSink& sink) {
                    uint64_t last_sent_seq = 0;
                    const auto interval = std::chrono::milliseconds(
                        1000 / std::max(1, AppConfig::kVideoStreamFps));
                    while (running_) {
                        uint64_t seq = 0;
                        auto frame = GetLatestEncodedFrame(seq);
                        if (!frame || frame->empty()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            continue;
                        }
                        if (seq == last_sent_seq) {
                            std::this_thread::sleep_for(interval);
                            continue;
                        }
                        std::string header =
                            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                            + std::to_string(frame->size()) + "\r\n\r\n";
                        if (!sink.write(header.c_str(), header.size())) return false;
                        if (!sink.write(reinterpret_cast<const char*>(frame->data()), frame->size())) return false;
                        if (!sink.write("\r\n", 2)) return false;
                        last_sent_seq = seq;
                    }
                    return true;
                });
        });

        // ── 单帧抓图 /api/v1/video/frame ─────────────────────────
        svr.Get("/api/v1/video/frame", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            uint64_t seq = 0;
            auto frame = GetLatestEncodedFrame(seq);
            if (!frame) { res.status = 503; res.set_content("not ready", "text/plain"); return; }
            res.set_header("X-Frame-Id", std::to_string(seq));
            res.set_content(reinterpret_cast<const char*>(frame->data()), frame->size(), "image/jpeg");
        });

        // ── 视频元信息 /api/v1/video/meta ────────────────────────
        svr.Get("/api/v1/video/meta", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            int w = 0, h = 0; long long ts = 0;
            { std::lock_guard<std::mutex> lk(video_raw_mtx_); w = latest_frame_width_; h = latest_frame_height_; ts = latest_frame_ts_ms_; }
            uint64_t seq = 0; bool ready = false;
            { std::lock_guard<std::mutex> lk(video_jpeg_mtx_); ready = latest_jpeg_ && !latest_jpeg_->empty(); seq = latest_jpeg_seq_; }
            std::ostringstream oss;
            oss << "{\"ready\":" << (ready ? "true" : "false")
                << ",\"width\":"        << w
                << ",\"height\":"       << h
                << ",\"capture_fps\":"  << AppConfig::kCameraTargetFps
                << ",\"stream_fps\":"   << AppConfig::kVideoStreamFps
                << ",\"jpeg_quality\":" << AppConfig::kVideoJpegQuality
                << ",\"frame_id\":"     << seq
                << ",\"timestamp_ms\":" << ts << "}";
            res.set_content(oss.str(), "application/json");
        });

        // ── 控制指令 /cmd（GET + POST）───────────────────────────
        auto handle_cmd = [serial](const std::string& act, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!act.empty() && serial) serial->SendCommand(act);
            res.set_content("{\"ok\":true}", "application/json");
        };

        svr.Get("/cmd", [handle_cmd](const httplib::Request& req, httplib::Response& res) {
            handle_cmd(req.has_param("act") ? req.get_param_value("act") : "", res);
        });

        svr.Post("/cmd", [handle_cmd](const httplib::Request& req, httplib::Response& res) {
            std::string act;
            auto pos = req.body.find("\"act\"");
            if (pos != std::string::npos) {
                auto q1 = req.body.find('"', pos + 5);
                if (q1 != std::string::npos) {
                    auto q2 = req.body.find('"', q1 + 1);
                    if (q2 != std::string::npos) act = req.body.substr(q1 + 1, q2 - q1 - 1);
                }
            }
            handle_cmd(act, res);
        });

        // ── 控制台页面 /control ──────────────────────────────────
        svr.Get("/control", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(
                "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>button{width:100px;height:100px;font-size:24px;margin:10px;"
                "border-radius:20px;background:#00d7ff;border:none;}</style></head>"
                "<body style='text-align:center;background:#222;color:white;padding-top:50px'>"
                "<h2>Mambo</h2>"
                "<div><button onclick=\"fetch('/cmd?act=forward')\">FWD</button></div>"
                "<div>"
                "<button onclick=\"fetch('/cmd?act=left')\">LEFT</button>"
                "<button onclick=\"fetch('/cmd?act=stop')\" style='background:#ff4444'>STOP</button>"
                "<button onclick=\"fetch('/cmd?act=right')\">RIGHT</button>"
                "</div>"
                "<div><button onclick=\"fetch('/cmd?act=backward')\">BACK</button></div>"
                "<hr>"
                "<p>视频流: <a href='/api/v1/video/stream' style='color:#0f0'>/api/v1/video/stream</a></p>"
                "<p>单帧: <a href='/api/v1/video/frame' style='color:#0f0'>/api/v1/video/frame</a></p>"
                "<p>元信息: <a href='/api/v1/video/meta' style='color:#0f0'>/api/v1/video/meta</a></p>"
                "<p>状态SSE: <a href='/status' style='color:#0f0'>/status</a></p>"
                "</body></html>",
                "text/html");
        });

        // ── Dashboard /dashboard ──────────────────────────────────
        svr.Get("/dashboard", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"html(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mambo Dashboard</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d1117;color:#e6edf3;font-family:monospace;font-size:13px}
  h1{padding:12px 16px;background:#161b22;border-bottom:1px solid #30363d;font-size:16px;letter-spacing:2px}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:12px}
  @media(max-width:700px){.grid{grid-template-columns:1fr}}
  .card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px}
  .card h2{font-size:12px;color:#8b949e;margin-bottom:10px;text-transform:uppercase;letter-spacing:1px}
  /* 视频 */
  #cam{width:100%;border-radius:4px;display:block}
  /* 眼睛预览 */
  #eye-canvas{width:100%;height:140px;background:#050a08;border-radius:4px;display:block}
  /* 传感器 */
  .row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
  .row:last-child{border:none}
  .label{color:#8b949e}
  .val{color:#58a6ff;font-weight:bold}
  .val.warn{color:#f85149}
  .val.ok{color:#3fb950}
  /* 控制器 */
  .ctrl{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;max-width:240px;margin:0 auto}
  .btn{padding:14px;border:none;border-radius:8px;background:#21262d;color:#e6edf3;
       font-size:20px;cursor:pointer;transition:background .1s;user-select:none}
  .btn:active,.btn.active{background:#1f6feb}
  .btn.stop{background:#da3633}
  .btn.stop:active{background:#f85149}
  .speed-row{margin-top:10px;display:flex;align-items:center;gap:8px}
  .speed-row input{flex:1;accent-color:#1f6feb}
  /* 状态点 */
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  .dot.on{background:#3fb950;box-shadow:0 0 6px #3fb950}
  .dot.off{background:#f85149;box-shadow:0 0 6px #f85149}
</style>
</head>
<body>
<h1>&#x1F916; MAMBO DASHBOARD</h1>
<div class="grid">

  <!-- 监控画面 -->
  <div class="card">
    <h2>&#x1F4F7; 监控画面</h2>
    <img id="cam" src="/api/v1/video/stream" alt="camera">
  </div>

  <!-- 眼睛预览 -->
  <div class="card">
    <h2>&#x1F441; 眼睛状态</h2>
    <canvas id="eye-canvas"></canvas>
    <div style="margin-top:8px" class="row">
      <span class="label">情绪</span>
      <span class="val" id="emo">-</span>
    </div>
    <div class="row">
      <span class="label">SSE</span>
      <span id="eye-dot"><span class="dot off"></span>断开</span>
    </div>
  </div>

  <!-- 传感器数据 -->
  <div class="card">
    <h2>&#x1F4CA; 传感器</h2>
    <div class="row"><span class="label">电压</span><span class="val" id="s-v">-</span></div>
    <div class="row"><span class="label">电流</span><span class="val" id="s-c">-</span></div>
    <div class="row"><span class="label">加速度 X/Y/Z</span><span class="val" id="s-acc">-</span></div>
    <div class="row"><span class="label">陀螺 X/Y/Z</span><span class="val" id="s-gyr">-</span></div>
    <div class="row"><span class="label">跌落检测</span><span class="val" id="s-cliff">-</span></div>
    <div class="row"><span class="label">雷达</span><span class="val" id="s-radar">-</span></div>
    <div class="row"><span class="label">动作</span><span class="val" id="s-act">-</span></div>
    <div class="row"><span class="label">视觉 FPS</span><span class="val" id="s-fps">-</span></div>
    <div class="row"><span class="label">识别状态</span><span class="val" id="s-state">-</span></div>
    <div class="row"><span class="label">人脸</span><span class="val" id="s-face">-</span></div>
  </div>

  <!-- 电机控制 -->
  <div class="card">
    <h2>&#x1F3AE; 电机控制</h2>
    <div class="ctrl">
      <div></div>
      <button class="btn" id="btn-fwd" onpointerdown="startCmd('forward')" onpointerup="stopCmd()" onpointerleave="stopCmd()">&#x2191;</button>
      <div></div>
      <button class="btn" id="btn-left" onpointerdown="startCmd('left')" onpointerup="stopCmd()" onpointerleave="stopCmd()">&#x2190;</button>
      <button class="btn stop" onpointerdown="stopCmd()" onpointerup="stopCmd()">&#x25A0;</button>
      <button class="btn" id="btn-right" onpointerdown="startCmd('right')" onpointerup="stopCmd()" onpointerleave="stopCmd()">&#x2192;</button>
      <div></div>
      <button class="btn" id="btn-bwd" onpointerdown="startCmd('backward')" onpointerup="stopCmd()" onpointerleave="stopCmd()">&#x2193;</button>
      <div></div>
    </div>
    <div class="speed-row">
      <span class="label">速度</span>
      <input type="range" min="0" max="255" value="220" id="spd" oninput="setSpeed(this.value)">
      <span class="val" id="spd-val">220</span>
    </div>
    <div style="margin-top:10px">
      <div class="row"><span class="label">键盘</span><span style="color:#8b949e">WASD / 方向键</span></div>
    </div>
  </div>

</div>
<script>
// ── 控制 ──────────────────────────────────────────────────────
let currentAct = 'stop';
let heartbeat = null;

function startCmd(act) {
  currentAct = act;
  fetch('/cmd?act=' + act).catch(()=>{});
  clearInterval(heartbeat);
  // ESP32 超时300ms，每150ms发一次心跳保持运动
  heartbeat = setInterval(() => {
    if (currentAct !== 'stop') fetch('/cmd?act=' + currentAct).catch(()=>{});
  }, 150);
}

function stopCmd() {
  currentAct = 'stop';
  clearInterval(heartbeat);
  heartbeat = null;
  fetch('/cmd?act=stop').catch(()=>{});
}

function setSpeed(v) {
  document.getElementById('spd-val').textContent = v;
  fetch('/cmd?act=speed:' + v).catch(()=>{});
}

// 键盘控制
const keyMap = {ArrowUp:'forward',KeyW:'forward',ArrowDown:'backward',KeyS:'backward',
                ArrowLeft:'left',KeyA:'left',ArrowRight:'right',KeyD:'right'};
const pressed = new Set();
document.addEventListener('keydown', e => {
  if (keyMap[e.code] && !pressed.has(e.code)) {
    pressed.add(e.code);
    startCmd(keyMap[e.code]);
  }
});
document.addEventListener('keyup', e => {
  if (keyMap[e.code]) {
    pressed.delete(e.code);
    if (pressed.size === 0) stopCmd();
  }
});

// ── 状态 SSE ─────────────────────────────────────────────────
const statusEs = new EventSource('/status');
statusEs.onmessage = e => {
  try {
    const d = JSON.parse(e.data);
    set('s-fps',   d.fps ?? '-');
    set('s-state', d.state ?? '-');
    if (d.face) {
      set('s-face', d.face.name + ' ' + d.face.emotion + ' ' + (d.face.score*100|0) + '%');
    } else {
      set('s-face', '无');
    }
    if (d.esp32) {
      const esp = d.esp32;
      setV('s-v',     (esp.v ?? '-') + ' V');
      setV('s-c',     esp.c != null ? (esp.c * 1000).toFixed(0) + ' mA' : '-');
      set('s-acc',    esp.ax != null ? `${esp.ax},${esp.ay},${esp.az} g` : '-');
      set('s-gyr',    esp.gx != null ? `${esp.gx},${esp.gy},${esp.gz} °/s` : '-');
      setWarn('s-cliff', esp.cliff ? '⚠ YES' : 'no', !!esp.cliff);
      setWarn('s-radar', esp.radar ? 'YES' : 'no', !!esp.radar);
      set('s-act',    esp.act ?? '-');
    }
  } catch(err) {}
};

// ── 眼睛 SSE + Canvas ────────────────────────────────────────
const canvas = document.getElementById('eye-canvas');
const ctx = canvas.getContext('2d');
let eyeX = 0, eyeY = 0, targetX = 0, targetY = 0;

const eyeEs = new EventSource('/eyes');
eyeEs.onopen = () => {
  document.getElementById('eye-dot').innerHTML = '<span class="dot on"></span>已连接';
};
eyeEs.onerror = () => {
  document.getElementById('eye-dot').innerHTML = '<span class="dot off"></span>断开';
};
eyeEs.onmessage = e => {
  try {
    const d = JSON.parse(e.data);
    targetX = d.x ?? 0;
    targetY = d.y ?? 0;
    if (d.emotion) set('emo', d.emotion);
  } catch(err) {}
};

function drawEyes() {
  const W = canvas.width = canvas.offsetWidth;
  const H = canvas.height = canvas.offsetHeight;
  ctx.clearRect(0, 0, W, H);

  // 平滑插值
  eyeX += (targetX - eyeX) * 0.15;
  eyeY += (targetY - eyeY) * 0.15;

  const cx1 = W * 0.3, cx2 = W * 0.7, cy = H * 0.5;
  const ew = 36, eh = 48, pupilR = 10, range = 12;
  const px = eyeX * range, py = eyeY * range;

  [[cx1, cy], [cx2, cy]].forEach(([cx, cy]) => {
    // 眼白
    ctx.save();
    ctx.beginPath();
    ctx.ellipse(cx, cy, ew, eh, 0, 0, Math.PI*2);
    ctx.fillStyle = '#00d7ff22';
    ctx.fill();
    ctx.strokeStyle = '#00d7ff88';
    ctx.lineWidth = 1.5;
    ctx.stroke();
    ctx.restore();
    // 瞳孔
    ctx.beginPath();
    ctx.arc(cx + px, cy + py, pupilR, 0, Math.PI*2);
    ctx.fillStyle = '#00d7ff';
    ctx.shadowColor = '#00d7ff';
    ctx.shadowBlur = 12;
    ctx.fill();
    ctx.shadowBlur = 0;
  });
  requestAnimationFrame(drawEyes);
}
drawEyes();

// ── 工具函数 ─────────────────────────────────────────────────
function set(id, v)          { const el=document.getElementById(id); if(el) el.textContent=v; }
function setV(id, v)         { const el=document.getElementById(id); if(el){el.textContent=v;el.className='val ok';} }
function setWarn(id, v, w)   { const el=document.getElementById(id); if(el){el.textContent=v;el.className='val '+(w?'warn':'ok');} }
</script>
</body>
</html>)html", "text/html");
        });

        encoder_thread = std::thread([this]() { VideoEncodeLoop(); });
        server_thread  = std::thread([this]() { svr.listen(AppConfig::kWebHost, AppConfig::kWebPort); });
    }

    ~WebServer() {
        running_ = false;
        video_raw_cv_.notify_all();
        svr.stop();
        if (encoder_thread.joinable()) encoder_thread.join();
        if (server_thread.joinable())  server_thread.join();
    }

private:
    static long long NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    static void ApplyNoCacheHeaders(httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
    }

    std::shared_ptr<const std::vector<uchar>> GetLatestEncodedFrame(uint64_t& seq) {
        std::lock_guard<std::mutex> lk(video_jpeg_mtx_);
        seq = latest_jpeg_seq_;
        return latest_jpeg_;
    }

    void VideoEncodeLoop() {
        uint64_t last_encoded_seq = 0;
        std::vector<int> params = {
            cv::IMWRITE_JPEG_QUALITY, AppConfig::kVideoJpegQuality,
            cv::IMWRITE_JPEG_OPTIMIZE, 1
        };
        while (running_) {
            cv::Mat frame;
            uint64_t raw_seq = 0;
            {
                std::unique_lock<std::mutex> lk(video_raw_mtx_);
                video_raw_cv_.wait_for(lk,
                    std::chrono::milliseconds(1000 / std::max(1, AppConfig::kVideoStreamFps)),
                    [this, last_encoded_seq]() {
                        return !running_ || latest_raw_seq_ != last_encoded_seq;
                    });
                if (!running_) break;
                if (latest_raw_frame_.empty() || latest_raw_seq_ == last_encoded_seq) continue;
                latest_raw_frame_.copyTo(frame);
                raw_seq = latest_raw_seq_;
            }

            // 缩放到流分辨率，减少编码耗时和带宽
            if (frame.cols > AppConfig::kStreamWidth || frame.rows > AppConfig::kStreamHeight) {
                cv::resize(frame, frame,
                    cv::Size(AppConfig::kStreamWidth, AppConfig::kStreamHeight),
                    0, 0, cv::INTER_LINEAR);
            }

            std::vector<uchar> jpeg;
            if (!cv::imencode(".jpg", frame, jpeg, params)) continue;
            {
                std::lock_guard<std::mutex> lk(video_jpeg_mtx_);
                latest_jpeg_     = std::make_shared<std::vector<uchar>>(std::move(jpeg));
                latest_jpeg_seq_ = raw_seq;
            }
            last_encoded_seq = raw_seq;
        }
    }
};

} // namespace mambo
