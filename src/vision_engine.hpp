#pragma once
#include "config.hpp"
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
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

        try {
            face_det_ = cv::FaceDetectorYN::create(AppConfig::kYunetPath, "", cv::Size(320, 320));
            face_det_->setScoreThreshold(0.5f);
            face_det_->setNMSThreshold(0.3f);
            face_rec_ = cv::FaceRecognizerSF::create(AppConfig::kSfacePath, "");
            yunet_ok_ = true;
            LoadFaceDatabase();
        } catch (const cv::Exception& e) {
            yunet_ok_ = false;
            face_det_.release();
            face_rec_.release();
            std::cerr << "[Warning] YuNet/SFace 初始化失败，已禁用人脸相关功能: " << e.what() << std::endl;
        } catch (...) {
            yunet_ok_ = false;
            face_det_.release();
            face_rec_.release();
            std::cerr << "[Warning] YuNet/SFace 初始化失败，已禁用人脸相关功能" << std::endl;
        }

        std::cerr << "[Info] 情绪DNN模块已禁用，使用默认情绪输出" << std::endl;
    }

    ~VisionEngine() {
        if (yolo_ctx_) awnn_destroy(yolo_ctx_);
        awnn_uninit();
    }

    // 快速人脸检测（只用 YuNet，不做识别和情绪，给眼睛跟随用）
    std::vector<cv::Rect> DetectFaceBoxes(const cv::Mat& frame) {
        std::vector<cv::Rect> boxes;

        // YuNet 在 OpenCV 4.5.4 下对“高度 < 320”的动态输入尺寸容易不稳定（可能触发 layer id=-1）。
        // 这里固定采用 320x320 的 letterbox 输入做快速检测。
        constexpr int kDetSize = 320;
        const float ratio = std::min((float)kDetSize / frame.cols, (float)kDetSize / frame.rows);
        const int nw = std::max(1, (int)std::round(frame.cols * ratio));
        const int nh = std::max(1, (int)std::round(frame.rows * ratio));
        const int dx = (kDetSize - nw) / 2;
        const int dy = (kDetSize - nh) / 2;
        cv::Mat canvas(kDetSize, kDetSize, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::resize(frame, canvas(cv::Rect(dx, dy, nw, nh)), cv::Size(nw, nh));

        cv::Mat faces;
        SafeYuNetDetect(canvas, faces);
        for (int i = 0; i < faces.rows; i++) {
            const float x = faces.at<float>(i, 0);
            const float y = faces.at<float>(i, 1);
            const float w = faces.at<float>(i, 2);
            const float h = faces.at<float>(i, 3);

            const float rx = (x - dx) / ratio;
            const float ry = (y - dy) / ratio;
            const float rw = w / ratio;
            const float rh = h / ratio;

            cv::Rect box((int)rx, (int)ry, (int)rw, (int)rh);
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

    struct PersonRecord { std::string name; cv::Mat feature; };
    std::vector<PersonRecord> face_db_;
    std::vector<FaceResult> last_faces_;
    bool yunet_ok_ = true;

    static cv::Mat PadRightBottomToMultiple(const cv::Mat& src, int multiple, int min_w = 0, int min_h = 0) {
        if (src.empty()) return src;
        const int want_w = std::max(src.cols, min_w);
        const int want_h = std::max(src.rows, min_h);
        const int new_w = ((want_w + multiple - 1) / multiple) * multiple;
        const int new_h = ((want_h + multiple - 1) / multiple) * multiple;
        if (new_w == src.cols && new_h == src.rows) return src;

        cv::Mat dst;
        const int right = new_w - src.cols;
        const int bottom = new_h - src.rows;
        cv::copyMakeBorder(src, dst, 0, bottom, 0, right, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        return dst;
    }

    bool SafeYuNetDetect(const cv::Mat& img, cv::Mat& out_faces) {
        if (!yunet_ok_ || face_det_.empty()) return false;
        try {
            face_det_->setInputSize(img.size());
            face_det_->detect(img, out_faces);
            return true;
        } catch (const cv::Exception& e) {
            std::cerr << "[Warning] YuNet detect 异常，已临时禁用人脸检测: " << e.what() << std::endl;
            yunet_ok_ = false;
            out_faces.release();
            return false;
        } catch (...) {
            std::cerr << "[Warning] YuNet detect 异常，已临时禁用人脸检测" << std::endl;
            yunet_ok_ = false;
            out_faces.release();
            return false;
        }
    }

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
            cv::Mat det_img = PadRightBottomToMultiple(img, 32, 320, 320);
            SafeYuNetDetect(det_img, faces);

            if (faces.rows == 0) {
                cv::Mat resized;
                cv::resize(img, resized, cv::Size(640, 480));
                det_img = PadRightBottomToMultiple(resized, 32, 320, 320);
                SafeYuNetDetect(det_img, faces);
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
        cv::Mat det_frame = PadRightBottomToMultiple(frame, 32, 320, 320);
        SafeYuNetDetect(det_frame, faces);

        for (int i = 0; i < faces.rows; i++) {
            cv::Rect box((int)faces.at<float>(i,0), (int)faces.at<float>(i,1),
                         (int)faces.at<float>(i,2), (int)faces.at<float>(i,3));
            box = box & cv::Rect(0, 0, frame.cols, frame.rows);
            if (box.width < 20) continue;

            // 情绪DNN模块已禁用，统一输出默认情绪
            std::string emotion = "ZhongXing";

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