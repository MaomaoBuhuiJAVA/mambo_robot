#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <cmath>

namespace mambo {

class VectorEyes {
public:
    VectorEyes() : smooth_x_(0), smooth_y_(0), blink_timer_(0), last_blink_(0) {}

    cv::Mat Render(int face_cx, int face_cy, int frame_w, int frame_h,
                   const std::string& emotion, ChatState state) {
        // 深色背景
        cv::Mat canvas(480, 800, CV_8UC3, cv::Scalar(5, 18, 15));

        // 人脸坐标归一化，负号修正镜像
        float norm_x = -(float)(face_cx - frame_w / 2) / (frame_w / 2);
        float norm_y =  (float)(face_cy - frame_h / 2) / (frame_h / 2);

        // 弹簧平滑（低系数=更丝滑，高系数=更跟手）
        smooth_x_ += (norm_x - smooth_x_) * 0.2f;
        smooth_y_ += (norm_y - smooth_y_) * 0.2f;

        // 跟随偏移，范围 ±80px
        int ox = (int)(smooth_x_ * 80);
        int oy = (int)(smooth_y_ * 80);

        // 眨眼逻辑
        blink_timer_++;
        bool blinking = false;
        if (blink_timer_ - last_blink_ > 150 + rand() % 200) {
            last_blink_ = blink_timer_;
        }
        if (blink_timer_ - last_blink_ < 4) blinking = true;

        // 颜色
        cv::Scalar color = GetColor(emotion, state);

        // 眼睛中心
        int lx = 250, rx = 550, ey = 240;

        // 外层眼眶（大圆角矩形，半透明灰）
        DrawRoundRect(canvas, cv::Rect(130, 130, 540, 220), 50, cv::Scalar(40, 50, 45), -1);

        // 左眼、右眼
        DrawEyeWithGlow(canvas, lx + ox, ey + oy, emotion, state, color, blinking);
        DrawEyeWithGlow(canvas, rx + ox, ey + oy, emotion, state, color, blinking);

        return canvas;
    }

private:
    float smooth_x_, smooth_y_;
    int blink_timer_, last_blink_;

    cv::Scalar GetColor(const std::string& emotion, ChatState state) {
        if (state == ChatState::kThinking)  return cv::Scalar(255, 150, 50);
        if (state == ChatState::kListening) return cv::Scalar(255, 229, 0);
        if (emotion == "KaiXin")  return cv::Scalar(170, 255, 0);   // 绿
        if (emotion == "ShengQi") return cv::Scalar(80,  80,  255); // 红
        if (emotion == "NanGuo")  return cv::Scalar(255, 180, 100); // 蓝
        return cv::Scalar(255, 229, 0); // 默认青色 #00e5ff BGR
    }

    // 用多边形近似圆角矩形
    void DrawRoundRect(cv::Mat& img, cv::Rect rect, int r, cv::Scalar color, int thickness) {
        std::vector<cv::Point> pts;
        int x = rect.x, y = rect.y, w = rect.width, h = rect.height;
        // 四个圆角
        for (int i = 90; i <= 180; i += 3) pts.push_back({x + r + (int)(r * cos(i * CV_PI / 180)), y + r + (int)(r * sin(i * CV_PI / 180))});
        for (int i = 180; i <= 270; i += 3) pts.push_back({x + r + (int)(r * cos(i * CV_PI / 180)), y + h - r + (int)(r * sin(i * CV_PI / 180))});
        for (int i = 270; i <= 360; i += 3) pts.push_back({x + w - r + (int)(r * cos(i * CV_PI / 180)), y + h - r + (int)(r * sin(i * CV_PI / 180))});
        for (int i = 0; i <= 90; i += 3) pts.push_back({x + w - r + (int)(r * cos(i * CV_PI / 180)), y + r + (int)(r * sin(i * CV_PI / 180))});
        if (thickness < 0) cv::fillPoly(img, std::vector<std::vector<cv::Point>>{pts}, color);
        else cv::polylines(img, pts, true, color, thickness);
    }

