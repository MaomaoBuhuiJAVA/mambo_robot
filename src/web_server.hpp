#pragma once
#include "../third_party/httplib.h"
#include "config.hpp"
#include "utils.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <sstream>
#include <thread>
#include <mutex>
#include <string>
#include <functional>
#include <vector>

namespace mambo {

class WebServer {
    httplib::Server svr;
    std::thread server_thread;
    std::thread encoder_thread;
    std::atomic<bool> running_{false};

    std::mutex sse_mtx_;
    std::string latest_data_ = "{\"x\":0,\"y\":0,\"emotion\":\"neutral\"}";

    std::mutex video_raw_mtx_;
    std::condition_variable video_raw_cv_;
    cv::Mat latest_raw_frame_;
    uint64_t latest_raw_seq_ = 0;

    std::mutex video_jpeg_mtx_;
    std::shared_ptr<const std::vector<uchar>> latest_jpeg_;
    uint64_t latest_jpeg_seq_ = 0;
    int latest_frame_width_ = 0;
    int latest_frame_height_ = 0;
    long long latest_frame_ts_ms_ = 0;

public:
    // C++ 端调用这个推送最新数据
    void PushEyeData(float norm_x, float norm_y, const std::string& emotion) {
        std::lock_guard<std::mutex> lk(sse_mtx_);
        latest_data_ = "{\"x\":" + std::to_string(norm_x)
                     + ",\"y\":" + std::to_string(norm_y)
                     + ",\"emotion\":\"" + emotion + "\"}";
    }

    void PushVideoFrame(const cv::Mat& frame) {
        if (frame.empty()) return;
        {
            std::lock_guard<std::mutex> lk(video_raw_mtx_);
            frame.copyTo(latest_raw_frame_);
            latest_frame_width_ = frame.cols;
            latest_frame_height_ = frame.rows;
            latest_frame_ts_ms_ = NowMs();
            ++latest_raw_seq_;
        }
        video_raw_cv_.notify_one();
    }

    void Start(SerialManager* serial) {
        running_ = true;

        // serve eyes-ui 静态文件
        svr.set_mount_point("/", "./eyes-ui/dist");

        // 控制台页面（移到 /control）
        svr.Get("/control", [](const httplib::Request&, httplib::Response& res) {
            std::string html =
                "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                "<style>button{width:100px;height:100px;font-size:24px;margin:10px;border-radius:20px;background:#00d7ff;border:none;}</style>"
                "</head><body style=\"text-align:center; background:#222; color:white; padding-top:50px;\">"
                "<h2>Mambo</h2>"
                "<div><button onclick=\"fetch('/cmd?act=forward')\">FWD</button></div>"
                "<div>"
                "<button onclick=\"fetch('/cmd?act=left')\">LEFT</button>"
                "<button onclick=\"fetch('/cmd?act=stop')\" style=\"background:#ff4444;\">STOP</button>"
                "<button onclick=\"fetch('/cmd?act=right')\">RIGHT</button>"
                "</div>"
                "<div><button onclick=\"fetch('/cmd?act=backward')\">BACK</button></div>"
                "</body></html>";
            res.set_content(html, "text/html");
        });

        svr.Get("/cmd", [serial](const httplib::Request& req, httplib::Response& res) {
            if (req.has_param("act") && serial) serial->SendCommand(req.get_param_value("act"));
            res.set_content("OK", "text/plain");
        });

        // SSE 端点：浏览器长连接，持续接收眼睛数据
        svr.Get("/eyes", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("text/event-stream",
                [this](size_t /*offset*/, httplib::DataSink& sink) {
                    while (running_) {
                        std::string data;
                        { std::lock_guard<std::mutex> lk(sse_mtx_); data = latest_data_; }
                        std::string msg = "data: " + data + "\n\n";
                        if (!sink.write(msg.c_str(), msg.size())) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
                    }
                    return true;
                });
        });

        // 单帧抓图接口，适合调试、巡检和截图
        svr.Get("/api/v1/video/frame", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            uint64_t seq = 0;
            auto frame = GetLatestEncodedFrame(seq);
            if (!frame) {
                res.status = 503;
                res.set_content("video stream is not ready", "text/plain");
                return;
            }
            res.set_header("X-Frame-Id", std::to_string(seq));
            res.set_content(reinterpret_cast<const char*>(frame->data()), frame->size(), "image/jpeg");
        });

