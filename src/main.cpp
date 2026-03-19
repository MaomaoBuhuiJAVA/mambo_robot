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

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    mambo::SerialManager serial(mambo::AppConfig::kSerialPort);
    mambo::WebServer web;
    web.Start(&serial);

    mambo::VisionEngine vision;
    mambo::DialogSystem dialog(&serial);
    dialog.Start();

    cv::VideoCapture cap(mambo::AppConfig::kCameraIndex);
    if (!cap.isOpened()) {
        std::cerr << "[Error] 无法打开摄像头 index=" << mambo::AppConfig::kCameraIndex << std::endl;
        curl_global_cleanup();
        return 1;
    }

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, mambo::AppConfig::kCameraWidth);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, mambo::AppConfig::kCameraHeight);
    cap.set(cv::CAP_PROP_FPS, mambo::AppConfig::kCameraTargetFps);

    std::mutex data_mtx;
    cv::Rect   fast_box;
    bool       has_face = false;
    std::vector<mambo::ObjectResult> slow_objects;
    std::vector<mambo::FaceResult>   slow_faces;
    int latest_frame_width = mambo::AppConfig::kCameraWidth;
    int latest_frame_height = mambo::AppConfig::kCameraHeight;
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
            web.PushVideoFrame(frame);

            auto boxes = vision.DetectFaceBoxes(frame);

            std::vector<mambo::ObjectResult> objects;
            std::vector<mambo::FaceResult>   faces;
            if (frame_count % 15 == 0)
                vision.ProcessFrame(frame, objects, faces, frame_count);

            {
                std::lock_guard<std::mutex> lk(data_mtx);
                has_face = !boxes.empty();
                fast_box = has_face ? boxes[0] : cv::Rect();
                latest_frame_width = frame.cols;
                latest_frame_height = frame.rows;
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
    float smooth_cx = mambo::AppConfig::kCameraWidth / 2.0f;
    float smooth_cy = mambo::AppConfig::kCameraHeight / 2.0f;
    auto last_print = std::chrono::steady_clock::now();

    while (true) {
        cv::Rect box; bool hf;
        std::vector<mambo::ObjectResult> objects;
        std::vector<mambo::FaceResult>   faces;
        int frame_width;
        int frame_height;
        {
            std::lock_guard<std::mutex> lk(data_mtx);
            box     = fast_box;
            hf      = has_face;
            objects = slow_objects;
            faces   = slow_faces;
            frame_width = latest_frame_width;
            frame_height = latest_frame_height;
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
        float half_w = std::max(1.0f, frame_width / 2.0f);
        float half_h = std::max(1.0f, frame_height / 2.0f);
        float nx = -(smooth_cx - half_w) / half_w;  // 负号修正镜像
        float ny =  (smooth_cy - half_h) / half_h;
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
