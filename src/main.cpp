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
    mambo::DialogSystem dialog(&serial, &web);
    dialog.Start();

    // 警报 TTS 播放（异步，不阻塞主循环）
    auto playAlert = [](const std::string& text) {
        std::thread([text]() {
            mambo::TtsPlay(text, mambo::AppConfig::kAlsaPlayDevice);
        }).detach();
    };

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

    // 共享帧（摄像头线程写，推理线程读）
    std::mutex              cap_mtx;
    cv::Mat                 shared_frame;
    uint64_t                shared_seq = 0;
    std::condition_variable cap_cv;

    std::mutex data_mtx;
    cv::Rect   fast_box;
    bool       has_face         = false;
    int        latest_frame_w   = mambo::AppConfig::kCameraWidth;
    int        latest_frame_h   = mambo::AppConfig::kCameraHeight;
    std::vector<mambo::ObjectResult> slow_objects;
    std::vector<mambo::FaceResult>   slow_faces;
    std::atomic<bool> running(true);
    std::atomic<int>  det_fps(0);

    // 线程1：只读摄像头 + 推流，不做任何推理（保证流帧率稳定）
    std::thread capture_thread([&]() {
        while (running) {
            cv::Mat frame;
            cap >> frame;
            if (frame.empty()) continue;

            web.PushVideoFrame(frame); // 异步编码，不阻塞

            {
                std::lock_guard<std::mutex> lk(cap_mtx);
                frame.copyTo(shared_frame);
                ++shared_seq;
            }
            cap_cv.notify_one();
        }
    });

    // 线程2：推理线程，从共享帧做检测，结果直接推送眼睛UI
    std::thread vision_thread([&]() {
        int frame_count = 0, cnt = 0;
        uint64_t last_seq = 0;
        float smooth_cx = mambo::AppConfig::kCameraWidth  / 2.0f;
        float smooth_cy = mambo::AppConfig::kCameraHeight / 2.0f;
        auto t0 = std::chrono::steady_clock::now();
        while (running) {
            cv::Mat frame;
            int fw, fh;
            {
                std::unique_lock<std::mutex> lk(cap_mtx);
                cap_cv.wait_for(lk, std::chrono::milliseconds(100),
                    [&]() { return !running || shared_seq != last_seq; });
                if (!running) break;
                if (shared_frame.empty() || shared_seq == last_seq) continue;
                shared_frame.copyTo(frame);
                fw = frame.cols; fh = frame.rows;
                last_seq = shared_seq;
            }
            frame_count++;

            auto boxes = vision.DetectFaceBoxes(frame);

            // 推理完立即更新坐标并推送，不经过主线程
            {
                std::lock_guard<std::mutex> lk(data_mtx);
                has_face       = !boxes.empty();
                fast_box       = has_face ? boxes[0] : cv::Rect();
                latest_frame_w = fw;
                latest_frame_h = fh;
            }

            // 直接在推理线程推送眼睛坐标，情绪由 LLM 控制
            {
                if (!boxes.empty()) {
                    float target_cx = boxes[0].x + boxes[0].width  / 2.0f;
                    float target_cy = boxes[0].y + boxes[0].height / 2.0f;
                    smooth_cx += (target_cx - smooth_cx) * 0.4f;
                    smooth_cy += (target_cy - smooth_cy) * 0.4f;
                }
                float half_w = std::max(1.0f, fw / 2.0f);
                float half_h = std::max(1.0f, fh / 2.0f);
                float nx = -(smooth_cx - half_w) / half_w;
                float ny =  (smooth_cy - half_h) / half_h;
                web.PushEyePos(nx, ny);
            }

            std::vector<mambo::ObjectResult> objects;
            std::vector<mambo::FaceResult>   faces;
            if (frame_count % 15 == 0)
                vision.ProcessFrame(frame, objects, faces, frame_count);

            {
                std::lock_guard<std::mutex> lk(data_mtx);
                if (frame_count % 15 == 0) {
                    slow_objects = objects;
                    slow_faces   = faces;
                    if (!faces.empty()) dialog.SetCurrentEmotion(faces[0].emotion);
                }
            }

            cnt++;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1000) { det_fps = cnt; cnt = 0; t0 = std::chrono::steady_clock::now(); }
        }
    });

    // 主线程：串口轮询 + 控制台打印 + status推送
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

        // 轮询串口接收 ESP32 上报
        serial.Poll();

        // 消费警报，触发语音
        std::string alert = serial.ConsumeAlert();
        if (!alert.empty()) {
            if (alert == "cliff") {
                playAlert("啊！前面是悬崖，星宝~");
            } else if (alert == "fall") {
                playAlert("啊！星宝要跌落了！");
            } else if (alert == "dizzy") {
                web.PushEyeData(0, 0, "Dizzy"); // 晕眩旋涡表情
                playAlert("别晃了，星宝好晕呀~");
            }
        }

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
            std::cout << "\n┌─ 视觉 FPS:" << det_fps << "  状态:" << ss
                      << "  🎤RMS:" << dialog.GetMicRms()
                      << "(阈值:" << mambo::AppConfig::kVoiceThreshold << ")";
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
                          << "加速度 X:" << esp.ax << " Y:" << esp.ay << " Z:" << esp.az << " g  "
                          << "陀螺 X:" << std::setprecision(1) << esp.gx
                          << " Y:" << esp.gy << " Z:" << esp.gz << " °/s  "
                          << "悬崖:" << (esp.cliff ? "⚠ 检测到" : "安全") << "  "
                          << "雷达:" << (esp.radar ? "触发" : "无") << "  "
                          << "动作:" << esp.act;
            } else {
                std::cout << "\n└─ ESP32  等待连接...";
            }
            std::cout << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    running = false;
    cap_cv.notify_all();
    capture_thread.join();
    vision_thread.join();
    curl_global_cleanup();
    return 0;
}
