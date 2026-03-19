#include "config.hpp"
#include "utils.hpp"
#include "web_server.hpp"
#include "vision_engine.hpp"
#include "dialog_system.hpp"
#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    mambo::SerialManager serial(mambo::AppConfig::kSerialPort);
    mambo::WebServer web;
    web.Start(&serial);

    mambo::VisionEngine vision;
    mambo::DialogSystem dialog(&serial);
    dialog.Start();

    cv::VideoCapture cap(0);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    std::mutex data_mtx;
    cv::Rect   fast_box;
    bool       has_face = false;
    std::vector<mambo::ObjectResult> slow_objects;
    std::vector<mambo::FaceResult>   slow_faces;
    std::atomic<bool> running(true);
    std::atomic<int>  det_fps(0);

    // 视觉线程：每帧人脸检测，每15帧完整推理
    std::thread vision_thread([&]() {
        int frame_count = 0, cnt = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (running) {
            cv::Mat frame;
            cap >> frame;
            if (frame.empty()) continue;
            frame_count++;

            auto boxes = vision.DetectFaceBoxes(frame);

            std::vector<mambo::ObjectResult> objects;
            std::vector<mambo::FaceResult>   faces;
            if (frame_count % 15 == 0)
                vision.ProcessFrame(frame, objects, faces, frame_count);

            {
                std::lock_guard<std::mutex> lk(data_mtx);
                has_face = !boxes.empty();
                fast_box = has_face ? boxes[0] : cv::Rect();
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
    float smooth_cx = 320, smooth_cy = 240;
    auto last_print = std::chrono::steady_clock::now();

    while (true) {
        cv::Rect box; bool hf;
        std::vector<mambo::ObjectResult> objects;
        std::vector<mambo::FaceResult>   faces;
        {
            std::lock_guard<std::mutex> lk(data_mtx);
            box     = fast_box;
            hf      = has_face;
            objects = slow_objects;
            faces   = slow_faces;
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

        // 推送给浏览器
        float nx = -(smooth_cx - 320.0f) / 320.0f;  // 负号修正镜像
        float ny =  (smooth_cy - 240.0f) / 240.0f;
        web.PushEyeData(nx, ny, emo);

        // 控制台每秒打印
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print).count() >= 1000) {
            last_print = now;
            std::string ss = (state == mambo::ChatState::kWaiting  ? "等待" :
                              state == mambo::ChatState::kListening ? "聆听" :
                              state == mambo::ChatState::kThinking  ? "思考" : "说话");
            std::cout << "\n[FPS:" << det_fps << " 状态:" << ss << "]";
            if (faces.empty()) std::cout << "  人脸:无";
            else for (const auto& f : faces)
                std::cout << "  [" << f.name << "|" << f.emotion
                          << "|" << std::fixed << std::setprecision(2) << f.score << "]";
            if (!objects.empty()) {
                std::cout << "  物品:";
                for (const auto& o : objects)
                    std::cout << mambo::kClassNames[o.label] << "(" << (int)(o.prob*100) << "%) ";
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
