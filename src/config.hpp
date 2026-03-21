#pragma once
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

namespace mambo {
struct AppConfig {
    static constexpr const char* kAlsaRecDevice  = "plughw:3,0";
    static constexpr const char* kAlsaPlayDevice = "plughw:2,0";

    // API Keys
    static constexpr const char* kBaiduApiKey    = "mOEMA8FfryEAdeEsJ5cIMH0D";
    static constexpr const char* kBaiduSecretKey = "H5JqN86FH681HLEOY2cfBbD8m22WVBBd";
    static constexpr const char* kDeepseekApiKey = "sk-a10bdd9276944b6685c6cdaeba7e60b5";

    // 模型与资源路径
    static constexpr const char* kYoloModelPath = "./models/yolov5.nb";
    static constexpr const char* kYunetPath     = "./models/face_detection_yunet_2023mar.onnx";
    static constexpr const char* kSfacePath     = "./models/face_recognition_sface_2021dec.onnx";
    static constexpr const char* kEmotionPath   = "./models/emotion-ferplus-8.onnx";
    static constexpr const char* kBaseImgDir    = "./faces/";

    // 串口 & Web
    static constexpr const char* kSerialPort = "/dev/ttyS7";
    static constexpr const char* kWebHost    = "0.0.0.0";
    static constexpr int kWebPort            = 8080;

    // 摄像头 & 视频流
    static constexpr int kCameraIndex      = 0;
    static constexpr int kCameraWidth      = 640;
    static constexpr int kCameraHeight     = 480;
    static constexpr int kCameraTargetFps  = 30;
    static constexpr int kVideoStreamFps   = 15;  // 流帧率
    static constexpr int kVideoJpegQuality = 65;  // JPEG 质量
    static constexpr int kStreamWidth      = 480; // 流编码宽度（检测仍用原始分辨率）
    static constexpr int kStreamHeight     = 270; // 流编码高度

    static constexpr int kVoiceThreshold   = 100;  // 正常说话触发阈值
    static constexpr int kSilenceThreshold = 80;   // 低于此值视为静音
    static constexpr int kSilenceLimitMs   = 1000;
    static constexpr int kInputSize        = 640;
};

const std::vector<std::string> kEmotionNames = {
    "ZhongXing","KaiXin","JingYa","NanGuo","ShengQi","YanWu","KongJu","MiMang"
};
const std::vector<std::string> kClassNames = {
    "Ren","ZiXingChe","QiChe","MoTuoChe","FeiJi","GongJiaoChe","HuoChe","KaChe","Chuan","HongLvDeng",
    "XiaoFangShuan","TingZhiPai","TingCheBiao","ChangYi","Niao","Mao","Gou","Ma","Yang","Niu",
    "DaXiang","Xiong","BanMa","ChangJingLu","BeiBao","YuSan","ShouTiBao","LingDai","XingLiXiang","FeiPan",
    "HuaXueBan","DanBan","YunDongQiu","FengZheng","BangQiuBang","ShouTao","HuaBan","ChongLangBan","WangQiuPai","PingZi",
    "GaoJiaoBei","BeiZi","ChaZi","Dao","ShaoZi","Wan","XiangJiao","PingGuo","SanMingZhi","JuZi",
    "XiLanHua","HuLuoBo","ReGou","PiSa","TianTianQuan","DanGao","YiZi","ShaFa","PenZai","Chuang",
    "CanZhuo","MaTong","DianShi","BiJiBen","ShuBiao","YaoKongQi","JianPan","ShouJi","WeiBoLu","KaoXiang",
    "MianBaoJi","ShuiCao","BingXiang","Shu","ZhongBiao","HuaPing","JianDao","TaiDiXiong","ChuiFengJi","YaShua"
};
enum class ChatState { kWaiting, kListening, kThinking, kSpeaking };
struct ObjectResult {
    cv::Rect_<float> rect; int label; float prob;
    ObjectResult(cv::Rect_<float> r, int l, float p) : rect(r), label(l), prob(p) {}
};
struct FaceResult {
    cv::Rect box; std::string name; double score; std::string emotion;
    FaceResult(cv::Rect b, std::string n, double s, std::string e)
        : box(b), name(n), score(s), emotion(e) {}
};
}
