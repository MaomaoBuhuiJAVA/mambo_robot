#pragma once
#include "config.hpp"
#include "utils.hpp"
#include "web_server.hpp"
#include "../third_party/json.hpp"
#include <alsa/asoundlib.h>
#include <libwebsockets.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>

using json = nlohmann::json;

namespace mambo {

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

static std::string AsrRecognize(const std::vector<short>& audio_data) {
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
    ci.address   = "124.222.205.168";
    ci.port      = 443;
    ci.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ci.path      = "/asr/";
    ci.host      = ci.address;
    ci.origin    = ci.address;
    ci.protocol  = protocols[0].name;
    ci.userdata  = &ctx;

    struct lws* wsi = lws_client_connect_via_info(&ci);
    if (!wsi) { std::cerr << "[ASR Error] 连接发起失败\n"; lws_context_destroy(lws_ctx); return ""; }

    // 等待结果，最多 4 秒
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
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
static void TtsPlay(const std::string& text, const std::string& alsa_device) {
    // 用 libcurl 直接下载，避免 system(curl) 的 shell 注入风险
    std::string body = "{\"text\":\"" + text + "\",\"character\":\"klee\"}";
    std::string pcm_data = HttpUtils::Post(
        "https://124.222.205.168/tts/",
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
        std::cerr << "[Dialog] 初始化完成（本地 ASR/LLM/TTS）\n";
    }
    void Start() { worker_thread_ = std::thread(&DialogSystem::AudioLoop, this); }
    ~DialogSystem() {
        if (worker_thread_.joinable())  worker_thread_.join();
        if (process_thread_.joinable()) process_thread_.join();
    }
    ChatState GetState() const { return chat_state_.load(); }
    void SetCurrentEmotion(const std::string& e) { std::lock_guard<std::mutex> lk(mtx_); current_emotion_ = e; }
    int GetMicRms() const { return mic_rms_.load(); }

private:
    SerialManager* serial_;
    WebServer*     web_;
    std::thread worker_thread_;
    std::thread process_thread_;  // ProcessInteraction 独立线程
    std::atomic<ChatState> chat_state_;
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

        // 1. ASR
        std::cerr << "[ASR] 开始识别...\n";
        std::string user_text = AsrRecognize(audio_data);
        if (interrupt_) { chat_state_ = ChatState::kWaiting; return; }
        if (user_text.empty()) {
            std::cerr << "[ASR] 识别失败，播放提示\n";
            chat_state_ = ChatState::kSpeaking;
            TtsPlay("星宝没有听清楚，请再说一遍。", AppConfig::kAlsaPlayDevice);
            chat_state_ = ChatState::kWaiting;
            return;
        }
        std::cerr << "[ASR] 识别结果: " << user_text << "\n";

        // 2. LLM
        std::string emotion; { std::lock_guard<std::mutex> lk(mtx_); emotion = current_emotion_; }
        chat_history_.push_back({{"role", "user"}, {"content", "[情绪:" + emotion + "] " + user_text}});

        // 只保留 system + 最近3轮(6条)，避免超出 context 1024 tokens
        std::vector<json> trimmed;
        trimmed.push_back(chat_history_[0]); // system prompt
        int start = std::max(1, (int)chat_history_.size() - 6);
        for (int i = start; i < (int)chat_history_.size(); i++)
            trimmed.push_back(chat_history_[i]);

        json req_body = {
            {"model", "qwen3.5-9b"},
            {"messages", trimmed},
            {"max_tokens", 300},
            {"temperature", 0.7},
            {"chat_template_kwargs", {{"enable_thinking", false}}}
        };
        std::cerr << "[LLM] 请求中...\n";
        std::string llm_res = HttpUtils::Post(
            "https://124.222.205.168/llm/v1/chat/completions",
            req_body.dump(), {"Content-Type: application/json"}, 20);

        if (llm_res.empty() || llm_res.find("<html") != std::string::npos) {
            std::cerr << "[LLM Error] 返回为空或服务器错误\n";
            if (!interrupt_) {
                chat_state_ = ChatState::kSpeaking;
                TtsPlay("星宝现在有点累，请稍后再问我吧。", AppConfig::kAlsaPlayDevice);
            }
            chat_state_ = ChatState::kWaiting; return;
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

            // 4. TTS 播放
            chat_history_.push_back({{"role", "assistant"}, {"content", ai_content}});
            chat_state_ = ChatState::kSpeaking;
            std::cerr << "[TTS] 开始合成: " << reply_text << "\n";
            TtsPlay(reply_text, AppConfig::kAlsaPlayDevice);

        } catch (const std::exception& e) {
            std::cerr << "[Error] 解析失败: " << e.what() << "\n";
        }
        chat_state_ = ChatState::kWaiting;
    }
};
}
