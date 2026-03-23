#pragma once
#include "config.hpp"
#include "utils.hpp"
#include "web_server.hpp"
#include "../third_party/json.hpp"
#include <curl/curl.h>
#include <alsa/asoundlib.h>
#include <libwebsockets.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>

using json = nlohmann::json;

namespace mambo {

enum class NluBackendMode {
    /** 与本地部署完全独立：百度 ASR + 百度 TTS + DeepSeek Chat（仅密钥与公网） */
    kBaiduDeepseek = 0,
    /** 仅本地/自建网关：WebSocket ASR + 自建 LLM + 自建 TTS，不与云端链路混用 */
    kLocalModels   = 1,
};

struct NluBackendConfig {
    const char* asr_host;
    const char* asr_path;
    const char* llm_url;
    const char* llm_model;
    const char* tts_url;
};

static constexpr NluBackendConfig kBackendBaiduDeepseek = {
    "vop.baidu.com/server_api",
    "",
    "https://api.deepseek.com/chat/completions",
    "deepseek-chat",
    "https://tsn.baidu.com/text2audio",
};

static constexpr NluBackendConfig kBackendLocalModels = {
    "124.222.205.168", "/asr/",
    "https://124.222.205.168/llm/v1/chat/completions",
    "qwen3.5-9b",
    "https://124.222.205.168/tts/",
};

static const char* BackendModeName(NluBackendMode m) {
    return m == NluBackendMode::kLocalModels ? "local" : "baidu_deepseek";
}

static NluBackendMode OtherBackendMode(NluBackendMode m) {
    return m == NluBackendMode::kLocalModels ? NluBackendMode::kBaiduDeepseek : NluBackendMode::kLocalModels;
}

static const NluBackendConfig& GetBackendConfig(NluBackendMode m) {
    return m == NluBackendMode::kLocalModels ? kBackendLocalModels : kBackendBaiduDeepseek;
}

static std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(k[(v >> 18) & 63]);
        out.push_back(k[(v >> 12) & 63]);
        out.push_back(k[(v >> 6) & 63]);
        out.push_back(k[v & 63]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(k[(v >> 18) & 63]);
        if (i + 1 < len) {
            v |= uint32_t(data[i + 1]) << 8;
            out.push_back(k[(v >> 12) & 63]);
            out.push_back(k[(v >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(k[(v >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

static std::string BaiduGetAccessToken(const char* api_key, const char* secret_key) {
    if (!api_key || !*api_key || !secret_key || !*secret_key) return "";
    std::string body = std::string("grant_type=client_credentials&client_id=") + api_key
                     + "&client_secret=" + secret_key;
    std::string res = HttpUtils::Post(
        "https://aip.baidubce.com/oauth/2.0/token",
        body, {"Content-Type: application/x-www-form-urlencoded"}, 8);
    if (res.empty()) return "";
    try {
        auto j = json::parse(res);
        if (j.contains("access_token")) return j["access_token"].get<std::string>();
    } catch (...) {}
    return "";
}

static json BaiduAsrProbe(const std::string& access_token) {
    json out = {{"ok", false}};
    if (access_token.empty()) { out["error"] = "empty_token"; return out; }
    std::vector<int16_t> pcm(16000, 0);
    std::string speech = Base64Encode(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    json req = {
        {"format", "pcm"},
        {"rate", 16000},
        {"channel", 1},
        {"cuid", "mambo_robot"},
        {"token", access_token},
        {"speech", speech},
        {"len", (int)(pcm.size() * sizeof(int16_t))}
    };
    std::string res = HttpUtils::Post(
        "https://vop.baidu.com/server_api",
        req.dump(), {"Content-Type: application/json"}, 12);
    if (res.empty()) { out["error"] = "empty_response"; return out; }
    try {
        auto j = json::parse(res);
        out["raw_err_no"] = j.value("err_no", -1);
        out["raw_err_msg"] = j.value("err_msg", "");
        out["ok"] = j.contains("err_no");
        if (j.contains("result") && j["result"].is_array() && !j["result"].empty())
            out["result0"] = j["result"][0].get<std::string>();
    } catch (...) {
        out["error"] = "json_parse_fail";
    }
    return out;
}

/** 缓存百度 access_token，减少 OAuth 请求 */
static std::string BaiduAccessTokenCached() {
    static std::mutex mtx;
    static std::string tok;
    static std::chrono::steady_clock::time_point deadline{};
    std::lock_guard<std::mutex> lk(mtx);
    auto now = std::chrono::steady_clock::now();
    if (!tok.empty() && now < deadline) return tok;

    if (!AppConfig::kBaiduApiKey || !*AppConfig::kBaiduApiKey
        || !AppConfig::kBaiduSecretKey || !*AppConfig::kBaiduSecretKey)
        return "";

    std::string body = std::string("grant_type=client_credentials&client_id=") + AppConfig::kBaiduApiKey
                     + "&client_secret=" + AppConfig::kBaiduSecretKey;
    std::string res = HttpUtils::Post(
        "https://aip.baidubce.com/oauth/2.0/token",
        body, {"Content-Type: application/x-www-form-urlencoded"}, 12);
    if (res.empty()) return "";
    try {
        auto j = json::parse(res);
        if (!j.contains("access_token")) return "";
        tok = j["access_token"].get<std::string>();
        int expires_in = j.value("expires_in", 2592000);
        if (expires_in < 600) expires_in = 600;
        deadline = now + std::chrono::seconds(expires_in - 300);
        return tok;
    } catch (...) {}
    return "";
}

/** 百度短语音识别标准版：PCM 16k 单声道，整段 POST */
static std::string BaiduAsrRecognizePcm(const std::vector<short>& audio_data, const std::string& access_token) {
    if (audio_data.empty() || access_token.empty()) return "";
    std::string speech = Base64Encode(reinterpret_cast<const uint8_t*>(audio_data.data()),
                                      audio_data.size() * sizeof(int16_t));
    json req = {
        {"format", "pcm"},
        {"rate", 16000},
        {"channel", 1},
        {"cuid", "mambo_robot"},
        {"token", access_token},
        {"speech", speech},
        {"len", (int)(audio_data.size() * sizeof(int16_t))},
        {"dev_pid", 1537}
    };
    std::string res = HttpUtils::Post(
        "https://vop.baidu.com/server_api",
        req.dump(), {"Content-Type: application/json"}, 60);
    if (res.empty()) {
        std::cerr << "[ASR/Baidu] HTTP 空响应\n";
        return "";
    }
    try {
        auto j = json::parse(res);
        int err_no = j.value("err_no", -1);
        if (err_no != 0) {
            std::cerr << "[ASR/Baidu] err_no=" << err_no << " err_msg=" << j.value("err_msg", "") << "\n";
            return "";
        }
        if (j.contains("result") && j["result"].is_array() && !j["result"].empty()) {
            std::string text = j["result"][0].get<std::string>();
            while (!text.empty() && text.front() == ' ') text.erase(0, 1);
            while (!text.empty() && text.back() == ' ') text.pop_back();
            return text;
        }
    } catch (...) {}
    std::cerr << "[ASR/Baidu] 响应解析失败\n";
    return "";
}

static json DeepseekChatProbe(const char* api_key) {
    json out = {{"ok", false}};
    if (!api_key || !*api_key) { out["error"] = "empty_key"; return out; }
    json req = {
        {"model", "deepseek-chat"},
        {"messages", json::array({json{{"role","user"},{"content","请回复一句：DeepSeek 联通测试成功"}}})},
        {"stream", false},
        {"max_tokens", 50}
    };
    std::string res = HttpUtils::Post(
        "https://api.deepseek.com/chat/completions",
        req.dump(),
        {"Content-Type: application/json", std::string("Authorization: Bearer ") + api_key},
        12);
    if (res.empty()) { out["error"] = "empty_response"; return out; }
    try {
        auto j = json::parse(res);
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
            out["error"] = "no_choices";
            return out;
        }
        std::string content = j["choices"][0]["message"].value("content", "");
        out["ok"] = !content.empty();
        if (content.size() > 120) content = content.substr(0, 120);
        out["reply"] = content;
    } catch (...) {
        out["error"] = "json_parse_fail";
    }
    return out;
}

// ===== ASR WebSocket 客户端 =====
struct AsrWsCtx {
    const std::vector<short>* audio_data = nullptr;
    size_t sent_bytes = 0;
    bool init_sent = false;
    bool end_sent  = false;
    std::string result;
    bool done = false;
    bool error = false;
    std::mutex mtx;
    std::condition_variable cv;
};

static int AsrLwsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                          void* user, void* in, size_t len) {
    AsrWsCtx* ctx = (AsrWsCtx*)lws_wsi_user(wsi);
    if (!ctx) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        std::cerr << "[ASR] WebSocket 连接成功\n";
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        // 第一次：发初始化 JSON
        if (ctx->sent_bytes == 0 && !ctx->init_sent) {
            ctx->init_sent = true;
            std::string init = R"({"mode":"2pass","wav_name":"rec","is_speaking":true,"wav_format":"pcm","chunk_size":[5,10,5],"itn":true,"audio_fs":16000})";
            std::vector<uint8_t> buf(LWS_PRE + init.size());
            memcpy(buf.data() + LWS_PRE, init.data(), init.size());
            lws_write(wsi, buf.data() + LWS_PRE, init.size(), LWS_WRITE_TEXT);
            lws_callback_on_writable(wsi);
            break;
        }
        // 发音频数据
        const size_t CHUNK = 3200;
        const uint8_t* base = (const uint8_t*)ctx->audio_data->data();
        size_t total = ctx->audio_data->size() * 2;
        if (ctx->sent_bytes < total) {
            size_t to_send = std::min(CHUNK, total - ctx->sent_bytes);
            std::vector<uint8_t> buf(LWS_PRE + to_send);
            memcpy(buf.data() + LWS_PRE, base + ctx->sent_bytes, to_send);
            lws_write(wsi, buf.data() + LWS_PRE, to_send, LWS_WRITE_BINARY);
            ctx->sent_bytes += to_send;
            lws_callback_on_writable(wsi);
        } else if (!ctx->end_sent) {
            // 发结束标志
            ctx->end_sent = true;
            std::string end_msg = R"({"is_speaking":false})";
            std::vector<uint8_t> buf(LWS_PRE + end_msg.size());
            memcpy(buf.data() + LWS_PRE, end_msg.data(), end_msg.size());
            lws_write(wsi, buf.data() + LWS_PRE, end_msg.size(), LWS_WRITE_TEXT);
            std::cerr << "[ASR] 已发送结束标志，等待结果...\n";
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        std::string msg((char*)in, len);
        std::cerr << "[ASR] 收到: " << msg << "\n";
        try {
            auto j = json::parse(msg);
            std::string text;
            if (j.contains("text")) text = j["text"].get<std::string>();

            // 清理 <|xx|> 标签
            while (true) {
                auto s = text.find("<|");
                if (s == std::string::npos) break;
                auto e = text.find("|>", s);
                if (e == std::string::npos) break;
                text.erase(s, e - s + 2);
            }
            // 去首尾空格
            while (!text.empty() && text.front() == ' ') text.erase(0, 1);
            while (!text.empty() && text.back()  == ' ') text.pop_back();

            if (!text.empty()) {
                std::lock_guard<std::mutex> lk(ctx->mtx);
                ctx->result = text; // 持续更新，取最新非空结果
            }

            // is_final=true 表示服务端处理完毕
            if (j.value("is_final", false)) {
                std::lock_guard<std::mutex> lk(ctx->mtx);
                ctx->done = true;
                ctx->cv.notify_all();
            }
        } catch (...) {}
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        std::cerr << "[ASR Error] 连接失败: " << (in ? (char*)in : "unknown") << "\n";
        { std::lock_guard<std::mutex> lk(ctx->mtx); ctx->error = true; ctx->done = true; ctx->cv.notify_all(); }
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        std::cerr << "[ASR] 连接关闭\n";
        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            ctx->done = true; // 连接关闭时，用已收到的最新结果
            ctx->cv.notify_all();
        }
        break;

    default: break;
    }
    return 0;
}

static std::string AsrRecognize(const std::vector<short>& audio_data, const char* asr_host, const char* asr_path) {
    AsrWsCtx ctx;
    ctx.audio_data = &audio_data;

    static const struct lws_protocols protocols[] = {
        { "asr", AsrLwsCallback, 0, 65536, 0, nullptr, 0 },
        { nullptr, nullptr, 0, 0 }
    };

    struct lws_context_creation_info info = {};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context* lws_ctx = lws_create_context(&info);
    if (!lws_ctx) { std::cerr << "[ASR Error] 无法创建 lws context\n"; return ""; }

    struct lws_client_connect_info ci = {};
    ci.context   = lws_ctx;
    std::string connect_ip = HttpUtils::ResolveIpv4Robust(asr_host);
    if (connect_ip.empty()) {
        std::cerr << "[ASR] DNS 全失败，尝试 libwebsockets 使用域名直连（依赖系统解析） host=" << asr_host << "\n";
        ci.address = asr_host;
    } else {
        std::cerr << "[ASR] 解析 " << asr_host << " -> " << connect_ip << "\n";
        ci.address = connect_ip.c_str();
    }
    ci.port      = 443;
    ci.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ci.path      = asr_path;
    ci.host      = asr_host;
    ci.origin    = asr_host;
    ci.protocol  = protocols[0].name;
    ci.userdata  = &ctx;

    struct lws* wsi = lws_client_connect_via_info(&ci);
    if (!wsi) {
        std::cerr << "[ASR Error] 连接发起失败 host=" << asr_host << " path=" << asr_path << "\n";
        lws_context_destroy(lws_ctx);
        return "";
    }

    // 等待结果（DNS/ TLS 较慢时放宽）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        lws_service(lws_ctx, 50);
        std::unique_lock<std::mutex> lk(ctx.mtx);
        if (ctx.done) break;
    }

    lws_context_destroy(lws_ctx);
    if (ctx.error || ctx.result.empty()) {
        std::cerr << "[ASR Error] 识别失败或无结果\n";
        return "";
    }
    return ctx.result;
}

// ===== TTS（libcurl 下载 PCM，aplay 播放）=====
static void TtsPlay(const std::string& text, const std::string& alsa_device, const char* tts_url) {
    // 用 libcurl 直接下载，避免 system(curl) 的 shell 注入风险
    std::string body = "{\"text\":\"" + text + "\",\"character\":\"klee\"}";
    std::string pcm_data = HttpUtils::Post(
        tts_url,
        body, {"Content-Type: application/json"}, 15);

    if (pcm_data.size() < 100) {
        std::cerr << "[TTS Error] PCM 数据过小 (" << pcm_data.size() << " bytes)\n";
        return;
    }
    std::cerr << "[TTS] 收到 " << pcm_data.size() << " bytes PCM\n";

    // 写入临时文件
    FILE* f = fopen("/tmp/tts.pcm", "wb");
    if (!f) { std::cerr << "[TTS Error] 无法写入 /tmp/tts.pcm\n"; return; }
    fwrite(pcm_data.data(), 1, pcm_data.size(), f);
    fclose(f);

    std::string play_cmd = "sox -t raw -r 24000 -e signed -b 16 -c 1 /tmp/tts.pcm "
                           "-t raw - vol 0.4 | "
                           "aplay -q -D " + alsa_device +
                           " -f S16_LE -r 24000 -c 1 >/dev/null 2>&1";
    system(play_cmd.c_str());
}

/** 百度语音合成（短文本）：PCM 16k 16bit mono（aue=4），与自建 TTS 无关 */
static void TtsPlayBaiduCloud(const std::string& text, const std::string& alsa_device) {
    std::string tok = BaiduAccessTokenCached();
    if (tok.empty()) {
        std::cerr << "[TTS/Baidu] 无 access_token\n";
        return;
    }
    CURL* curl = curl_easy_init();
    if (!curl) return;
    char* esc_tex = curl_easy_escape(curl, text.c_str(), (int)text.size());
    std::string url = std::string("https://tsn.baidu.com/text2audio?tex=") + esc_tex
                    + "&tok=" + tok
                    + "&cuid=mambo_robot&ctp=1&lan=zh&spd=5&pit=5&vol=5&per=4&aue=4";
    curl_free(esc_tex);
    curl_easy_cleanup(curl);

    std::string pcm = HttpUtils::Get(url, 45);
    if (pcm.size() < 64) {
        std::cerr << "[TTS/Baidu] 响应过短 size=" << pcm.size() << "\n";
        return;
    }
    if (pcm[0] == '{') {
        std::cerr << "[TTS/Baidu] API 错误: " << pcm.substr(0, 400) << "\n";
        return;
    }
    FILE* f = fopen("/tmp/tts_baidu.pcm", "wb");
    if (!f) { std::cerr << "[TTS/Baidu] 无法写入 /tmp/tts_baidu.pcm\n"; return; }
    fwrite(pcm.data(), 1, pcm.size(), f);
    fclose(f);

    std::string play_cmd = "sox -t raw -r 16000 -e signed -b 16 -c 1 /tmp/tts_baidu.pcm "
                           "-t raw - vol 0.5 | "
                           "aplay -q -D " + alsa_device +
                           " -f S16_LE -r 16000 -c 1 >/dev/null 2>&1";
    system(play_cmd.c_str());
}

// ===== 主对话系统 =====
class DialogSystem {
public:
    DialogSystem(SerialManager* serial, WebServer* web)
        : serial_(serial), web_(web), chat_state_(ChatState::kWaiting) {
        chat_history_.push_back({{"role", "system"}, {"content",
            "你是星宝，陪伴孤独症小朋友的桌面机器人。温柔简短，直接执行请求不反问。"
            "只返回JSON，不含Markdown：{\"reply\":\"回复\",\"emotion\":\"情绪\",\"action\":\"动作\",\"duration\":秒}"
            "emotion: ZhongXing/KaiXin/JingYa/NanGuo/ShengQi/KongJu"
            "action: stop/forward/backward/left/right，duration最大10秒"
            "转圈=left,5秒；前进=forward,2秒；后退=backward,2秒；左/右转=left/right,3秒"
        }});
        std::cerr << "[Dialog] 两条独立链路: baidu=百度ASR+百度TTS+DeepSeek；local=124.222.205.168 全链路，互不切换\n";
    }
    void Start() { worker_thread_ = std::thread(&DialogSystem::AudioLoop, this); }
    ~DialogSystem() {
        if (worker_thread_.joinable())  worker_thread_.join();
        if (process_thread_.joinable()) process_thread_.join();
    }
    ChatState GetState() const { return chat_state_.load(); }
    const char* GetBackendName() const {
        return backend_mode_.load() == NluBackendMode::kLocalModels ? "local" : "baidu";
    }
    const char* GetBackendSelectedName() const { return GetBackendName(); }
    const char* GetBackendEffectiveName() const { return GetBackendSelectedName(); }
    const char* GetSelectedAsrHost() const { return GetBackendConfig(backend_mode_.load()).asr_host; }
    const char* GetSelectedLlmUrl() const { return GetBackendConfig(backend_mode_.load()).llm_url; }
    const char* GetSelectedTtsUrl() const { return GetBackendConfig(backend_mode_.load()).tts_url; }
    const char* GetEffectiveAsrHost() const { return GetBackendConfig(effective_backend_.load()).asr_host; }
    const char* GetEffectiveLlmUrl() const { return GetBackendConfig(effective_backend_.load()).llm_url; }
    const char* GetEffectiveTtsUrl() const { return GetBackendConfig(effective_backend_.load()).tts_url; }

    /** 供 HTTP /backend 与控制台共用：当前 NLU 后端状态（与 /status SSE 字段一致） */
    json BackendStatusJson() const {
        json j;
        j["ok"]                      = true;
        j["backend_selected"]        = GetBackendSelectedName();
        j["backend_effective"]       = GetBackendEffectiveName();
        j["backend_selected_asr_host"] = GetSelectedAsrHost();
        j["backend_selected_llm_url"]  = GetSelectedLlmUrl();
        j["backend_selected_tts_url"]  = GetSelectedTtsUrl();
        j["backend_effective_asr_host"] = GetEffectiveAsrHost();
        j["backend_effective_llm_url"]  = GetEffectiveLlmUrl();
        j["backend_effective_tts_url"]  = GetEffectiveTtsUrl();
        return j;
    }

    /**
     * 网页一键切换：mode 为空或 "status" 仅查询；否则切换为 local / baidu / toggle。
     * 返回 JSON 字符串（application/json）。
     */
    std::string HandleBackendHttp(const std::string& mode_raw) {
        std::string m = mode_raw;
        auto trim = [](std::string& x) {
            while (!x.empty() && (x.back() == '\n' || x.back() == '\r' || x.back() == ' ' || x.back() == '\t')) x.pop_back();
            size_t i = 0;
            while (i < x.size() && (x[i] == ' ' || x[i] == '\t')) i++;
            if (i) x.erase(0, i);
        };
        trim(m);
        for (char& ch : m) ch = (char)std::tolower((unsigned char)ch);

        if (m.empty() || m == "status") return BackendStatusJson().dump();

        if (m == "local" || m == "1") {
            backend_mode_.store(NluBackendMode::kLocalModels);
        } else if (m == "baidu" || m == "2" || m == "baidu_deepseek") {
            backend_mode_.store(NluBackendMode::kBaiduDeepseek);
        } else if (m == "toggle" || m == "t") {
            backend_mode_.store(OtherBackendMode(backend_mode_.load()));
        } else {
            json j = BackendStatusJson();
            j["ok"] = false;
            j["error"] = "unknown_mode";
            return j.dump();
        }
        std::cerr << "[Web] NLU 后端切换为: " << BackendModeName(backend_mode_.load()) << "\n";
        return BackendStatusJson().dump();
    }

    bool IsMuted() const { return muted_.load(); }
    void SetMuted(bool m) { muted_.store(m); }
    void ToggleMuted() { muted_.store(!muted_.load()); }
    std::string RunBaiduDeepseekDiagJson() const {
        json j;
        j["baidu_token_ok"] = false;
        j["baidu_asr"] = json::object();
        j["deepseek"] = json::object();

        std::string token = BaiduGetAccessToken(AppConfig::kBaiduApiKey, AppConfig::kBaiduSecretKey);
        j["baidu_token_ok"] = !token.empty();
        j["baidu_asr"] = token.empty() ? json{{"ok", false}, {"error", "token_failed"}} : BaiduAsrProbe(token);
        j["deepseek"] = DeepseekChatProbe(AppConfig::kDeepseekApiKey);
        j["ok"] = j["baidu_token_ok"].get<bool>() && j["baidu_asr"].value("ok", false) && j["deepseek"].value("ok", false);
        return j.dump();
    }
    void HandleConsoleCommand(const std::string& line) {
        std::string s = line;
        auto trim = [](std::string& x) {
            while (!x.empty() && (x.back() == '\n' || x.back() == '\r' || x.back() == ' ' || x.back() == '\t')) x.pop_back();
            size_t i = 0;
            while (i < x.size() && (x[i] == ' ' || x[i] == '\t')) i++;
            if (i) x.erase(0, i);
        };
        trim(s);
        if (s.empty()) return;

        for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);

        auto mode = backend_mode_.load();

        if (s == "m" || s == "mute" || s == "mute?" || s == "mute ?" || s == "silent" || s == "silent?") {
            ToggleMuted();
            std::cerr << "[Console] 静音=" << (muted_.load() ? "ON" : "OFF") << "\n";
            return;
        }
        if (s == "diag" || s == "diag baidu" || s == "diag deepseek" || s == "diag baidu_deepseek") {
            std::string r = RunBaiduDeepseekDiagJson();
            std::cerr << "[Diag] " << r << "\n";
            return;
        }
        if (s == "unmute") {
            muted_.store(false);
            std::cerr << "[Console] 静音=OFF\n";
            return;
        }
        if (s == "mute on" || s == "mute 1" || s == "mute true") {
            muted_.store(true);
            std::cerr << "[Console] 静音=ON\n";
            return;
        }
        if (s == "mute off" || s == "mute 0" || s == "mute false") {
            muted_.store(false);
            std::cerr << "[Console] 静音=OFF\n";
            return;
        }

        if (s == "?" || s == "help" || s == "backend" || s == "backend?" || s == "backend ?") {
            std::cerr << "[Console] 当前 backend=" << BackendModeName(mode)
                      << "，输入: `1`(local) / `2`(baidu) / `toggle` 或 `backend baidu` / `backend local`\n";
            return;
        }

        if (s == "1" || s == "local" || s.find("local") != std::string::npos) {
            backend_mode_.store(NluBackendMode::kLocalModels);
            std::cerr << "[Console] NLU 后端切换为: local\n";
            return;
        }

        if (s == "2" || s == "baidu" || s.find("baidu") != std::string::npos || s.find("deepseek") != std::string::npos) {
            backend_mode_.store(NluBackendMode::kBaiduDeepseek);
            std::cerr << "[Console] NLU 后端切换为: baidu_deepseek\n";
            return;
        }

        if (s == "toggle" || s == "t") {
            backend_mode_.store(mode == NluBackendMode::kLocalModels ? NluBackendMode::kBaiduDeepseek : NluBackendMode::kLocalModels);
            std::cerr << "[Console] NLU 后端切换为: " << BackendModeName(backend_mode_.load()) << "\n";
            return;
        }

        std::cerr << "[Console] 未识别命令: `" << line << "`，输入 `1` / `2` / `toggle` 或 `backend ?`。\n";
    }
    const char* GetCurrentTtsUrl() const {
        const auto mode = backend_mode_.load();
        return (mode == NluBackendMode::kLocalModels) ? kBackendLocalModels.tts_url
                                                     : kBackendBaiduDeepseek.tts_url;
    }

    /** 警报等异步播报：按当前后端走百度 TTS 或自建 TTS，互不混用 */
    void PlayAlertTts(const std::string& text) {
        if (IsMuted()) return;
        if (backend_mode_.load() == NluBackendMode::kBaiduDeepseek) {
            std::string alsa = AppConfig::kAlsaPlayDevice;
            std::thread([text, alsa]() { TtsPlayBaiduCloud(text, alsa); }).detach();
        } else {
            std::thread([text]() {
                TtsPlay(text, AppConfig::kAlsaPlayDevice, kBackendLocalModels.tts_url);
            }).detach();
        }
    }
    void SetCurrentEmotion(const std::string& e) { std::lock_guard<std::mutex> lk(mtx_); current_emotion_ = e; }
    int GetMicRms() const { return mic_rms_.load(); }

private:
    SerialManager* serial_;
    WebServer*     web_;
    std::thread worker_thread_;
    std::thread process_thread_;  // ProcessInteraction 独立线程
    std::atomic<ChatState> chat_state_;
    std::atomic<NluBackendMode> backend_mode_{NluBackendMode::kBaiduDeepseek};
    std::atomic<NluBackendMode> effective_backend_{NluBackendMode::kBaiduDeepseek};
    std::atomic<bool> muted_{true};
    std::atomic<bool> interrupt_{false};  // 打断标志
    std::vector<json> chat_history_;
    std::mutex mtx_;
    std::string current_emotion_ = "ZhongXing";
    std::atomic<int> mic_rms_{0};

    void AudioLoop() {
        snd_pcm_t* h_rec;
        int err = snd_pcm_open(&h_rec, AppConfig::kAlsaRecDevice, SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) { std::cerr << "[Audio Error] 无法打开录音设备: " << snd_strerror(err) << "\n"; return; }
        snd_pcm_set_params(h_rec, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 32000);
        short buffer[512]; std::vector<short> record_buf;
        bool is_recording = false; int silence_ms = 0;

        while (true) {
            // thinking/speaking 时持续监听，检测到声音就打断
            if (chat_state_ == ChatState::kSpeaking ||
                chat_state_ == ChatState::kThinking) {
                int res = snd_pcm_readi(h_rec, buffer, 512);
                if (res > 0) {
                    long long sum = 0;
                    for (int i = 0; i < res; i++) sum += std::abs(buffer[i]);
                    int rms = sum / res;
                    mic_rms_ = rms;
                    // speaking 时阈值更高（避免扬声器回声误触发），thinking 时用正常阈值
                    int threshold = (chat_state_ == ChatState::kSpeaking)
                                    ? AppConfig::kVoiceThreshold * 4
                                    : AppConfig::kVoiceThreshold;
                    if (rms > threshold) {
                        std::cerr << "[Audio] 检测到打断 rms=" << rms << " threshold=" << threshold << "\n";
                        interrupt_ = true;
                        system("pkill -f 'aplay' 2>/dev/null; pkill -f 'sox' 2>/dev/null");
                        // thinking 状态：LLM 请求阻塞无法 join，直接 detach 让它自己结束
                        // speaking 状态：TTS 已被 pkill 杀掉，join 很快
                        if (chat_state_ == ChatState::kSpeaking) {
                            if (process_thread_.joinable()) process_thread_.join();
                        } else {
                            if (process_thread_.joinable()) process_thread_.detach();
                        }
                        // 等扬声器声音消散，清空麦克风缓冲区
                        usleep(500000); // 500ms
                        snd_pcm_drop(h_rec);
                        snd_pcm_prepare(h_rec);
                        chat_state_ = ChatState::kListening;
                        interrupt_ = false;
                        is_recording = true;
                        record_buf.clear();
                        silence_ms = 0;
                    }
                }
                usleep(10000);
                continue;
            }
            int res = snd_pcm_readi(h_rec, buffer, 512);
            if (res == -EPIPE) { snd_pcm_prepare(h_rec); continue; }
            if (res < 0) { usleep(10000); continue; }

            long long sum = 0;
            for (int i = 0; i < 512; i++) sum += std::abs(buffer[i]);
            int rms = sum / 512;
            mic_rms_ = rms;

            if (!is_recording && rms > AppConfig::kVoiceThreshold) {
                chat_state_ = ChatState::kListening;
                is_recording = true;
                record_buf.clear();
                std::cerr << "[Audio] 开始录音 (rms=" << rms << ")\n";
            }
            if (is_recording) {
                record_buf.insert(record_buf.end(), buffer, buffer + 512);
                silence_ms = (rms < AppConfig::kSilenceThreshold) ? silence_ms + 32 : 0;
                if (silence_ms > AppConfig::kSilenceLimitMs) {
                    is_recording = false; silence_ms = 0;
                    std::cerr << "[Audio] 录音结束，" << record_buf.size() << " 采样\n";
                    if (record_buf.size() >= 16000) {
                        // 等上一次处理完
                        if (process_thread_.joinable()) process_thread_.join();
                        auto buf_copy = record_buf;
                        process_thread_ = std::thread([this, buf_copy]() {
                            ProcessInteraction(buf_copy);
                        });
                    } else {
                        std::cerr << "[Audio] 录音太短，丢弃\n";
                        chat_state_ = ChatState::kWaiting;
                    }
                    snd_pcm_prepare(h_rec);
                }
            }
        }
    }

    void ProcessInteraction(const std::vector<short>& audio_data) {
        chat_state_ = ChatState::kThinking;
        const NluBackendMode selected_mode = backend_mode_.load();
        effective_backend_.store(selected_mode);
        const NluBackendConfig* cfg = &GetBackendConfig(selected_mode);

        // 1. ASR（两条链路完全独立，失败不互相兜底）
        std::string user_text;
        if (selected_mode == NluBackendMode::kBaiduDeepseek) {
            std::cerr << "[ASR] 百度语音识别 (REST vop)...\n";
            std::string tok = BaiduAccessTokenCached();
            if (tok.empty()) {
                std::cerr << "[ASR/Baidu] 获取 access_token 失败\n";
            } else {
                user_text = BaiduAsrRecognizePcm(audio_data, tok);
            }
        } else {
            std::cerr << "[ASR] 本地/自建 WebSocket ASR host=" << cfg->asr_host << "\n";
            user_text = AsrRecognize(audio_data, cfg->asr_host, cfg->asr_path);
        }
        if (interrupt_) { chat_state_ = ChatState::kWaiting; return; }
        if (user_text.empty()) {
            std::cerr << "[ASR] 识别失败，播放提示\n";
            if (!muted_.load() && !interrupt_) {
                chat_state_ = ChatState::kSpeaking;
                if (selected_mode == NluBackendMode::kBaiduDeepseek)
                    TtsPlayBaiduCloud("星宝没有听清楚，请再说一遍。", AppConfig::kAlsaPlayDevice);
                else
                    TtsPlay("星宝没有听清楚，请再说一遍。", AppConfig::kAlsaPlayDevice, cfg->tts_url);
            }
            chat_state_ = ChatState::kWaiting;
            return;
        }
        std::cerr << "[ASR] 识别结果: " << user_text << "\n";

        // 2. LLM
        std::string emotion; { std::lock_guard<std::mutex> lk(mtx_); emotion = current_emotion_; }
        chat_history_.push_back({{"role", "user"}, {"content", "[情绪:" + emotion + "] " + user_text}});

        std::vector<json> trimmed;
        trimmed.push_back(chat_history_[0]);
        int start = std::max(1, (int)chat_history_.size() - 6);
        for (int i = start; i < (int)chat_history_.size(); i++)
            trimmed.push_back(chat_history_[i]);

        std::string llm_res;
        if (selected_mode == NluBackendMode::kBaiduDeepseek) {
            json req_body = {
                {"model", "deepseek-chat"},
                {"messages", trimmed},
                {"max_tokens", 300},
                {"temperature", 0.7}
            };
            std::cerr << "[LLM] DeepSeek 请求中...\n";
            llm_res = HttpUtils::Post(
                "https://api.deepseek.com/chat/completions",
                req_body.dump(),
                {"Content-Type: application/json", std::string("Authorization: Bearer ") + AppConfig::kDeepseekApiKey},
                60);
        } else {
            json req_body = {
                {"model", cfg->llm_model},
                {"messages", trimmed},
                {"max_tokens", 300},
                {"temperature", 0.7},
                {"chat_template_kwargs", {{"enable_thinking", false}}}
            };
            std::cerr << "[LLM] 自建接口请求中...\n";
            llm_res = HttpUtils::Post(
                cfg->llm_url,
                req_body.dump(), {"Content-Type: application/json"}, 20);
        }

        if (llm_res.empty() || llm_res.find("<html") != std::string::npos) {
            std::cerr << "[LLM Error] 返回为空或服务器错误\n";
            if (!muted_.load() && !interrupt_) {
                chat_state_ = ChatState::kSpeaking;
                if (selected_mode == NluBackendMode::kBaiduDeepseek)
                    TtsPlayBaiduCloud("星宝现在有点累，请稍后再问我吧。", AppConfig::kAlsaPlayDevice);
                else
                    TtsPlay("星宝现在有点累，请稍后再问我吧。", AppConfig::kAlsaPlayDevice, cfg->tts_url);
            }
            chat_state_ = ChatState::kWaiting;
            return;
        }
        std::cerr << "[LLM raw] " << llm_res.substr(0, 400) << "\n";

        try {
            auto llm_json = json::parse(llm_res);
            if (!llm_json.contains("choices")) {
                std::cerr << "[LLM Error] 无 choices 字段\n";
                chat_state_ = ChatState::kWaiting; return;
            }
            std::string ai_content;
            auto& msg = llm_json["choices"][0]["message"];
            if (msg["content"].is_null()) {
                std::cerr << "[LLM Error] content 为 null\n";
                chat_state_ = ChatState::kWaiting; return;
            }
            ai_content = msg["content"].get<std::string>();
            std::cerr << "[LLM] content: " << ai_content << "\n";

            // 清理 Markdown 标记
            if (ai_content.find("```") != std::string::npos) {
                auto start = ai_content.find("{");
                auto end   = ai_content.rfind("}");
                if (start != std::string::npos && end != std::string::npos)
                    ai_content = ai_content.substr(start, end - start + 1);
            }

            auto ai_json = json::parse(ai_content);
            std::string reply_text = ai_json["reply"];
            std::string action     = ai_json.value("action", "stop");
            std::string emotion    = ai_json.value("emotion", "ZhongXing");
            float duration         = std::min(ai_json.value("duration", 0.0f), 10.0f);
            std::cerr << "[LLM] reply=" << reply_text << " emotion=" << emotion
                      << " action=" << action << " duration=" << duration << "\n";

            // 推送表情到前端
            if (web_) web_->PushEyeData(0, 0, emotion);

            // 3. 动作心跳
            if (serial_ && action != "stop" && duration > 0) {
                std::thread([this, action, duration]() {
                    auto end_t = std::chrono::steady_clock::now()
                                 + std::chrono::milliseconds((int)(duration * 1000));
                    while (std::chrono::steady_clock::now() < end_t) {
                        serial_->SendCommand(action);
                        usleep(150000);
                    }
                    serial_->SendCommand("stop");
                }).detach();
            }

            // 4. TTS（百度链路走百度合成；本地链路走自建）
            chat_history_.push_back({{"role", "assistant"}, {"content", ai_content}});
            if (!muted_.load() && !interrupt_) {
                chat_state_ = ChatState::kSpeaking;
                std::cerr << "[TTS] 开始合成: " << reply_text << "\n";
                if (selected_mode == NluBackendMode::kBaiduDeepseek)
                    TtsPlayBaiduCloud(reply_text, AppConfig::kAlsaPlayDevice);
                else
                    TtsPlay(reply_text, AppConfig::kAlsaPlayDevice, cfg->tts_url);
            }

        } catch (const std::exception& e) {
            std::cerr << "[Error] 解析失败: " << e.what() << "\n";
        }
        chat_state_ = ChatState::kWaiting;
    }
};
}
