#pragma once
#include "config.hpp"
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/dnn.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <cfloat>
#include <cmath>
#include "awnn_lib.h"

namespace mambo {

class VisionEngine {
public:
    VisionEngine() {
        awnn_init();
        yolo_ctx_ = awnn_create(AppConfig::kYoloModelPath);
        yolo_buffer_.resize(AppConfig::kInputSize * AppConfig::kInputSize * 3);

        face_det_ = cv::FaceDetectorYN::create(AppConfig::kYunetPath, "", cv::Size(320, 320));
        face_det_->setScoreThreshold(0.5f);
        face_det_->setNMSThreshold(0.3f);

        face_rec_ = cv::FaceRecognizerSF::create(AppConfig::kSfacePath, "");
        emotion_net_ = cv::dnn::readNetFromONNX(AppConfig::kEmotionPath);

        LoadFaceDatabase();
    }

    ~VisionEngine() {
        if (yolo_ctx_) awnn_destroy(yolo_ctx_);
        awnn_uninit();
    }

    // 快速人脸检测（只用 YuNet，不做识别和情绪，给眼睛跟随用）
    std::vector<cv::Rect> DetectFaceBoxes(const cv::Mat& frame) {
        std::vector<cv::Rect> boxes;
        cv::Mat small;
        cv::resize(frame, small, cv::Size(320, 240));  // 降低分辨率加速
        cv::Mat faces;
        face_det_->setInputSize(small.size());
        face_det_->detect(small, faces);
        float scale_x = (float)frame.cols / 320.0f;
        float scale_y = (float)frame.rows / 240.0f;
        for (int i = 0; i < faces.rows; i++) {
            cv::Rect box((int)(faces.at<float>(i,0) * scale_x), (int)(faces.at<float>(i,1) * scale_y),
                         (int)(faces.at<float>(i,2) * scale_x), (int)(faces.at<float>(i,3) * scale_y));
            box = box & cv::Rect(0, 0, frame.cols, frame.rows);
            if (box.width >= 20) boxes.push_back(box);
        }
        return boxes;
    }

    // 完整推理（YOLO + 人脸识别 + 情绪），低频调用
    void ProcessFrame(cv::Mat& frame, std::vector<ObjectResult>& objects, std::vector<FaceResult>& faces, int frame_count) {
        float ratio = std::min(640.0 / frame.cols, 640.0 / frame.rows);
        cv::Mat canvas(640, 640, CV_8UC3, cv::Scalar(114, 114, 114));
        cv::resize(frame,
            canvas(cv::Rect((640 - frame.cols * ratio) / 2, (640 - frame.rows * ratio) / 2,
                            frame.cols * ratio, frame.rows * ratio)),
            cv::Size(frame.cols * ratio, frame.rows * ratio));

        FastPlanarBgr2Rgb(canvas, yolo_buffer_.data());
        void* inputs[] = { yolo_buffer_.data() };
        awnn_set_input_buffers(yolo_ctx_, inputs);
        awnn_run(yolo_ctx_);
        float** yolo_out = (float**)awnn_get_output_buffers(yolo_ctx_);
        objects = DecodeYolo(yolo_out, ratio, frame.cols, frame.rows);

        if (frame_count % 5 == 0 || last_faces_.empty()) {
            last_faces_ = DetectFaces(frame);
        }
        faces = last_faces_;
    }

    // 仅做物品检测（YOLO），用于提高物品识别刷新率
    // 注意：不执行人脸识别/情绪，避免拖慢或降低整体体验
    void ProcessObjects(cv::Mat& frame, std::vector<ObjectResult>& objects) {
        float ratio = std::min(640.0f / frame.cols, 640.0f / frame.rows);
        cv::Mat canvas(640, 640, CV_8UC3, cv::Scalar(114, 114, 114));
        cv::resize(frame,
                   canvas(cv::Rect((640 - frame.cols * ratio) / 2, (640 - frame.rows * ratio) / 2,
                                   frame.cols * ratio, frame.rows * ratio)),
                   cv::Size(frame.cols * ratio, frame.rows * ratio));

        FastPlanarBgr2Rgb(canvas, yolo_buffer_.data());
        void* inputs[] = { yolo_buffer_.data() };
        awnn_set_input_buffers(yolo_ctx_, inputs);
        awnn_run(yolo_ctx_);
        float** yolo_out = (float**)awnn_get_output_buffers(yolo_ctx_);
        objects = DecodeYolo(yolo_out, ratio, frame.cols, frame.rows);
    }

