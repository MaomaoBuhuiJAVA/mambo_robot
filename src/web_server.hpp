#pragma once
#include "../third_party/httplib.h"
#include "../third_party/json.hpp"
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
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mambo {

using json = nlohmann::json;

class WebServer {
    httplib::Server svr;
    std::thread server_thread;
    std::thread encoder_thread;
    std::atomic<bool> running_{false};
    std::function<std::string(const std::string&)> backend_http_;
    std::function<void(const std::string&)> mute_cmd_;
    std::function<std::string(const std::string&)> speaker_volume_http_;
    std::function<std::string(const std::string&)> speaker_debug_http_;
    std::function<std::string(const std::string&)> voice_params_http_;
    std::function<std::string(const std::string&)> persona_config_http_;
    std::function<std::string(const std::string&)> typed_dialog_http_;
    std::function<std::string()> diag_baidu_deepseek_;
    std::function<std::string()> clear_memory_;
    std::function<std::string(const std::string&, const std::string&)> motion_mode_http_;
    std::function<std::string()> object_library_reset_;

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

    // 轻量 JSON 字段提取（仅用于简单 body 解析，避免额外依赖）
    static std::string ExtractJsonStringField(const std::string& body, const std::string& key) {
        size_t p = body.find("\"" + key + "\"");
        if (p == std::string::npos) return "";
        p = body.find(':', p);
        if (p == std::string::npos) return "";
        p = body.find('"', p);
        if (p == std::string::npos) return "";
        ++p;
        size_t q = body.find('"', p);
        if (q == std::string::npos) return "";
        return body.substr(p, q - p);
    }

    static int ExtractJsonIntField(const std::string& body, const std::string& key, int def) {
        size_t p = body.find("\"" + key + "\"");
        if (p == std::string::npos) return def;
        p = body.find(':', p);
        if (p == std::string::npos) return def;
        ++p;
        while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
        size_t end = p;
        while (end < body.size() && (body[end] == '-' || (body[end] >= '0' && body[end] <= '9'))) ++end;
        if (end == p) return def;
        return std::atoi(body.substr(p, end - p).c_str());
    }

public:
    void SetBackendHttpHandler(std::function<std::string(const std::string&)> cb) { backend_http_ = std::move(cb); }
    void SetMuteCommandHandler(std::function<void(const std::string&)> cb) { mute_cmd_ = std::move(cb); }
    void SetSpeakerVolumeHandler(std::function<std::string(const std::string&)> cb) { speaker_volume_http_ = std::move(cb); }
    void SetSpeakerDebugHandler(std::function<std::string(const std::string&)> cb) { speaker_debug_http_ = std::move(cb); }
    void SetVoiceParamsHandler(std::function<std::string(const std::string&)> cb) { voice_params_http_ = std::move(cb); }
    void SetPersonaConfigHandler(std::function<std::string(const std::string&)> cb) { persona_config_http_ = std::move(cb); }
    void SetTypedDialogHandler(std::function<std::string(const std::string&)> cb) { typed_dialog_http_ = std::move(cb); }
    void SetDiagBaiduDeepseekHandler(std::function<std::string()> cb) { diag_baidu_deepseek_ = std::move(cb); }
    void SetClearMemoryHandler(std::function<std::string()> cb) { clear_memory_ = std::move(cb); }
    void SetMotionModeHandler(std::function<std::string(const std::string&, const std::string&)> cb) { motion_mode_http_ = std::move(cb); }
    void SetObjectLibraryResetHandler(std::function<std::string()> cb) { object_library_reset_ = std::move(cb); }
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
        svr.set_mount_point("/alerts", "./data/alerts");

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
        // App 统一命名：状态流 SSE
        svr.Get("/api/v1/status/stream", [this](const httplib::Request&, httplib::Response& res) {
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
        // App 统一命名：眼睛状态流 SSE
        svr.Get("/api/v1/eyes/stream", [this](const httplib::Request&, httplib::Response& res) {
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
        // App 统一命名：状态快照
        svr.Get("/api/v1/status", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            std::string data;
            { std::lock_guard<std::mutex> lk(status_mtx_); data = status_data_.empty() ? "{}" : status_data_; }
            res.set_content(data, "application/json");
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

        // App 电机控制接口（支持边看视频边发控制）
        // GET  : /api/v1/control/motor?action=forward
        // POST : {"action":"forward","duration_ms":300}
        auto motor_control = [serial](const std::string& action, int duration_ms, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!serial) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"serial_not_ready\"}", "application/json");
                return;
            }
            if (action.empty()) {
                res.status = 400;
                res.set_content("{\"ok\":false,\"error\":\"empty_action\"}", "application/json");
                return;
            }
            serial->SendCommand(action);
            if (duration_ms > 0) {
                std::thread([serial, duration_ms]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
                    serial->SendCommand("stop");
                }).detach();
            }
            std::ostringstream oss;
            oss << "{\"ok\":true,\"action\":\"" << action << "\",\"duration_ms\":" << duration_ms << "}";
            res.set_content(oss.str(), "application/json");
        };
        svr.Get("/api/v1/control/motor", [motor_control](const httplib::Request& req, httplib::Response& res) {
            const std::string action = req.has_param("action")
                ? req.get_param_value("action")
                : (req.has_param("act") ? req.get_param_value("act") : "");
            int duration_ms = 0;
            if (req.has_param("duration_ms")) {
                duration_ms = std::max(0, std::atoi(req.get_param_value("duration_ms").c_str()));
            }
            motor_control(action, duration_ms, res);
        });
        svr.Post("/api/v1/control/motor", [motor_control](const httplib::Request& req, httplib::Response& res) {
            std::string action = ExtractJsonStringField(req.body, "action");
            if (action.empty()) action = ExtractJsonStringField(req.body, "act");
            int duration_ms = std::max(0, ExtractJsonIntField(req.body, "duration_ms", 0));
            motor_control(action, duration_ms, res);
        });
        // App 兼容：通用控制接口（与旧 /cmd 对齐）
        svr.Get("/api/v1/control/cmd", [handle_cmd](const httplib::Request& req, httplib::Response& res) {
            handle_cmd(req.has_param("act") ? req.get_param_value("act") : "", res);
        });
        svr.Post("/api/v1/control/cmd", [handle_cmd](const httplib::Request& req, httplib::Response& res) {
            std::string act = ExtractJsonStringField(req.body, "act");
            if (act.empty()) act = ExtractJsonStringField(req.body, "action");
            handle_cmd(act, res);
        });

        // App 运动模式接口：自动避障 / 跟随模式
        // GET  : /api/v1/control/mode?name=obstacle&value=on
        // POST : {"name":"follow","value":"toggle"}
        auto mode_control = [this](const std::string& name, const std::string& value, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!motion_mode_http_) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"no_mode_handler\"}", "application/json");
                return;
            }
            res.set_content(motion_mode_http_(name, value), "application/json");
        };
        svr.Get("/api/v1/control/mode", [mode_control](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.has_param("name") ? req.get_param_value("name") : "";
            std::string value = req.has_param("value") ? req.get_param_value("value") : "get";
            mode_control(name, value, res);
        });
        svr.Post("/api/v1/control/mode", [mode_control](const httplib::Request& req, httplib::Response& res) {
            std::string name = ExtractJsonStringField(req.body, "name");
            std::string value = ExtractJsonStringField(req.body, "value");
            if (value.empty()) value = "get";
            mode_control(name, value, res);
        });

        // App 全量状态接口：一次返回状态 + 情绪/识别 + ESP32 + 眼睛 + 视频元信息
        svr.Get("/api/v1/app/state", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            std::string status_json = "{}";
            std::string eye_json = "{\"x\":0,\"y\":0,\"emotion\":\"neutral\"}";
            {
                std::lock_guard<std::mutex> lk(status_mtx_);
                status_json = status_data_.empty() ? "{}" : status_data_;
            }
            {
                std::lock_guard<std::mutex> lk(eye_mtx_);
                eye_json = eye_data_.empty() ? "{\"x\":0,\"y\":0,\"emotion\":\"neutral\"}" : eye_data_;
            }

            int w = 0, h = 0;
            long long ts = 0;
            {
                std::lock_guard<std::mutex> lk(video_raw_mtx_);
                w = latest_frame_width_;
                h = latest_frame_height_;
                ts = latest_frame_ts_ms_;
            }
            uint64_t frame_id = 0;
            bool ready = false;
            {
                std::lock_guard<std::mutex> lk(video_jpeg_mtx_);
                ready = latest_jpeg_ && !latest_jpeg_->empty();
                frame_id = latest_jpeg_seq_;
            }

            std::ostringstream oss;
            oss << "{"
                << "\"ok\":true,"
                << "\"status\":" << status_json << ","
                << "\"eye\":" << eye_json << ","
                << "\"video\":{"
                << "\"ready\":" << (ready ? "true" : "false")
                << ",\"width\":" << w
                << ",\"height\":" << h
                << ",\"capture_fps\":" << AppConfig::kCameraTargetFps
                << ",\"stream_fps\":" << AppConfig::kVideoStreamFps
                << ",\"jpeg_quality\":" << AppConfig::kVideoJpegQuality
                << ",\"frame_id\":" << frame_id
                << ",\"timestamp_ms\":" << ts
                << "}"
                << "}";
            res.set_content(oss.str(), "application/json");
        });

        auto handle_backend = [this](const std::string& mode) -> std::string {
            if (!backend_http_) return "{\"ok\":false,\"error\":\"no_handler\"}";
            return backend_http_(mode);
        };
        svr.Get("/backend", [this, handle_backend](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!backend_http_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "";
            res.set_content(handle_backend(mode), "application/json");
        });
        svr.Post("/backend", [this, handle_backend](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!backend_http_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            std::string mode;
            if (!req.body.empty()) {
                size_t p = req.body.find("\"mode\"");
                if (p != std::string::npos) {
                    p = req.body.find(':', p);
                    if (p != std::string::npos) {
                        p = req.body.find('"', p);
                        if (p != std::string::npos) {
                            ++p;
                            size_t q = req.body.find('"', p);
                            if (q != std::string::npos) mode = req.body.substr(p, q - p);
                        }
                    }
                }
            }
            res.set_content(handle_backend(mode), "application/json");
        });
        // App 统一命名：后端查询/切换
        svr.Get("/api/v1/backend", [this, handle_backend](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!backend_http_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "";
            res.set_content(handle_backend(mode), "application/json");
        });
        svr.Post("/api/v1/backend", [this, handle_backend](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!backend_http_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            std::string mode = ExtractJsonStringField(req.body, "mode");
            res.set_content(handle_backend(mode), "application/json");
        });

        svr.Get("/mute", [this](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!mute_cmd_) { res.status = 503; res.set_content("{\"ok\":false}", "application/json"); return; }
            std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "";
            if (mode == "on" || mode == "1" || mode == "true") mute_cmd_("mute on");
            else if (mode == "off" || mode == "0" || mode == "false") mute_cmd_("mute off");
            else if (mode == "toggle" || mode == "t") mute_cmd_("mute");
            res.set_content("{\"ok\":true}", "application/json");
        });
        // App 统一命名：静音控制
        svr.Get("/api/v1/mute", [this](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!mute_cmd_) { res.status = 503; res.set_content("{\"ok\":false}", "application/json"); return; }
            std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "";
            if (mode == "on" || mode == "1" || mode == "true") mute_cmd_("mute on");
            else if (mode == "off" || mode == "0" || mode == "false") mute_cmd_("mute off");
            else if (mode == "toggle" || mode == "t") mute_cmd_("mute");
            res.set_content("{\"ok\":true}", "application/json");
        });
        svr.Post("/api/v1/mute", [this](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!mute_cmd_) { res.status = 503; res.set_content("{\"ok\":false}", "application/json"); return; }
            std::string mode = ExtractJsonStringField(req.body, "mode");
            if (mode == "on" || mode == "1" || mode == "true") mute_cmd_("mute on");
            else if (mode == "off" || mode == "0" || mode == "false") mute_cmd_("mute off");
            else if (mode == "toggle" || mode == "t") mute_cmd_("mute");
            res.set_content("{\"ok\":true}", "application/json");
        });
        // 扬声器音量控制：GET 查询，GET/POST 设置
        // GET  /api/v1/audio/volume
        // GET  /api/v1/audio/volume?value=65
        // POST /api/v1/audio/volume {"value":"65"}
        auto handle_volume = [this](const std::string& value, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!speaker_volume_http_) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json");
                return;
            }
            res.set_content(speaker_volume_http_(value), "application/json");
        };
        svr.Get("/api/v1/audio/volume", [handle_volume](const httplib::Request& req, httplib::Response& res) {
            std::string value = req.has_param("value") ? req.get_param_value("value") : "";
            handle_volume(value, res);
        });
        svr.Post("/api/v1/audio/volume", [this, handle_volume](const httplib::Request& req, httplib::Response& res) {
            std::string value = ExtractJsonStringField(req.body, "value");
            if (value.empty()) {
                int v = ExtractJsonIntField(req.body, "value", -1);
                if (v >= 0) value = std::to_string(v);
            }
            handle_volume(value, res);
        });
        // 音量调试接口：返回 sink 探测、命令执行与前后音量
        // GET  /api/v1/audio/debug
        // GET  /api/v1/audio/debug?value=35
        auto handle_volume_debug = [this](const std::string& value, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!speaker_debug_http_) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"no_debug_handler\"}", "application/json");
                return;
            }
            res.set_content(speaker_debug_http_(value), "application/json");
        };
        svr.Get("/api/v1/audio/debug", [handle_volume_debug](const httplib::Request& req, httplib::Response& res) {
            std::string value = req.has_param("value") ? req.get_param_value("value") : "";
            handle_volume_debug(value, res);
        });
        svr.Post("/api/v1/audio/debug", [this, handle_volume_debug](const httplib::Request& req, httplib::Response& res) {
            std::string value = ExtractJsonStringField(req.body, "value");
            if (value.empty()) {
                int v = ExtractJsonIntField(req.body, "value", -1);
                if (v >= 0) value = std::to_string(v);
            }
            handle_volume_debug(value, res);
        });

        // 麦克风唤醒阈值 / 静音结束：GET 查询；GET 带 query 或 POST JSON 更新
        // GET  /api/v1/audio/wake
        // GET  /api/v1/audio/wake?voice_threshold=350&silence_limit_ms=1200
        // POST /api/v1/audio/wake {"voice_threshold":350,"silence_limit_ms":1200,"silence_threshold":180}
        auto wake_body_from_req = [](const httplib::Request& req) -> std::string {
            if (!req.body.empty()) return req.body;
            bool any = false;
            std::string j = "{";
            auto add_int = [&](const char* key) {
                if (!req.has_param(key)) return;
                if (any) j += ",";
                any = true;
                j += "\"";
                j += key;
                j += "\":";
                j += req.get_param_value(key);
            };
            add_int("voice_threshold");
            add_int("silence_threshold");
            add_int("silence_limit_ms");
            if (!any) return "";
            j += "}";
            return j;
        };
        auto handle_wake = [this, wake_body_from_req](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!voice_params_http_) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json");
                return;
            }
            res.set_content(voice_params_http_(wake_body_from_req(req)), "application/json");
        };
        svr.Get("/api/v1/audio/wake", handle_wake);
        svr.Post("/api/v1/audio/wake", handle_wake);

        auto handle_persona = [this](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!persona_config_http_) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json");
                return;
            }
            res.set_content(persona_config_http_(req.body), "application/json");
        };
        svr.Get("/api/v1/dialog/persona", handle_persona);
        svr.Post("/api/v1/dialog/persona", handle_persona);

        svr.Get("/diag/baidu_deepseek", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!diag_baidu_deepseek_) { res.status = 503; res.set_content("{\"ok\":false}", "application/json"); return; }
            res.set_content(diag_baidu_deepseek_(), "application/json");
        });
        svr.Get("/api/v1/diag/baidu_deepseek", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!diag_baidu_deepseek_) { res.status = 503; res.set_content("{\"ok\":false}", "application/json"); return; }
            res.set_content(diag_baidu_deepseek_(), "application/json");
        });
        // 对话记忆清除
        svr.Get("/api/v1/memory/clear", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!clear_memory_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            res.set_content(clear_memory_(), "application/json");
        });
        svr.Post("/api/v1/memory/clear", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!clear_memory_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            res.set_content(clear_memory_(), "application/json");
        });
        svr.Post("/api/v1/vision/object_library/reset", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!object_library_reset_) { res.status = 503; res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json"); return; }
            res.set_content(object_library_reset_(), "application/json");
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
        // 优先加载 v3 设计稿，其次回退到 v2/旧版
        svr.Get("/dashboard", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            std::string html = ReadFile("./src/dashboard_v3.html");
            if (html.empty()) html = ReadFile("./src/dashboard_v2.html");
            if (html.empty()) html = ReadFile("./src/dashboard.html"); // 兼容旧文件
            if (html.empty()) {
                res.status = 503;
                res.set_content("dashboard.html not found", "text/plain");
                return;
            }
            res.set_header("Content-Type", "text/html; charset=utf-8");
            res.set_content(html, "text/html");
        });

        // ── 打字对话页 /chat ─────────────────────────────────────
        svr.Get("/chat", [this](const httplib::Request&, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            std::string html = ReadFile("./src/chat_typed.html");
            if (html.empty()) {
                res.status = 503;
                res.set_content("chat_typed.html not found", "text/plain");
                return;
            }
            res.set_header("Content-Type", "text/html; charset=utf-8");
            res.set_content(html, "text/html");
        });

        // Web 打字对话 API
        // POST /api/v1/dialog/text {"text":"你好","mode":"chat"|"echo","speak":true}
        auto handle_typed_dialog = [this](const httplib::Request& req, httplib::Response& res) {
            ApplyNoCacheHeaders(res);
            if (!typed_dialog_http_) {
                res.status = 503;
                res.set_content("{\"ok\":false,\"error\":\"no_handler\"}", "application/json");
                return;
            }
            std::string body = req.body;
            if (body.empty() && req.has_param("text")) {
                // 兼容 GET/POST query（调试用）
                json j;
                j["text"] = req.get_param_value("text");
                if (req.has_param("mode")) j["mode"] = req.get_param_value("mode");
                if (req.has_param("speak")) j["speak"] = (req.get_param_value("speak") != "0");
                body = j.dump();
            }
            res.set_content(typed_dialog_http_(body), "application/json");
        };
        svr.Post("/api/v1/dialog/text", handle_typed_dialog);
        svr.Get("/api/v1/dialog/text", handle_typed_dialog);

        encoder_thread = std::thread([this]() { VideoEncodeLoop(); });
        server_thread  = std::thread([this]() { svr.listen(AppConfig::kWebHost, AppConfig::kWebPort); });
    }

    void Stop() {
        running_ = false;
        video_raw_cv_.notify_all();
        svr.stop();
        if (encoder_thread.joinable()) encoder_thread.join();
        if (server_thread.joinable())  server_thread.join();
    }

    ~WebServer() { Stop(); }

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