        // MJPEG 实时流，兼容浏览器、OpenCV、VLC、上位机
        svr.Get("/api/v1/video/stream", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=frame",
                [this](size_t /*offset*/, httplib::DataSink& sink) {
                    uint64_t last_sent_seq = 0;
                    const auto frame_interval = std::chrono::milliseconds(1000 / std::max(1, AppConfig::kVideoStreamFps));
                    while (running_) {
                        uint64_t seq = 0;
                        auto frame = GetLatestEncodedFrame(seq);
                        if (!frame || frame->empty()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            continue;
                        }
                        if (seq == last_sent_seq) {
                            std::this_thread::sleep_for(frame_interval);
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

        // 元信息接口，便于接入方做能力探测
        svr.Get("/api/v1/video/meta", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            int width = 0;
            int height = 0;
            long long timestamp_ms = 0;
            {
                std::lock_guard<std::mutex> lk(video_raw_mtx_);
                width = latest_frame_width_;
                height = latest_frame_height_;
                timestamp_ms = latest_frame_ts_ms_;
            }

            uint64_t seq = 0;
            bool ready = false;
            {
                std::lock_guard<std::mutex> lk(video_jpeg_mtx_);
                ready = latest_jpeg_ && !latest_jpeg_->empty();
                seq = latest_jpeg_seq_;
            }

            std::ostringstream oss;
            oss << "{"
                << "\"ready\":" << (ready ? "true" : "false")
                << ",\"width\":" << width
                << ",\"height\":" << height
                << ",\"capture_fps\":" << AppConfig::kCameraTargetFps
                << ",\"stream_fps\":" << AppConfig::kVideoStreamFps
                << ",\"jpeg_quality\":" << AppConfig::kVideoJpegQuality
                << ",\"frame_id\":" << seq
                << ",\"timestamp_ms\":" << timestamp_ms
                << "}";
            res.set_content(oss.str(), "application/json");
        });

        encoder_thread = std::thread([this]() { VideoEncodeLoop(); });
        server_thread = std::thread([this]() { svr.listen(AppConfig::kWebHost, AppConfig::kWebPort); });
    }

    ~WebServer() {
        running_ = false;
        video_raw_cv_.notify_all();
        svr.stop();
        if (encoder_thread.joinable()) encoder_thread.join();
        if (server_thread.joinable()) server_thread.join();
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
        std::vector<int> encode_params = {
            cv::IMWRITE_JPEG_QUALITY, AppConfig::kVideoJpegQuality,
            cv::IMWRITE_JPEG_OPTIMIZE, 1
        };

        while (running_) {
            cv::Mat frame;
            uint64_t raw_seq = 0;
            {
                std::unique_lock<std::mutex> lk(video_raw_mtx_);
                video_raw_cv_.wait_for(
                    lk,
                    std::chrono::milliseconds(1000 / std::max(1, AppConfig::kVideoStreamFps)),
                    [this, last_encoded_seq]() { return !running_ || latest_raw_seq_ != last_encoded_seq; });
                if (!running_) break;
                if (latest_raw_frame_.empty() || latest_raw_seq_ == last_encoded_seq) continue;
                latest_raw_frame_.copyTo(frame);
                raw_seq = latest_raw_seq_;
            }

            std::vector<uchar> jpeg;
            if (!cv::imencode(".jpg", frame, jpeg, encode_params)) continue;

            {
                std::lock_guard<std::mutex> lk(video_jpeg_mtx_);
                latest_jpeg_ = std::make_shared<std::vector<uchar>>(std::move(jpeg));
                latest_jpeg_seq_ = raw_seq;
            }
            last_encoded_seq = raw_seq;
        }
    }
};

} // namespace mambo