    // 仅做人脸检测/识别+情绪更新（不再跑 YOLO），用于避免重复物品推理
    void ProcessFaces(cv::Mat& frame, std::vector<FaceResult>& faces, int frame_count) {
        if (frame_count % 5 == 0 || last_faces_.empty()) {
            last_faces_ = DetectFaces(frame);
        }
        faces = last_faces_;
    }

private:
    Awnn_Context_t* yolo_ctx_ = nullptr;
    std::vector<uint8_t> yolo_buffer_;
    cv::Ptr<cv::FaceDetectorYN> face_det_;
    cv::Ptr<cv::FaceRecognizerSF> face_rec_;
    cv::dnn::Net emotion_net_;

    struct PersonRecord { std::string name; cv::Mat feature; };
    std::vector<PersonRecord> face_db_;
    std::vector<FaceResult> last_faces_;

    void LoadFaceDatabase() {
        std::vector<std::pair<std::string, std::string>> templates = {
            {"csh", std::string(AppConfig::kBaseImgDir) + "csh.jpg"},
            {"oyc", std::string(AppConfig::kBaseImgDir) + "oyc.jpg"}
        };

        for (const auto& tmpl : templates) {
            cv::Mat img = cv::imread(tmpl.second);
            if (img.empty()) {
                std::cerr << "[Warning] 找不到底库图片: " << tmpl.second << std::endl;
                continue;
            }

            cv::Mat faces;
            face_det_->setInputSize(img.size());
            face_det_->detect(img, faces);

            if (faces.rows == 0) {
                cv::Mat resized;
                cv::resize(img, resized, cv::Size(640, 480));
                face_det_->setInputSize(resized.size());
                face_det_->detect(resized, faces);
                if (faces.rows > 0) img = resized;
            }

            if (faces.rows > 0) {
                cv::Mat aligned, feature;
                face_rec_->alignCrop(img, faces.row(0), aligned);
                face_rec_->feature(aligned, feature);
                face_db_.push_back({tmpl.first, feature.clone()});
                std::cout << "[Success] 成功提取人脸特征: " << tmpl.first << std::endl;
            } else {
                std::cerr << "[Error] 无法从图片提取人脸，请检查方向或光照: " << tmpl.second << std::endl;
            }
        }
    }

    std::vector<FaceResult> DetectFaces(const cv::Mat& frame) {
        std::vector<FaceResult> results;
        cv::Mat faces;
        face_det_->setInputSize(frame.size());
        face_det_->detect(frame, faces);

        for (int i = 0; i < faces.rows; i++) {
            cv::Rect box((int)faces.at<float>(i,0), (int)faces.at<float>(i,1),
                         (int)faces.at<float>(i,2), (int)faces.at<float>(i,3));
            box = box & cv::Rect(0, 0, frame.cols, frame.rows);
            if (box.width < 20) continue;

            // 情绪识别
            cv::Mat roi = frame(box).clone();
            cv::cvtColor(roi, roi, cv::COLOR_BGR2GRAY);
            cv::Mat blob = cv::dnn::blobFromImage(roi, 1.0, cv::Size(64, 64), cv::Scalar(0), false, false);
            emotion_net_.setInput(blob);
            cv::Mat prob = emotion_net_.forward();
            cv::Point classIdPoint;
            cv::minMaxLoc(prob, 0, 0, 0, &classIdPoint);
            std::string emotion = kEmotionNames[classIdPoint.x];

            // 人脸识别
            std::string name = "Unknown";
            double max_score = -1.0;
            try {
                cv::Mat aligned, feature;
                face_rec_->alignCrop(frame, faces.row(i), aligned);
                face_rec_->feature(aligned, feature);
                for (const auto& db_person : face_db_) {
                    double score = face_rec_->match(feature, db_person.feature, cv::FaceRecognizerSF::FR_COSINE);
                    if (score > max_score) {
                        max_score = score;
                        if (score > 0.60) name = db_person.name;
                    }
                }
            } catch (...) {}

            results.push_back({box, name, max_score, emotion});
        }
        return results;
    }

