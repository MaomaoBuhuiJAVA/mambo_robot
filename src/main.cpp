#include "config.hpp"
#include "utils.hpp"
#include "web_server.hpp"
#include "vision_engine.hpp"
#include "dialog_system.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <cstdio>

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    mambo::SerialManager serial(mambo::AppConfig::kSerialPort);
    mambo::WebServer web;
    web.Start(&serial);

    mambo::VisionEngine vision;
    mambo::DialogSystem dialog(&serial);
    dialog.Start();

    // 摄像头初始化
    cv::VideoCapture cap(mambo::AppConfig::kCameraIndex);
    if (!cap.isOpened()) {
        std::cerr << "[Error] 无法打开摄像头 index=" << mambo::AppConfig::kCameraIndex << std::endl;
        curl_global_cleanup();
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  mambo::AppConfig::kCameraWidth);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, mambo::AppConfig::kCameraHeight);
    cap.set(cv::CAP_PROP_FPS,          mambo::AppConfig::kCameraTargetFps);

    std::mutex data_mtx;
    cv::Rect   fast_box;
    bool       has_face         = false;
    int        latest_frame_w   = mambo::AppConfig::kCameraWidth;
    int        latest_frame_h   = mambo::AppConfig::kCameraHeight;
    std::vector<mambo::ObjectResult> slow_objects;
    std::vector<mambo::FaceResult>   slow_faces;
    std::atomic<bool> running(true);
    std::atomic<int>  det_fps(0);

    // 视觉线程：每帧检测 + 推送视频流，每15帧完整推理
    std::thread vision_thread([&]() {
        int frame_count = 0, cnt = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (running) {
            cv::Mat frame;
            cap >> frame;
            if (frame.empty()) continue;
            frame_count++;

            web.PushVideoFrame(frame); // 异步编码，不阻塞

            auto boxes = vision.DetectFaceBoxes(frame);

            std::vector<mambo::ObjectResult> objects;
            std::vector<mambo::FaceResult>   faces;
            if (frame_count % 15 == 0)
                vision.ProcessFrame(frame, objects, faces, frame_count);

            {
                std::lock_guard<std::mutex> lk(data_mtx);
                has_face       = !boxes.empty();
                fast_box       = has_face ? boxes[0] : cv::Rect();
                latest_frame_w = frame.cols;
                latest_frame_h = frame.rows;
                if (frame_count % 15 == 0) {
                    slow_objects = objects;
                    slow_faces   = faces;
                }
            }

            cnt++;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1000) { det_fps = cnt; cnt = 0; t0 = std::chrono::steady_clock::now(); }
        }
    });

    // 主线程：推送数据 + 控制台打印
    float smooth_cx = mambo::AppConfig::kCameraWidth  / 2.0f;
    float smooth_cy = mambo::AppConfig::kCameraHeight / 2.0f;
    auto last_print = std::chrono::steady_clock::now();

    while (true) {
        cv::Rect box; bool hf; int fw, fh;
        std::vector<mambo::ObjectResult> objects;
        std::vector<mambo::FaceResult>   faces;
        {
            std::lock_guard<std::mutex> lk(data_mtx);
            box     = fast_box;
            hf      = has_face;
            objects = slow_objects;
            faces   = slow_faces;
            fw      = latest_frame_w;
            fh      = latest_frame_h;
        }

        mambo::ChatState state = dialog.GetState();
        std::string emo = "ZhongXing";

        if (hf) {
            smooth_cx = box.x + box.width  / 2.0f;
            smooth_cy = box.y + box.height / 2.0f;
        }
        if (!faces.empty()) {
            emo = faces[0].emotion;
            dialog.SetCurrentEmotion(emo);
        }

        // 推送眼睛UI（用实际分辨率归一化，负号修正镜像）
        float half_w = std::max(1.0f, fw / 2.0f);
        float half_h = std::max(1.0f, fh / 2.0f);
        float nx = -(smooth_cx - half_w) / half_w;
        float ny =  (smooth_cy - half_h) / half_h;
        web.PushEyeData(nx, ny, emo);

        // 轮询串口接收 ESP32 上报
        serial.Poll();

        // 每秒构建 status JSON + 控制台打印
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print).count() >= 1000) {
            last_print = now;
            std::string ss = (state == mambo::ChatState::kWaiting  ? "waiting"   :
                              state == mambo::ChatState::kListening ? "listening" :
                              state == mambo::ChatState::kThinking  ? "thinking"  : "speaking");

            // 构建 status JSON
            std::string json = "{";
            json += "\"fps\":" + std::to_string(det_fps.load());
            json += ",\"state\":\"" + ss + "\"";
            if (!faces.empty()) {
                char buf[128];
                snprintf(buf, sizeof(buf), ",\"face\":{\"name\":\"%s\",\"emotion\":\"%s\",\"score\":%.2f}",
                         faces[0].name.c_str(), faces[0].emotion.c_str(), faces[0].score);
                json += buf;
            } else {
                json += ",\"face\":null";
            }
            json += ",\"objects\":[";
            for (size_t i = 0; i < objects.size(); i++) {
                if (i) json += ",";
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"label\":\"%s\",\"prob\":%.2f}",
                         mambo::kClassNames[objects[i].label].c_str(), objects[i].prob);
                json += buf;
            }
            json += "]";
            std::string espStatus = serial.GetEsp32Status();
            if (!espStatus.empty()) json += ",\"esp32\":{" + espStatus + "}";
            json += "}";
            web.PushStatus(json);

            // 控制台打印 ── 香橙派
            std::cout << "\n┌─ 视觉 FPS:" << det_fps << "  状态:" << ss;
            if (!faces.empty())
                std::cout << "  人脸:[" << faces[0].name << "|" << faces[0].emotion
                          << "|" << std::fixed << std::setprecision(2) << faces[0].score << "]";
            else
                std::cout << "  人脸:无";
            if (!objects.empty()) {
                std::cout << "  物品:";
                for (const auto& o : objects)
                    std::cout << mambo::kClassNames[o.label] << "(" << (int)(o.prob*100) << "%) ";
            }

            // 控制台打印 ── ESP32
            auto esp = serial.GetEsp32Data();
            if (esp.valid) {
                std::cout << std::fixed << std::setprecision(2);
                std::cout << "\n└─ ESP32  "
                          << "电压:" << esp.v << "V  "
                          << "电流:" << esp.c * 1000 << "mA  "
                          << "加速度:[" << esp.ax << "," << esp.ay << "," << esp.az << "]g  "
                          << "陀螺:[" << std::setprecision(1) << esp.gx << "," << esp.gy << "," << esp.gz << "]°/s  "
                          << "跌落:" << (esp.cliff ? "⚠ YES" : "no") << "  "
                          << "雷达:" << (esp.radar ? "YES" : "no") << "  "
                          << "动作:" << esp.act;
            } else {
                std::cout << "\n└─ ESP32  等待连接...";
            }
            std::cout << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    running = false;
    vision_thread.join();
    curl_global_cleanup();
    return 0;
}
