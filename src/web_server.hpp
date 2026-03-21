#pragma once
#include "../third_party/httplib.h"
#include "config.hpp"
#include "utils.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
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

    std::mutex eye_mtx_;
    std::condition_variable eye_cv_;
    std::string eye_data_    = "{\"x\":0,\"y\":0,\"emotion\":\"neutral\"}";
    std::string eye_emotion_ = "ZhongXing";
    uint64_t    eye_seq_     = 0;

    std::mutex status_mtx_;
    std::string status_data_ = "{}";

    std::mutex              video_raw_mtx_;
    std::condition_variable video_raw_cv_;
    cv::Mat                 latest_raw_frame_;
    uint64_t                latest_raw_seq_      = 0;
    int                     latest_frame_width_  = 0;
    int                     latest_frame_height_ = 0;
    long long               latest_frame_ts_ms_  = 0;

    std::mutex                                video_jpeg_mtx_;
    std::shared_ptr<const std::vector<uchar>> latest_jpeg_;
    uint64_t                                  latest_jpeg_seq_ = 0;

    // 读取文件内容（用于 dashboard HTML）
    static std::string ReadFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "";
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

public:
    void PushEyeData(float nx, float ny, const std::string& emotion) {
        { std::lock_guard<std::mutex> lk(eye_mtx_);
          eye_emotion_ = emotion;
          eye_data_ = "{\"x\":" + std::to_string(nx) + ",\"y\":" + std::to_string(ny)
                    + ",\"emotion\":\"" + emotion + "\"}";
          ++eye_seq_; }
        eye_cv_.notify_all();
    }

    void PushEyePos(float nx, float ny) {
        { std::lock_guard<std::mutex> lk(eye_mtx_);
          eye_data_ = "{\"x\":" + std::to_string(nx) + ",\"y\":" + std::to_string(ny)
                    + ",\"emotion\":\"" + eye_emotion_ + "\"}";
          ++eye_seq_; }
        eye_cv_.notify_all();
    }

    void PushVideoFrame(const cv::Mat& frame) {
        if (frame.empty()) return;
        { std::lock_guard<std::mutex> lk(video_raw_mtx_);
          frame.copyTo(latest_raw_frame_);
          latest_frame_width_  = frame.cols;
          latest_frame_height_ = frame.rows;
          latest_frame_ts_ms_  = NowMs();
          ++latest_raw_seq_; }
        video_raw_cv_.notify_one();
    }

    void PushStatus(const std::string& json) {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_data_ = json;
    }

    void Start(SerialManager* serial) {
        running_ = true;
        svr.set_mount_point("/", "./eyes-ui/dist");

        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_content("", "text/plain");
        });

        // /eyes SSE
        svr.Get("/eyes", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            uint64_t last_seq = 0;
            res.set_chunked_content_provider("text/event-stream",
                [this, last_seq](size_t, httplib::DataSink& sink) mutable {
                    std::string data;
                    { std::unique_lock<std::mutex> lk(eye_mtx_);
                      eye_cv_.wait_for(lk, std::chrono::milliseconds(100),
                          [this, &last_seq]() { return !running_ || eye_seq_ != last_seq; });
                      if (!running_) return false;
                      data = eye_data_; last_seq = eye_seq_; }
                    std::string msg = "data: " + data + "\n\n";
                    return sink.write(msg.c_str(), msg.size());
                });
        });

        // /status SSE
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

        // MJPEG 流（共用 handler）
        auto mjpeg_handler = [this](size_t, httplib::DataSink& sink) {
            uint64_t last_sent_seq = 0;
            const auto interval = std::chrono::milliseconds(1000 / std::max(1, AppConfig::kVideoStreamFps));
            while (running_) {
                uint64_t seq = 0;
                auto frame = GetLatestEncodedFrame(seq);
                if (!frame || frame->empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
                if (seq == last_sent_seq) { std::this_thread::sleep_for(interval); continue; }
                std::string hdr = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    + std::to_string(frame->size()) + "\r\nX-Frame-Id: " + std::to_string(seq) + "\r\n\r\n";
                if (!sink.write(hdr.c_str(), hdr.size())) return false;
                if (!sink.write(reinterpret_cast<const char*>(frame->data()), frame->size())) return false;
                if (!sink.write("\r\n", 2)) return false;
                last_sent_seq = seq;
            }
            return true;
        };
        svr.Get("/api/v1/video/stream", [this, mjpeg_handler](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=frame", mjpeg_handler);
        });
        svr.Get("/stream", [this, mjpeg_handler](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=frame", mjpeg_handler);
        });

        // 单帧
        svr.Get("/api/v1/video/frame", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            uint64_t seq = 0;
            auto frame = GetLatestEncodedFrame(seq);
            if (!frame) { res.status = 503; res.set_content("not ready", "text/plain"); return; }
            res.set_header("X-Frame-Id", std::to_string(seq));
            res.set_content(reinterpret_cast<const char*>(frame->data()), frame->size(), "image/jpeg");
        });

        // 视频元信息
        svr.Get("/api/v1/video/meta", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            int w=0,h=0; long long ts=0;
            { std::lock_guard<std::mutex> lk(video_raw_mtx_); w=latest_frame_width_; h=latest_frame_height_; ts=latest_frame_ts_ms_; }
            uint64_t seq=0; bool ready=false;
            { std::lock_guard<std::mutex> lk(video_jpeg_mtx_); ready=latest_jpeg_&&!latest_jpeg_->empty(); seq=latest_jpeg_seq_; }
            std::ostringstream oss;
            oss<<"{\"ready\":"<<(ready?"true":"false")<<",\"width\":"<<w<<",\"height\":"<<h
               <<",\"capture_fps\":"<<AppConfig::kCameraTargetFps<<",\"stream_fps\":"<<AppConfig::kVideoStreamFps
               <<",\"jpeg_quality\":"<<AppConfig::kVideoJpegQuality<<",\"frame_id\":"<<seq<<",\"timestamp_ms\":"<<ts<<"}";
            res.set_content(oss.str(), "application/json");
        });

        // 控制指令
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
                auto q1 = req.body.find('"', pos+5);
                if (q1 != std::string::npos) { auto q2 = req.body.find('"', q1+1); if (q2 != std::string::npos) act = req.body.substr(q1+1, q2-q1-1); }
            }
            handle_cmd(act, res);
        });

        // 简易控制页
        svr.Get("/control", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(
                "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>button{width:100px;height:100px;font-size:24px;margin:10px;border-radius:20px;background:#00d7ff;border:none;}</style></head>"
                "<body style='text-align:center;background:#222;color:white;padding-top:50px'>"
                "<h2>Mambo</h2>"
                "<div><button onclick=\"fetch('/cmd?act=forward')\">FWD</button></div>"
                "<div><button onclick=\"fetch('/cmd?act=left')\">LEFT</button>"
                "<button onclick=\"fetch('/cmd?act=stop')\" style='background:#ff4444'>STOP</button>"
                "<button onclick=\"fetch('/cmd?act=right')\">RIGHT</button></div>"
                "<div><button onclick=\"fetch('/cmd?act=backward')\">BACK</button></div>"
                "<p><a href='/dashboard' style='color:#0f0'>超级控制台</a></p>"
                "</body></html>", "text/html");
        });

        // ── 超级控制台 /dashboard ─────────────────────────────────
        // 从 src/dashboard.html 读取（运行时路径为 ./src/dashboard.html）
        svr.Get("/dashboard", [](const httplib::Request&, httplib::Response& res) {
            std::string html = ReadFile("./src/dashboard.html");
            if (html.empty()) {
                res.status = 503;
                res.set_content("dashboard.html not found", "text/plain");
                return;
            }
            res.set_header("Content-Type", "text/html; charset=utf-8");
            res.set_content(html, "text/html");
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
            { std::unique_lock<std::mutex> lk(video_raw_mtx_);
              video_raw_cv_.wait_for(lk,
                  std::chrono::milliseconds(1000 / std::max(1, AppConfig::kVideoStreamFps)),
                  [this, last_encoded_seq]() { return !running_ || latest_raw_seq_ != last_encoded_seq; });
              if (!running_) break;
              if (latest_raw_frame_.empty() || latest_raw_seq_ == last_encoded_seq) continue;
              latest_raw_frame_.copyTo(frame);
              raw_seq = latest_raw_seq_; }

            if (frame.cols > AppConfig::kStreamWidth || frame.rows > AppConfig::kStreamHeight)
                cv::resize(frame, frame, cv::Size(AppConfig::kStreamWidth, AppConfig::kStreamHeight), 0, 0, cv::INTER_LINEAR);

            std::vector<uchar> jpeg;
            if (!cv::imencode(".jpg", frame, jpeg, params)) continue;
            { std::lock_guard<std::mutex> lk(video_jpeg_mtx_);
              latest_jpeg_     = std::make_shared<std::vector<uchar>>(std::move(jpeg));
              latest_jpeg_seq_ = raw_seq; }
            last_encoded_seq = raw_seq;
        }
    }
};

} // namespace mambo
