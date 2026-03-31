#include "../third_party/httplib.h"
#include "config.hpp"
#include "vision_engine.hpp"

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

} // namespace

int main() {
    using namespace mambo;

    std::atomic<bool> running{true};

    // 摄像头
    cv::VideoCapture cap(AppConfig::kCameraIndex);
    if (!cap.isOpened()) {
        std::cerr << "[TestVision] cannot open camera index=" << AppConfig::kCameraIndex << "\n";
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  AppConfig::kCameraWidth);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, AppConfig::kCameraHeight);
    cap.set(cv::CAP_PROP_FPS,          AppConfig::kCameraTargetFps);

    VisionEngine vision;

    // 共享：最新 JPEG + 最新 status JSON
    std::mutex mtx;
    std::condition_variable cv;
    std::shared_ptr<std::vector<uchar>> latest_jpeg;
    uint64_t latest_seq = 0;
    std::string latest_status = "{}";

    // 采集+推理线程
    std::thread worker([&]() {
        int frame_count = 0;
        int fps_cnt = 0;
        int fps_val = 0;
        auto t0 = std::chrono::steady_clock::now();

        while (running.load()) {
            cv::Mat frame;
            cap >> frame;
            if (frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }

            frame_count++;
            fps_cnt++;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1000) {
                fps_val = fps_cnt;
                fps_cnt = 0;
                t0 = std::chrono::steady_clock::now();
            }

            std::vector<ObjectResult> objects;
            std::vector<FaceResult> faces;

            // 这里直接高频跑完整人脸识别+情绪（用于测试）
            vision.ProcessFrame(frame, objects, faces, frame_count);

            // 叠框显示
            for (const auto& f : faces) {
                cv::rectangle(frame, f.box, cv::Scalar(0, 255, 255), 2);
                std::ostringstream label;
                label << f.name << " | " << f.emotion << " | " << std::fixed << std::setprecision(2) << f.score;
                cv::putText(frame, label.str(), cv::Point(f.box.x, std::max(16, f.box.y - 6)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 2);
            }

            // 状态 JSON（给网页列表）
            std::ostringstream st;
            st << "{"
               << "\"ok\":true,"
               << "\"ts_ms\":" << NowMs() << ","
               << "\"fps\":" << fps_val << ","
               << "\"faces\":[";
            for (size_t i = 0; i < faces.size(); i++) {
                if (i) st << ",";
                const auto& f = faces[i];
                st << "{"
                   << "\"name\":\"" << JsonEscape(f.name) << "\","
                   << "\"emotion\":\"" << JsonEscape(f.emotion) << "\","
                   << "\"score\":" << std::fixed << std::setprecision(2) << f.score << ","
                   << "\"box\":{"
                      << "\"x\":" << f.box.x << ",\"y\":" << f.box.y << ",\"w\":" << f.box.width << ",\"h\":" << f.box.height
                   << "}"
                   << "}";
            }
            st << "]"
               << "}";

            // 编码 JPEG
            auto jpg = std::make_shared<std::vector<uchar>>();
            try {
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, AppConfig::kVideoJpegQuality};
                cv::imencode(".jpg", frame, *jpg, params);
            } catch (...) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(mtx);
                latest_jpeg = jpg;
                latest_seq++;
                latest_status = st.str();
            }
            cv.notify_all();
        }
    });

    httplib::Server svr;

    // 简单页面
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Test Vision Console</title>
  <style>
    body{margin:0;background:#0b0c1f;color:#e4e3fe;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;}
    .wrap{display:grid;grid-template-columns:1fr 360px;gap:16px;padding:16px;height:100vh;box-sizing:border-box;}
    .card{background:rgba(34,35,62,.45);border:1px solid rgba(228,227,254,.08);border-radius:16px;overflow:hidden;}
    .video{position:relative;height:100%;}
    .video img{width:100%;height:100%;object-fit:contain;background:#000;display:block;}
    .badge{position:absolute;left:12px;top:12px;background:rgba(0,0,0,.55);border:1px solid rgba(255,255,255,.12);padding:6px 10px;border-radius:999px;font-size:12px;}
    .side{padding:12px;display:flex;flex-direction:column;gap:10px;}
    .row{border-bottom:1px dashed rgba(255,255,255,.12);padding:8px 0;}
    .k{color:#a9a9c2;font-size:12px;}
    .v{font-weight:700;}
    .pill{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid rgba(253,216,53,.4);background:rgba(253,216,53,.16);color:#fdd835;font-size:12px;margin-left:8px;}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card video">
      <img id="feed" src="/video/stream" alt="stream"/>
      <div class="badge">LIVE · <span id="fps">0</span> FPS</div>
    </div>
    <div class="card side">
      <div class="k">人脸识别结果 <span class="pill" id="face-count">0</span></div>
      <div id="list"></div>
    </div>
  </div>
  <script>
    const fpsEl = document.getElementById('fps');
    const listEl = document.getElementById('list');
    const cntEl = document.getElementById('face-count');
    const es = new EventSource('/status/stream');
    es.onmessage = (e) => {
      let d = null;
      try { d = JSON.parse(e.data); } catch (_) { return; }
      fpsEl.textContent = String(d.fps ?? 0);
      const faces = Array.isArray(d.faces) ? d.faces : [];
      cntEl.textContent = String(faces.length);
      listEl.innerHTML = faces.map(f => (
        '<div class="row">' +
          '<div class="v">' + (f.name || '-') + '</div>' +
          '<div class="k">emotion: ' + (f.emotion || '-') + ' · score: ' + (f.score ?? '-') + '</div>' +
        '</div>'
      )).join('') || '<div class="k">暂无人脸</div>';
    };
  </script>
</body>
</html>
)HTML", "text/html; charset=utf-8");
    });

    // MJPEG
    svr.Get("/video/stream", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=frame",
            [&](size_t, httplib::DataSink& sink) {
                uint64_t last = 0;
                while (running.load()) {
                    std::shared_ptr<std::vector<uchar>> jpg;
                    uint64_t seq = 0;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait_for(lk, std::chrono::milliseconds(200), [&]() { return !running.load() || latest_seq != last; });
                        if (!running.load()) return false;
                        jpg = latest_jpeg;
                        seq = latest_seq;
                    }
                    if (!jpg || jpg->empty() || seq == last) continue;
                    last = seq;
                    std::string hdr = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpg->size()) + "\r\n\r\n";
                    if (!sink.write(hdr.c_str(), hdr.size())) return false;
                    if (!sink.write(reinterpret_cast<const char*>(jpg->data()), jpg->size())) return false;
                    if (!sink.write("\r\n", 2)) return false;
                }
                return true;
            });
    });

    // SSE status
    svr.Get("/status/stream", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider("text/event-stream",
            [&](size_t, httplib::DataSink& sink) {
                std::string data;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    data = latest_status;
                }
                std::string msg = "data: " + data + "\n\n";
                if (!sink.write(msg.c_str(), msg.size())) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return running.load();
            });
    });

    std::cerr << "[TestVision] open: http://" << AppConfig::kWebHost << ":" << AppConfig::kWebPort << "/\n";
    svr.listen(AppConfig::kWebHost, AppConfig::kWebPort);

    running = false;
    cv.notify_all();
    if (worker.joinable()) worker.join();
    return 0;
}