    // 绘制带发光效果的眼睛
    void DrawEyeWithGlow(cv::Mat& canvas, int cx, int cy, const std::string& emotion,
                         ChatState state, cv::Scalar color, bool blinking) {
        // 在临时图层上画眼睛，然后模糊叠加实现发光
        cv::Mat glow_layer(canvas.size(), CV_8UC3, cv::Scalar(0, 0, 0));
        cv::Mat eye_layer(canvas.size(), CV_8UC3, cv::Scalar(0, 0, 0));

        if (blinking) {
            // 眨眼：横线
            cv::line(eye_layer, cv::Point(cx - 65, cy), cv::Point(cx + 65, cy), color, 5);
            cv::line(glow_layer, cv::Point(cx - 65, cy), cv::Point(cx + 65, cy), color, 5);
        } else if (state == ChatState::kThinking) {
            cv::ellipse(eye_layer, cv::Point(cx, cy), cv::Size(45, 45), 0, 0, 360, color, -1);
            cv::ellipse(glow_layer, cv::Point(cx, cy), cv::Size(45, 45), 0, 0, 360, color, -1);
        } else if (emotion == "KaiXin") {
            // 开心：面包片形（圆角矩形下半部分 + 圆顶）
            DrawRoundRect(eye_layer, cv::Rect(cx - 65, cy - 30, 130, 120), 30, color, -1);
            // 遮掉顶部变成面包片
            cv::rectangle(eye_layer, cv::Rect(cx - 70, cy - 100, 140, 80), cv::Scalar(0,0,0), -1);
            DrawRoundRect(glow_layer, cv::Rect(cx - 65, cy - 30, 130, 120), 30, color, -1);
            cv::rectangle(glow_layer, cv::Rect(cx - 70, cy - 100, 140, 80), cv::Scalar(0,0,0), -1);
        } else if (emotion == "ShengQi") {
            // 生气：倾斜圆角矩形
            cv::RotatedRect rr(cv::Point2f(cx, cy), cv::Size2f(130, 160), 15);
            cv::ellipse(eye_layer, rr, color, -1);
            cv::ellipse(glow_layer, rr, color, -1);
            // 遮掉上半部分
            std::vector<cv::Point> mask = {{cx-80,cy-100},{cx+80,cy-100},{cx+40,cy-10},{cx-40,cy-10}};
            cv::fillConvexPoly(eye_layer, mask, cv::Scalar(0,0,0));
            cv::fillConvexPoly(glow_layer, mask, cv::Scalar(0,0,0));
        } else if (emotion == "NanGuo") {
            // 难过：倒面包片
            DrawRoundRect(eye_layer, cv::Rect(cx - 65, cy - 90, 130, 120), 30, color, -1);
            cv::rectangle(eye_layer, cv::Rect(cx - 70, cy + 20, 140, 80), cv::Scalar(0,0,0), -1);
            DrawRoundRect(glow_layer, cv::Rect(cx - 65, cy - 90, 130, 120), 30, color, -1);
            cv::rectangle(glow_layer, cv::Rect(cx - 70, cy + 20, 140, 80), cv::Scalar(0,0,0), -1);
        } else {
            // 默认：圆角矩形
            DrawRoundRect(eye_layer, cv::Rect(cx - 65, cy - 90, 130, 180), 35, color, -1);
            DrawRoundRect(glow_layer, cv::Rect(cx - 65, cy - 90, 130, 180), 35, color, -1);
        }

        // 发光：对 glow_layer 做大半径高斯模糊，叠加到 canvas
        cv::GaussianBlur(glow_layer, glow_layer, cv::Size(61, 61), 25);
        cv::add(canvas, glow_layer, canvas);

        // 再叠加一层小模糊（近光晕）
        cv::Mat near_glow = eye_layer.clone();
        cv::GaussianBlur(near_glow, near_glow, cv::Size(21, 21), 8);
        cv::add(canvas, near_glow, canvas);

        // 最后叠加清晰眼睛本体
        cv::add(canvas, eye_layer, canvas);
    }
};

} // namespace mambo
