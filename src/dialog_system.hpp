#pragma once
#include "config.hpp"
#include "utils.hpp"
#include "../third_party/json.hpp"
#include <alsa/asoundlib.h>
#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

using json = nlohmann::json;

namespace mambo {
class DialogSystem {
public:
    DialogSystem(SerialManager* serial) : serial_(serial), chat_state_(ChatState::kWaiting) {
        RefreshBaiduToken();
        chat_history_.push_back({{"role", "system"}, {"content",
            "你是一个叫曼波的桌面机器人，性格活泼可爱，口头禅'曼波~'。回答简短口语化。"
            "你可以控制底盘移动，也能表达情绪。"
            "你必须且只能返回合法JSON，绝对不要包含Markdown标记(如```json)。"
            "格式：{\"reply\":\"回复话语\",\"emotion\":\"情绪\",\"action\":\"动作\",\"duration\":秒数}"
            "emotion可选值：ZhongXing/KaiXin/JingYa/NanGuo/ShengQi/YanWu/KongJu/MiMang"
            "action可选值：forward/backward/left/right/spin/stop"
            "spin表示原地转圈(left持续执行)。duration为动作持续秒数(0=不动,最大10)。"
            "示例：用户说'往前走'→{\"reply\":\"好的曼波~\",\"emotion\":\"KaiXin\",\"action\":\"forward\",\"duration\":2}"
        }});
    }

    void Start() { worker_thread_ = std::thread(&DialogSystem::AudioLoop, this); }
    ~DialogSystem() { if (worker_thread_.joinable()) worker_thread_.join(); }
    ChatState GetState() const { return chat_state_.load(); }
    void SetCurrentEmotion(const std::string& e) {
        std::lock_guard<std::mutex> lk(mtx_);
        current_emotion_ = e;
    }

private:
    SerialManager* serial_;
    std::thread worker_thread_;
    std::atomic<ChatState> chat_state_;
    std::string baidu_token_;
    std::vector<json> chat_history_;
    std::mutex mtx_;
    std::string current_emotion_ = "ZhongXing";

    void RefreshBaiduToken() {
        std::string url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id="
            + std::string(AppConfig::kBaiduApiKey) + "&client_secret=" + AppConfig::kBaiduSecretKey;
        try { baidu_token_ = json::parse(HttpUtils::Post(url, "", {}))["access_token"]; } catch (...) {}
    }

    void SpeakText(const std::string& text) {
        CURL* curl = curl_easy_init();
        if (!curl) return;
        char* escaped = curl_easy_escape(curl, text.c_str(), text.length());
        std::string tts_url = "https://tsn.baidu.com/text2audio?tex=" + std::string(escaped)
            + "&lan=zh&cuid=op4pro&ctp=1&tok=" + baidu_token_
            + "&per=4&spd=5&pit=6&vol=5";
        curl_free(escaped);
        curl_easy_cleanup(curl);
        system(("wget -q -O tmp.mp3 \"" + tts_url + "\" && mpg123 -q -a "
                + std::string(AppConfig::kAlsaPlayDevice) + " tmp.mp3 >/dev/null 2>&1").c_str());
    }

