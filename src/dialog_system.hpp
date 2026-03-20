#pragma once
#include "config.hpp"
#include "utils.hpp"
#include "../third_party/json.hpp"
#include <alsa/asoundlib.h>
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
            "你是一个叫星宝的桌面机器人，专门陪伴孤独症小朋友。说话温柔、简短、有耐心，口头禅'星宝~'。你可以控制底盘移动。"
            "你必须且只能返回合法的JSON格式，绝对不要包含Markdown标记(如```json)。"
            "格式：{\"reply\": \"你的回复话语\", \"action\": \"forward/backward/left/right/stop\", \"duration\": 秒数}"
            "duration表示动作持续秒数(0=不动，转向默认3秒，前进后退默认2秒，最大10秒)。"
            "示例：用户说'向左转'→{\"reply\":\"好的星宝~\",\"action\":\"left\",\"duration\":3}"
        }});
    }
    void Start() { worker_thread_ = std::thread(&DialogSystem::AudioLoop, this); }
    ~DialogSystem() { if (worker_thread_.joinable()) worker_thread_.join(); }
    ChatState GetState() const { return chat_state_.load(); }
    void SetCurrentEmotion(const std::string& e) { std::lock_guard<std::mutex> lk(mtx_); current_emotion_ = e; }
    std::string GetBaiduToken() const { return baidu_token_; }

private:
    SerialManager* serial_;
    std::thread worker_thread_;
    std::atomic<ChatState> chat_state_;
    std::string baidu_token_;
    std::vector<json> chat_history_;
    std::mutex mtx_;
    std::string current_emotion_ = "ZhongXing";

    void RefreshBaiduToken() {
        std::string url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=" + std::string(AppConfig::kBaiduApiKey) + "&client_secret=" + AppConfig::kBaiduSecretKey;
        try { baidu_token_ = json::parse(HttpUtils::Post(url, "", {}))["access_token"]; } catch (...) {}
    }

    void AudioLoop() {
        snd_pcm_t* h_rec;
        snd_pcm_open(&h_rec, AppConfig::kAlsaRecDevice, SND_PCM_STREAM_CAPTURE, 0);
        snd_pcm_set_params(h_rec, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 32000);
        short buffer[512]; std::vector<short> record_buf;
        bool is_recording = false; int silence_ms = 0;

        while (true) {
            if (chat_state_ == ChatState::kSpeaking) { usleep(100000); continue; } // 说话时不录音
            int res = snd_pcm_readi(h_rec, buffer, 512);
            if (res == -EPIPE) { snd_pcm_prepare(h_rec); continue; }
            
            long long sum = 0; for (int i = 0; i < 512; i++) sum += std::abs(buffer[i]);
            int rms = sum / 512;

            if (!is_recording && rms > AppConfig::kVoiceThreshold) {
                chat_state_ = ChatState::kListening; is_recording = true; record_buf.clear();
            }
            if (is_recording) {
                record_buf.insert(record_buf.end(), buffer, buffer + 512);
                silence_ms = (rms < AppConfig::kSilenceThreshold) ? silence_ms + 32 : 0;
                if (silence_ms > AppConfig::kSilenceLimitMs) {
                    is_recording = false; silence_ms = 0;
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
        std::string asr_res = HttpUtils::Post(asr_url, std::string((char*)audio_data.data(), audio_data.size() * 2), {"Content-Type: audio/pcm; rate=16000"});
        
        try {
            auto asr_json = json::parse(asr_res);
            if (!asr_json.contains("result")) { chat_state_ = ChatState::kWaiting; return; }
            std::string user_text = asr_json["result"][0];
            
            std::string emotion; { std::lock_guard<std::mutex> lk(mtx_); emotion = current_emotion_; }
            chat_history_.push_back({{"role", "user"}, {"content", "[当前对方情绪:" + emotion + "] " + user_text}});
            
            json req_body = {{"model", "deepseek-chat"}, {"messages", chat_history_}};
            std::string llm_res = HttpUtils::Post("https://api.deepseek.com/chat/completions", req_body.dump(), {"Authorization: Bearer " + std::string(AppConfig::kDeepseekApiKey), "Content-Type: application/json"});
            
            std::string ai_content = json::parse(llm_res)["choices"][0]["message"]["content"];
            
            // 清理可能存在的 Markdown 标记
            if (ai_content.find("```json") != std::string::npos) {
                size_t start = ai_content.find("{");
                size_t end = ai_content.rfind("}");
                if (start != std::string::npos && end != std::string::npos) ai_content = ai_content.substr(start, end - start + 1);
            }

            auto ai_json = json::parse(ai_content);
            std::string reply_text = ai_json["reply"];
            std::string action = ai_json.value("action", "stop");
            float duration = ai_json.value("duration", 0.0f);
            duration = std::min(duration, 10.0f);

            // 持续发送动作心跳直到 duration 到期（ESP32 超时 300ms，每 150ms 发一次）
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

            // 2. 播放语音
            chat_history_.push_back({{"role", "assistant"}, {"content", ai_content}});
            chat_state_ = ChatState::kSpeaking;
            
            CURL* curl = curl_easy_init();
            char* escaped_text = curl_easy_escape(curl, reply_text.c_str(), reply_text.length());
            std::string tts_url = "https://tsn.baidu.com/text2audio?tex=" + std::string(escaped_text) + "&lan=zh&cuid=op4pro&ctp=1&tok=" + baidu_token_ + "&per=4&spd=5&pit=6&vol=3";
            curl_free(escaped_text); curl_easy_cleanup(curl);
            
            // 阻塞播放，播放完恢复 Waiting
            system(("wget -q -O tmp.mp3 \"" + tts_url + "\" && mpg123 -q -a " + std::string(AppConfig::kAlsaPlayDevice) + " tmp.mp3 >/dev/null 2>&1").c_str());
        } catch (...) { std::cerr << "[Error] 交互解析失败\n"; }
        chat_state_ = ChatState::kWaiting;
    }
};
}