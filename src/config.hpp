#pragma once
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

namespace mambo {
struct AppConfig {
    static constexpr const char* kAlsaRecDevice  = "plughw:2,0";
    static constexpr const char* kAlsaPlayDevice = "plughw:3,0";

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

    // ===== 音频采集判定参数（见 src/dialog_system.hpp: AudioLoop）=====
    // 麦克风输入用 mean(|sample|) 近似“音量/RMS”（16kHz, S16_LE, 单声道）。
    // 1) 超过 kVoiceThreshold：开始录音；2) 连续低于 kSilenceThreshold 达到 kSilenceLimitMs：结束录音。
    // 另外：机器人正在说话(speaking)时，为避免扬声器回声误触发“打断”，会用 kVoiceThreshold * 4 作为打断阈值。
    static constexpr int kVoiceThreshold   = 300;   // 开始录音/可打断的触发阈值（越大越不敏感）
    static constexpr int kSilenceThreshold = 200;   // 静音判定阈值（低于此值视为“安静”）
    static constexpr int kSilenceLimitMs   = 1000;  // 静音持续超过该毫秒数 → 判定一句话结束

    // ===== 视觉模型参数 =====
    static constexpr int kInputSize        = 640;   // 检测模型输入尺寸（例如 YOLO 640x640）

    /** 本地 NLU 地址（与 dialog 中 kBackendLocalModels 的 host 一致，用于 DNS 兜底/覆盖） */
    static constexpr const char* kLocalBackendHostname = "124.222.205.168";
    /** 若希望用域名访问但解析失败，可填 IPv4；当前已直连 IP 时可留空 */
    static constexpr const char* kLocalBackendIpOverride = "";

    // ===== 统一监护事件上报 =====
    static constexpr bool kMonitorEventIngestEnabled = true;
    static constexpr const char* kMonitorEventBaseUrl = "http://192.168.110.93:8089";
    static constexpr const char* kMonitorEventPath = "/app/public/monitor/events";
    static constexpr const char* kMonitorEventAccessKey = "xinxing-monitor-key-2026";
    static constexpr long long kMonitorChildId = 2036013232947302402LL;
    static constexpr long long kMonitorDeviceId = 2038596373160538114LL;
    static constexpr int kMonitorEventTimeoutSec = 6;
    static constexpr int kMonitorVisionMinIntervalMs = 12000;
    static constexpr int kMonitorSensorMinIntervalMs = 8000;
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