    void FastPlanarBgr2Rgb(const cv::Mat& src, uint8_t* dest) {
        int img_size = 640 * 640;
        uint8_t* r_p = dest, *g_p = dest + img_size, *b_p = dest + img_size * 2;
        const uint8_t* p_s = src.data;
        for (int i = 0; i < img_size; ++i) {
            *b_p++ = *p_s++; *g_p++ = *p_s++; *r_p++ = *p_s++;
        }
    }

    inline float Sigmoid(float x) { return 1.f / (1.f + expf(-x)); }

    std::vector<ObjectResult> DecodeYolo(float** out, float ratio, int frame_w, int frame_h) {
        std::vector<ObjectResult> proposals;
        int strides[] = {8, 16, 32};
        float anchors[18] = {10,13, 16,30, 33,23, 30,61, 62,45, 59,119, 116,90, 156,198, 373,326};

        for (int i = 0; i < 3; i++) {
            int stride = strides[i];
            int feat_w = 640 / stride;
            float* feat = out[i];
            for (int h = 0; h < feat_w; h++) {
                for (int w = 0; w < feat_w; w++) {
                    for (int a = 0; a < 3; a++) {
                        const float* f = &feat[(a * feat_w * feat_w + h * feat_w + w) * 85];
                        if (Sigmoid(f[4]) < 0.45f) continue;
                        float cls = -FLT_MAX; int id = 0;
                        for (int s = 0; s < 80; s++) { if (f[s+5] > cls) { cls = f[s+5]; id = s; } }
                        if (Sigmoid(f[4]) * Sigmoid(cls) >= 0.45f) {
                            float dx = Sigmoid(f[0]), dy = Sigmoid(f[1]);
                            float dw = Sigmoid(f[2]), dh = Sigmoid(f[3]);
                            float cx = (dx * 2.0f - 0.5f + w) * stride;
                            float cy = (dy * 2.0f - 0.5f + h) * stride;
                            float pw = dw * dw * 4.0f * anchors[i*6 + a*2];
                            float ph = dh * dh * 4.0f * anchors[i*6 + a*2 + 1];
                            float rx = (cx - pw * 0.5f - (640 - frame_w * ratio) / 2) / ratio;
                            float ry = (cy - ph * 0.5f - (640 - frame_h * ratio) / 2) / ratio;
                            float rw = pw / ratio, rh = ph / ratio;
                            proposals.push_back({cv::Rect_<float>(rx, ry, rw, rh), id,
                                                 Sigmoid(f[4]) * Sigmoid(cls)});
                        }
                    }
                }
            }
        }

        // NMS
        std::vector<ObjectResult> final_objs;
        std::sort(proposals.begin(), proposals.end(),
            [](const ObjectResult& a, const ObjectResult& b){ return a.prob > b.prob; });
        std::vector<bool> suppress(proposals.size(), false);
        for (size_t i = 0; i < proposals.size(); i++) {
            if (suppress[i]) continue;
            final_objs.push_back(proposals[i]);
            for (size_t j = i + 1; j < proposals.size(); j++) {
                float inter = (proposals[i].rect & proposals[j].rect).area();
                if (inter / (proposals[i].rect.area() + proposals[j].rect.area() - inter) > 0.45f)
                    suppress[j] = true;
            }
        }
        return final_objs;
    }
};

} // namespace mambo