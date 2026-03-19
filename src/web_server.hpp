#pragma once
#include "../third_party/httplib.h"
#include "utils.hpp"
#include <thread>
#include <mutex>
#include <string>
#include <functional>

namespace mambo {

class WebServer {
    httplib::Server svr;
    std::thread server_thread;

    std::mutex sse_mtx_;
    std::string latest_data_ = "{\"x\":0,\"y\":0,\"emotion\":\"neutral\"}";

public:
    // C++ 端调用这个推送最新数据
    void PushEyeData(float norm_x, float norm_y, const std::string& emotion) {
        std::lock_guard<std::mutex> lk(sse_mtx_);
        latest_data_ = "{\"x\":" + std::to_string(norm_x)
                     + ",\"y\":" + std::to_string(norm_y)
                     + ",\"emotion\":\"" + emotion + "\"}";
    }

    void Start(SerialManager* serial) {
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
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_chunked_content_provider("text/event-stream",
                [this](size_t /*offset*/, httplib::DataSink& sink) {
                    while (true) {
                        std::string data;
                        { std::lock_guard<std::mutex> lk(sse_mtx_); data = latest_data_; }
                        std::string msg = "data: " + data + "\n\n";
                        if (!sink.write(msg.c_str(), msg.size())) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
                    }
                    return true;
                });
        });

        server_thread = std::thread([this]() { svr.listen("0.0.0.0", 8080); });
    }

    ~WebServer() { svr.stop(); if (server_thread.joinable()) server_thread.join(); }
};

} // namespace mambo