    void AudioLoop() {
        snd_pcm_t* h_rec;
        snd_pcm_open(&h_rec, AppConfig::kAlsaRecDevice, SND_PCM_STREAM_CAPTURE, 0);
        snd_pcm_set_params(h_rec, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 32000);
        short buffer[512];
        std::vector<short> record_buf;
        bool is_recording = false;
        int silence_ms = 0;

        while (true) {
            // 跌落警报优先处理
            if (serial_ && serial_->ConsumeAlert()) {
                chat_state_ = ChatState::kSpeaking;
                SetCurrentEmotion("KongJu");
                SpeakText("啊！曼波要掉下去了！");
                chat_state_ = ChatState::kWaiting;
                snd_pcm_prepare(h_rec);
                continue;
            }

            if (chat_state_ == ChatState::kSpeaking) { usleep(100000); continue; }

            int res = snd_pcm_readi(h_rec, buffer, 512);
            if (res == -EPIPE) { snd_pcm_prepare(h_rec); continue; }

            long long sum = 0;
            for (int i = 0; i < 512; i++) sum += std::abs(buffer[i]);
            int rms = sum / 512;

            if (!is_recording && rms > AppConfig::kVoiceThreshold) {
                chat_state_ = ChatState::kListening;
                is_recording = true;
                record_buf.clear();
            }
            if (is_recording) {
                record_buf.insert(record_buf.end(), buffer, buffer + 512);
                silence_ms = (rms < AppConfig::kSilenceThreshold) ? silence_ms + 32 : 0;
                if (silence_ms > AppConfig::kSilenceLimitMs) {
                    is_recording = false;
                    silence_ms = 0;
                    if (record_buf.size() >= 16000) ProcessInteraction(record_buf);
                    else chat_state_ = ChatState::kWaiting;
                    snd_pcm_prepare(h_rec);
                }
            }
        }
    }

    void ProcessInteraction(const std::vector<short>& audio_data) {
        chat_state_ = ChatState::kThinking;
        std::string asr_url = "http://vop.baidu.com/server_api?dev_pid=1537&cuid=op4pro&token=" + baidu_token_;
        std::string asr_res = HttpUtils::Post(asr_url,
            std::string((char*)audio_data.data(), audio_data.size() * 2),
            {"Content-Type: audio/pcm; rate=16000"});

        try {
            auto asr_json = json::parse(asr_res);
            if (!asr_json.contains("result")) { chat_state_ = ChatState::kWaiting; return; }
            std::string user_text = asr_json["result"][0];

            std::string face_emotion;
            { std::lock_guard<std::mutex> lk(mtx_); face_emotion = current_emotion_; }
            chat_history_.push_back({{"role", "user"}, {"content", "[当前对方情绪:" + face_emotion + "] " + user_text}});

            json req_body = {{"model", "deepseek-chat"}, {"messages", chat_history_}};
            std::string llm_res = HttpUtils::Post(
                "https://api.deepseek.com/chat/completions", req_body.dump(),
                {"Authorization: Bearer " + std::string(AppConfig::kDeepseekApiKey),
                 "Content-Type: application/json"});

            std::string ai_content = json::parse(llm_res)["choices"][0]["message"]["content"];

            // 清理可能存在的 Markdown 标记
            if (ai_content.find("```") != std::string::npos) {
                size_t s = ai_content.find("{");
                size_t e = ai_content.rfind("}");
                if (s != std::string::npos && e != std::string::npos)
                    ai_content = ai_content.substr(s, e - s + 1);
            }

            auto ai_json      = json::parse(ai_content);
            std::string reply = ai_json.value("reply",    "曼波~");
            std::string emo   = ai_json.value("emotion",  "ZhongXing");
            std::string act   = ai_json.value("action",   "stop");
            float       dur   = ai_json.value("duration", 0.0f);

            if (act == "spin") act = "left";

            // 更新情绪
            SetCurrentEmotion(emo);

            // 发送动作，持续心跳直到 duration 到期
            if (serial_ && act != "stop" && dur > 0) {
                float d = std::min(dur, 10.0f);
                std::thread([this, act, d]() {
                    auto end_t = std::chrono::steady_clock::now()
                                 + std::chrono::milliseconds((int)(d * 1000));
                    while (std::chrono::steady_clock::now() < end_t) {
                        serial_->SendCommand(act);
                        usleep(150000);
                    }
                    serial_->SendCommand("stop");
                }).detach();
            }

            // 播放语音
            chat_history_.push_back({{"role", "assistant"}, {"content", ai_content}});
            chat_state_ = ChatState::kSpeaking;
            SpeakText(reply);

        } catch (...) { std::cerr << "[Error] 交互解析失败\n"; }
        chat_state_ = ChatState::kWaiting;
    }
};
} // namespace mambo
