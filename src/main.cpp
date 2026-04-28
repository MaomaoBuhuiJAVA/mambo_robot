#include "config.hpp"
#include "utils.hpp"
#include "web_server.hpp"
#include "vision_engine.hpp"
#include "dialog_system.hpp"
#include "../third_party/json.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <cstdio>
#include <fstream>
#include <csignal>
#include <cmath>
#include <cctype>
#include <sstream>
#include <vector>
#include <deque>
#include <utility>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

static std::atomic<bool> g_shutdown{false};
static void sigHandler(int) { g_shutdown = true; }

namespace {
constexpr const char* kObjectLibraryPath = "./data/object_library.json";
constexpr int         kRenYoloClassId    = 0;  // kClassNames[0] == "Ren"
constexpr float       kObjectLibraryMinProb = 0.30f;

void LoadObjectLibraryFromFile(std::unordered_map<int, int>& counts) {
    std::ifstream ifs(kObjectLibraryPath);
    if (!ifs) return;
    try {
        std::ostringstream ss;
        ss << ifs.rdbuf();
        const std::string raw = ss.str();
        if (raw.empty()) return;
        nlohmann::json j = nlohmann::json::parse(raw);
        if (!j.contains("counts") || !j["counts"].is_object()) return;
        for (auto it = j["counts"].begin(); it != j["counts"].end(); ++it) {
            int id = std::stoi(it.key());
            if (it.value().is_number_integer())
                counts[id] = it.value().get<int>();
        }
    } catch (...) {}
}

void SaveObjectLibraryToFile(const std::unordered_map<int, int>& counts) {
    try { std::filesystem::create_directories("./data"); } catch (...) {}
    try {
        nlohmann::json j;
        j["counts"] = nlohmann::json::object();
        for (const auto& p : counts) j["counts"][std::to_string(p.first)] = p.second;
        std::ofstream ofs(kObjectLibraryPath, std::ios::trunc);
        if (ofs) ofs << j.dump(2);
    } catch (...) {}
}

std::string BuildMonitorTimestamp() {
    auto t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

std::string BuildMonitorEventUrl() {
    std::string base = mambo::AppConfig::kMonitorEventBaseUrl ? mambo::AppConfig::kMonitorEventBaseUrl : "";
    std::string path = mambo::AppConfig::kMonitorEventPath ? mambo::AppConfig::kMonitorEventPath : "";
    if (!base.empty() && !path.empty() && base.back() == '/' && path.front() == '/') {
        return base.substr(0, base.size() - 1) + path;
    }
    if (!base.empty() && !path.empty() && base.back() != '/' && path.front() != '/') {
        return base + "/" + path;
    }
    return base + path;
}

std::string MapVisionEmotionToMonitorEvent(const std::string& emotion) {
    if (emotion == "ShengQi") return "irritable_expression";
    if (emotion == "MiMang") return "low_attention";
    if (emotion == "NanGuo" || emotion == "KongJu" || emotion == "YanWu") return "negative_emotion";
    return "";
}

int MonitorConfidenceForEvent(const std::string& event_type) {
    if (event_type == "negative_emotion") return 84;
    if (event_type == "irritable_expression") return 88;
    if (event_type == "low_attention") return 80;
    if (event_type == "agitation_high") return 90;
    return 75;
}

std::string BuildVisionSummary(const mambo::FaceResult& face, const std::string& event_type) {
    if (event_type == "negative_emotion") return "视觉识别到负向情绪：" + face.emotion;
    if (event_type == "irritable_expression") return "视觉识别到烦躁表情：" + face.emotion;
    if (event_type == "low_attention") return "视觉识别到专注下降：" + face.emotion;
    return "视觉识别到监护事件";
}

class MonitorEventReporter {
public:
    MonitorEventReporter() : endpoint_(BuildMonitorEventUrl()) {
    }

    void ReportVisionIfNeeded(const std::vector<mambo::FaceResult>& faces) {
        if (faces.empty()) return;
        const auto& face = faces.front();
        const std::string event_type = MapVisionEmotionToMonitorEvent(face.emotion);
        if (event_type.empty()) return;

        nlohmann::json payload;
        payload["summary"] = BuildVisionSummary(face, event_type);
        payload["emotion"] = face.emotion;
        payload["faceName"] = face.name;
        payload["matchScore"] = face.score;
        if (event_type == "low_attention") {
            payload["durationSec"] = 6;
        }

        TryReport("vision", event_type, MonitorConfidenceForEvent(event_type), payload,
            mambo::AppConfig::kMonitorVisionMinIntervalMs);
    }

    void ReportSensorAlertIfNeeded(const std::string& alert, const mambo::SerialManager::Esp32Data& esp) {
        if (alert != "agitated") return;

        nlohmann::json payload;
        payload["summary"] = "传感器识别到躁动升高告警";
        payload["alert"] = alert;
        payload["radarDist"] = esp.radar_dist;
        payload["radarEnergy"] = esp.radar_energy;
        payload["durationSec"] = 5;

        TryReport("sensor", "agitation_high", MonitorConfidenceForEvent("agitation_high"), payload,
            mambo::AppConfig::kMonitorSensorMinIntervalMs);
    }

private:
    bool IsEnabled() const {
        return mambo::AppConfig::kMonitorEventIngestEnabled
            && mambo::AppConfig::kMonitorChildId > 0
            && mambo::AppConfig::kMonitorDeviceId > 0
            && mambo::AppConfig::kMonitorEventAccessKey[0] != '\0'
            && !endpoint_.empty();
    }

    bool ShouldSend(const std::string& key, int min_interval_ms) {
        const auto now = std::chrono::steady_clock::now();
        auto it = last_sent_at_.find(key);
        if (it != last_sent_at_.end()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed < min_interval_ms) return false;
        }
        last_sent_at_[key] = now;
        return true;
    }

    void TryReport(const std::string& source, const std::string& event_type, int confidence,
                   const nlohmann::json& payload, int min_interval_ms) {
        if (!IsEnabled()) return;
        const std::string dedup_key = source + "::" + event_type;
        if (!ShouldSend(dedup_key, min_interval_ms)) return;

        nlohmann::json body;
        body["childId"] = mambo::AppConfig::kMonitorChildId;
        body["deviceId"] = mambo::AppConfig::kMonitorDeviceId;
        body["source"] = source;
        body["eventType"] = event_type;
        body["confidence"] = confidence;
        body["timestamp"] = BuildMonitorTimestamp();
        body["payload"] = payload;

        std::vector<std::string> headers = {
            "Content-Type: application/json",
            "Accept: application/json",
            std::string("X-Monitor-Access-Key: ") + mambo::AppConfig::kMonitorEventAccessKey
        };
        const std::string request_body = body.dump();
        const std::string url = endpoint_;

        std::thread([url, request_body, headers, source, event_type]() {
            std::string response = mambo::HttpUtils::Post(
                url,
                request_body,
                headers,
                mambo::AppConfig::kMonitorEventTimeoutSec
            );
            if (response.empty()) {
                std::cerr << "[MonitorEvent] 上报失败: " << source << "/" << event_type << " 响应为空\n";
                return;
            }
            try {
                nlohmann::json result = nlohmann::json::parse(response);
                const int code = result.value("code", 500);
                if (code == 200) {
                    std::string status_code = "-";
                    if (result.contains("data") && result["data"].is_object()) {
                        status_code = result["data"].value("code", "-");
                    }
                    std::cerr << "[MonitorEvent] 上报成功: " << source << "/" << event_type
                              << " -> status=" << status_code << "\n";
                    return;
                }
                std::cerr << "[MonitorEvent] 上报失败: " << source << "/" << event_type
                          << " -> " << result.value("msg", response) << "\n";
            } catch (...) {
                std::cerr << "[MonitorEvent] 返回无法解析: " << response << "\n";
            }
        }).detach();
    }

    std::string endpoint_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_sent_at_;
};
}  // namespace

static int ExtractPercentFromAmixer(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        size_t b = i;
        while (b > 0 && std::isdigit((unsigned char)text[b - 1])) --b;
        if (b < i) {
            try {
                int v = std::stoi(text.substr(b, i - b));
                if (v >= 0 && v <= 100) return v;
            } catch (...) {}
        }
    }
    return -1;
}

static std::string RunCommand(const std::string& cmd) {
    std::string out;
    char buf[256];
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return out;
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    pclose(fp);
    return out;
}

struct CmdResult {
    int exit_code = -1;
    std::string output;
};

static CmdResult RunCommandWithCode(const std::string& cmd) {
    CmdResult r;
    char buf[256];
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return r;
    while (fgets(buf, sizeof(buf), fp)) r.output += buf;
    int status = pclose(fp);
    if (status >= 0 && WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    return r;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string GetAlsaCardIndexFromPlayDevice() {
    const std::string dev = mambo::AppConfig::kAlsaPlayDevice ? mambo::AppConfig::kAlsaPlayDevice : "";
    // 期望格式：plughw:3,0 / hw:3,0
    size_t p = dev.find(':');
    if (p == std::string::npos) return "";
    ++p;
    size_t q = p;
    while (q < dev.size() && std::isdigit((unsigned char)dev[q])) ++q;
    if (q == p) return "";
    return dev.substr(p, q - p);
}

static std::string DetectPulseSinkName() {
    // 先取默认 sink
    {
        const std::string info = RunCommand("pactl info 2>/dev/null");
        auto p = info.find("Default Sink:");
        if (p != std::string::npos) {
            p += std::string("Default Sink:").size();
            while (p < info.size() && std::isspace((unsigned char)info[p])) ++p;
            size_t e = p;
            while (e < info.size() && info[e] != '\n' && info[e] != '\r') ++e;
            std::string name = info.substr(p, e - p);
            if (!name.empty()) return name;
        }
    }
    // 兜底：取第一个 sink 的 name（第二列）
    {
        const std::string sinks = RunCommand("pactl list sinks short 2>/dev/null");
        size_t line_start = 0;
        while (line_start < sinks.size()) {
            size_t line_end = sinks.find('\n', line_start);
            if (line_end == std::string::npos) line_end = sinks.size();
            std::string line = sinks.substr(line_start, line_end - line_start);
            if (!line.empty()) {
                std::istringstream iss(line);
                std::string id, name;
                if (iss >> id >> name) return name;
            }
            line_start = line_end + 1;
        }
    }
    return "";
}

static std::pair<std::string, std::string> DetectPulseSinkIdAndName() {
    const std::string sinks = RunCommand("pactl list sinks short 2>/dev/null");
    size_t line_start = 0;
    while (line_start < sinks.size()) {
        size_t line_end = sinks.find('\n', line_start);
        if (line_end == std::string::npos) line_end = sinks.size();
        std::string line = sinks.substr(line_start, line_end - line_start);
        if (!line.empty()) {
            std::istringstream iss(line);
            std::string id, name;
            if (iss >> id >> name) return {id, name};
        }
        line_start = line_end + 1;
    }
    return {"", ""};
}

static int ExtractPercentFromWpctl(const std::string& text) {
    // 例: "Volume: 0.42 [MUTED]"
    auto p = text.find("Volume:");
    if (p == std::string::npos) return -1;
    p += 7;
    while (p < text.size() && std::isspace((unsigned char)text[p])) ++p;
    size_t e = p;
    bool dot_seen = false;
    while (e < text.size()) {
        char c = text[e];
        if (std::isdigit((unsigned char)c)) { ++e; continue; }
        if (c == '.' && !dot_seen) { dot_seen = true; ++e; continue; }
        break;
    }
    if (e == p) return -1;
    try {
        double v = std::stod(text.substr(p, e - p));
        if (v >= 0.0 && v <= 10.0) { // 允许超过1.0的放大值，后面裁剪
            int pct = (int)std::round(v * 100.0);
            return std::max(0, std::min(100, pct));
        }
    } catch (...) {}
    return -1;
}

static std::vector<std::string> DetectMixerControls(const std::string& card_index = "") {
    const std::string cmd = card_index.empty()
        ? "amixer scontrols 2>/dev/null"
        : ("amixer -c " + card_index + " scontrols 2>/dev/null");
    const std::string out = RunCommand(cmd);
    std::vector<std::string> controls;
    size_t pos = 0;
    while (true) {
        size_t p = out.find('\'', pos);
        if (p == std::string::npos) break;
        size_t q = out.find('\'', p + 1);
        if (q == std::string::npos) break;
        std::string c = out.substr(p + 1, q - p - 1);
        if (!c.empty()) controls.push_back(c);
        pos = q + 1;
    }
    return controls;
}

static std::string EscapeDoubleQuotes(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '"') out += "\\\"";
        else out += ch;
    }
    return out;
}

static int GetSpeakerVolumePercent() {
    // 0) 优先直接控制 TTS 播放设备所在 ALSA 卡（例如 plughw:3,0）
    const std::string play_card = GetAlsaCardIndexFromPlayDevice();
    if (!play_card.empty()) {
        const std::vector<std::string> preferred = {
            "Master", "Speaker", "Headphone", "PCM", "Playback", "Lineout"
        };
        std::vector<std::string> controls = preferred;
        for (const auto& c : DetectMixerControls(play_card)) {
            if (std::find(controls.begin(), controls.end(), c) == controls.end()) controls.push_back(c);
        }
        for (const auto& c : controls) {
            const std::string out = RunCommand("amixer -c " + play_card + " sget \"" + EscapeDoubleQuotes(c) + "\" 2>/dev/null");
            int v = ExtractPercentFromAmixer(out);
            if (v >= 0) return v;
        }
    }

    // 1) PulseAudio / PipeWire
    {
        std::string sink = DetectPulseSinkName();
        if (!sink.empty()) {
            const std::string out = RunCommand("pactl get-sink-volume \"" + EscapeDoubleQuotes(sink) + "\" 2>/dev/null");
            int v = ExtractPercentFromAmixer(out);
            if (v >= 0) return v;
        }
        const std::string out_default = RunCommand("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null");
        int v_default = ExtractPercentFromAmixer(out_default);
        if (v_default >= 0) return v_default;
    }
    {
        const std::string out = RunCommand("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
        int v = ExtractPercentFromWpctl(out);
        if (v >= 0) return v;
    }

    // 2) ALSA 兜底
    const std::vector<std::string> preferred = {
        "Master", "Speaker", "Headphone", "PCM", "Playback", "Lineout"
    };
    std::vector<std::string> controls = preferred;
    for (const auto& c : DetectMixerControls()) {
        if (std::find(controls.begin(), controls.end(), c) == controls.end()) controls.push_back(c);
    }
    for (const auto& c : controls) {
        const std::string out = RunCommand("amixer sget \"" + EscapeDoubleQuotes(c) + "\" 2>/dev/null");
        int v = ExtractPercentFromAmixer(out);
        if (v >= 0) return v;
    }
    return 70; // 回退默认值
}

static bool SetSpeakerVolumePercent(int volume_percent) {
    const int v = std::max(0, std::min(100, volume_percent));
    auto verify = [&](int target) {
        int now = GetSpeakerVolumePercent();
        return std::abs(now - target) <= 2;
    };
    // 0) 优先按 TTS 播放设备所在 ALSA 卡设置
    const std::string play_card = GetAlsaCardIndexFromPlayDevice();
    if (!play_card.empty()) {
        const std::vector<std::string> preferred = {
            "Master", "Speaker", "Headphone", "PCM", "Playback", "Lineout"
        };
        std::vector<std::string> controls = preferred;
        for (const auto& c : DetectMixerControls(play_card)) {
            if (std::find(controls.begin(), controls.end(), c) == controls.end()) controls.push_back(c);
        }
        for (const auto& c : controls) {
            const std::string cmd = "amixer -c " + play_card + " sset \"" + EscapeDoubleQuotes(c) + "\" " + std::to_string(v) + "% >/dev/null 2>&1";
            if (std::system(cmd.c_str()) == 0 && verify(v)) return true;
        }
    }

    const auto sink_pair = DetectPulseSinkIdAndName();
    const std::string sink_id = sink_pair.first;
    const std::string sink_name = sink_pair.second;
    const std::vector<std::string> pulse_targets = {
        sink_name.empty() ? "" : ("\"" + EscapeDoubleQuotes(sink_name) + "\""),
        sink_id,
        "@DEFAULT_SINK@"
    };

    // 1) PulseAudio / PipeWire
    for (const auto& t : pulse_targets) {
        if (t.empty()) continue;
        const std::string cmd_set = "pactl set-sink-volume " + t + " " + std::to_string(v) + "% >/dev/null 2>&1";
        const std::string cmd_unmute = "pactl set-sink-mute " + t + " 0 >/dev/null 2>&1";
        if (std::system(cmd_set.c_str()) == 0) {
            std::system(cmd_unmute.c_str());
            if (verify(v)) return true;
        }
    }
    {
        const std::string cmd = "wpctl set-volume @DEFAULT_AUDIO_SINK@ " + std::to_string(v) + "% >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0 && verify(v)) return true;
    }

    // 2) ALSA 兜底
    const std::vector<std::string> preferred = {
        "Master", "Speaker", "Headphone", "PCM", "Playback", "Lineout"
    };
    std::vector<std::string> controls = preferred;
    for (const auto& c : DetectMixerControls()) {
        if (std::find(controls.begin(), controls.end(), c) == controls.end()) controls.push_back(c);
    }
    for (const auto& c : controls) {
        const std::string cmd = "amixer sset \"" + EscapeDoubleQuotes(c) + "\" " + std::to_string(v) + "% >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0 && verify(v)) return true;
    }
    return false;
}

static std::string BuildSpeakerDebugJson(const std::string& value) {
    const int before = GetSpeakerVolumePercent();
    int target = before;
    bool has_target = false;
    if (!value.empty()) {
        has_target = true;
        try { target = std::stoi(value); } catch (...) {}
        target = std::max(0, std::min(100, target));
    }

    const auto sink_pair = DetectPulseSinkIdAndName();
    const std::string sink_id = sink_pair.first;
    const std::string sink_name = sink_pair.second;
    const std::string play_card = GetAlsaCardIndexFromPlayDevice();

    const CmdResult info = RunCommandWithCode("pactl info 2>&1");
    const CmdResult sinks = RunCommandWithCode("pactl list sinks short 2>&1");
    const CmdResult ctrls = RunCommandWithCode("amixer scontrols 2>&1");

    std::vector<std::string> attempt_cmds;
    std::vector<CmdResult> attempt_results;
    bool applied = false;

    if (has_target) {
        auto run_attempt = [&](const std::string& cmd) {
            if (applied) return;
            attempt_cmds.push_back(cmd);
            CmdResult r = RunCommandWithCode(cmd + " 2>&1");
            attempt_results.push_back(r);
            if (r.exit_code == 0) {
                int after_try = GetSpeakerVolumePercent();
                if (std::abs(after_try - target) <= 2) applied = true;
            }
        };

        if (!sink_name.empty()) {
            run_attempt("pactl set-sink-volume \"" + EscapeDoubleQuotes(sink_name) + "\" " + std::to_string(target) + "%");
            run_attempt("pactl set-sink-mute \"" + EscapeDoubleQuotes(sink_name) + "\" 0");
        }
        if (!sink_id.empty()) {
            run_attempt("pactl set-sink-volume " + sink_id + " " + std::to_string(target) + "%");
            run_attempt("pactl set-sink-mute " + sink_id + " 0");
        }
        run_attempt("pactl set-sink-volume @DEFAULT_SINK@ " + std::to_string(target) + "%");
        run_attempt("pactl set-sink-mute @DEFAULT_SINK@ 0");
        run_attempt("wpctl set-volume @DEFAULT_AUDIO_SINK@ " + std::to_string(target) + "%");
    }

    const int after = GetSpeakerVolumePercent();
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << ((has_target ? applied : true) ? "true" : "false") << ","
        << "\"before\":" << before << ","
        << "\"after\":" << after << ","
        << "\"target\":" << target << ","
        << "\"sink_detected\":{"
            << "\"id\":\"" << JsonEscape(sink_id) << "\","
            << "\"name\":\"" << JsonEscape(sink_name) << "\""
        << "},"
        << "\"alsa_play_card\":\"" << JsonEscape(play_card) << "\","
        << "\"commands\":[";
    for (size_t i = 0; i < attempt_cmds.size(); ++i) {
        if (i) oss << ",";
        oss << "{"
            << "\"cmd\":\"" << JsonEscape(attempt_cmds[i]) << "\","
            << "\"exit_code\":" << attempt_results[i].exit_code << ","
            << "\"output\":\"" << JsonEscape(attempt_results[i].output) << "\""
            << "}";
    }
    oss << "],"
        << "\"probe\":{"
            << "\"pactl_info_exit\":" << info.exit_code << ","
            << "\"pactl_info\":\"" << JsonEscape(info.output) << "\","
            << "\"pactl_sinks_exit\":" << sinks.exit_code << ","
            << "\"pactl_sinks\":\"" << JsonEscape(sinks.output) << "\","
            << "\"amixer_scontrols_exit\":" << ctrls.exit_code << ","
            << "\"amixer_scontrols\":\"" << JsonEscape(ctrls.output) << "\""
        << "}"
        << "}";
    return oss.str();
}

int main() {
    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    mambo::SerialManager serial(mambo::AppConfig::kSerialPort);
    mambo::WebServer web;
    web.Start(&serial);
    MonitorEventReporter monitor_event_reporter;
    std::atomic<bool> auto_obstacle_enabled{false};
    std::atomic<bool> follow_mode_enabled{false};
    // 跟随模式：对自动指令做简单平滑，避免频繁“点刹”
    std::string auto_drive_last_cmd = "stop";
    std::string auto_drive_filtered_cmd = "stop";
    std::string auto_drive_last_raw = "stop";
    int auto_drive_same_cnt = 0;
    // 跟随：水平方向用较快 EMA + 「原始 nx 已回中」立刻停转，减轻平滑滞后导致的甩头过头
    float follow_nx_smooth = 0.f;

    mambo::VisionEngine vision;
    mambo::DialogSystem dialog(&serial, &web);
    web.SetBackendHttpHandler([&dialog](const std::string& mode) { return dialog.HandleBackendHttp(mode); });
    web.SetMuteCommandHandler([&dialog](const std::string& cmd) { dialog.HandleConsoleCommand(cmd); });
    web.SetSpeakerVolumeHandler([&](const std::string& value) -> std::string {
        int current = GetSpeakerVolumePercent();
        bool changed = false;
        bool ok = true;
        int target = current;
        if (!value.empty()) {
            try { target = std::stoi(value); } catch (...) {}
            target = std::max(0, std::min(100, target));
            changed = SetSpeakerVolumePercent(target);
            current = GetSpeakerVolumePercent();
            ok = changed;
        }
        std::ostringstream oss;
        oss << "{"
            << "\"ok\":" << (ok ? "true" : "false") << ","
            << "\"volume\":" << current << ","
            << "\"changed\":" << (changed ? "true" : "false") << ","
            << "\"target\":" << target
            << "}";
        return oss.str();
    });
    web.SetSpeakerDebugHandler([&](const std::string& value) -> std::string {
        return BuildSpeakerDebugJson(value);
    });
    web.SetDiagBaiduDeepseekHandler([&dialog]() { return dialog.RunBaiduDeepseekDiagJson(); });
    web.SetClearMemoryHandler([&dialog]() { return dialog.ClearConversationMemoryJson(); });
    web.SetVoiceParamsHandler([&dialog](const std::string& body) { return dialog.HandleVoiceParamsHttp(body); });
    web.SetPersonaConfigHandler([&dialog](const std::string& body) { return dialog.HandlePersonaConfigHttp(body); });
    web.SetTypedDialogHandler([&dialog](const std::string& body) { return dialog.HandleTypedDialogHttp(body); });
    web.SetTtsParamsHandler([&dialog](const std::string& body) { return dialog.HandleTtsParamsHttp(body); });
    web.SetRecordBlockHandler([&dialog](const std::string& body) { return dialog.HandleRecordBlockHttp(body); });
    std::mutex object_lib_mtx;
    std::unordered_map<int, int> object_library_counts;
    LoadObjectLibraryFromFile(object_library_counts);
    web.SetObjectLibraryResetHandler([&]() {
        std::lock_guard<std::mutex> lk(object_lib_mtx);
        object_library_counts.clear();
        SaveObjectLibraryToFile(object_library_counts);
        return std::string("{\"ok\":true}");
    });
    web.SetMotionModeHandler([&](const std::string& name, const std::string& value) -> std::string {
        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        };
        const std::string n = to_lower(name);
        const std::string v = to_lower(value);
        auto apply_value = [&](std::atomic<bool>& flag) {
            if (v == "toggle") { flag.store(!flag.load()); return; }
            if (v == "1" || v == "true" || v == "on" || v == "enable" || v == "enabled") { flag.store(true); return; }
            if (v == "0" || v == "false" || v == "off" || v == "disable" || v == "disabled") { flag.store(false); return; }
        };
        if (n == "obstacle") apply_value(auto_obstacle_enabled);
        else if (n == "follow") apply_value(follow_mode_enabled);
        std::ostringstream oss;
        oss << "{"
            << "\"ok\":true,"
            << "\"auto_obstacle_enabled\":" << (auto_obstacle_enabled.load() ? "true" : "false") << ","
            << "\"follow_mode_enabled\":" << (follow_mode_enabled.load() ? "true" : "false")
            << "}";
        return oss.str();
    });
    dialog.Start();
    std::cerr << "[Console] 输入 `1`/`2`/`toggle` 切换后端；`backend ?` 查看当前。\n";

    // 警报 TTS 播放已在“消费警报”处移除（取消自动说话）

    // 摄像头初始化
    cv::VideoCapture cap;
    auto open_camera = [&cap]() -> bool {
        if (cap.isOpened()) cap.release();
        if (!cap.open(mambo::AppConfig::kCameraIndex)) return false;
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
        cap.set(cv::CAP_PROP_FRAME_WIDTH,  mambo::AppConfig::kCameraWidth);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, mambo::AppConfig::kCameraHeight);
        cap.set(cv::CAP_PROP_FPS,          mambo::AppConfig::kCameraTargetFps);
        return cap.isOpened();
    };
    if (!open_camera()) {
        std::cerr << "[Error] 无法打开摄像头 index=" << mambo::AppConfig::kCameraIndex << std::endl;
        curl_global_cleanup();
        return 1;
    }

    // 共享帧（摄像头线程写，推理线程读）
    std::mutex              cap_mtx;
    cv::Mat                 shared_frame;
    uint64_t                shared_seq = 0;
    std::condition_variable cap_cv;

    std::mutex data_mtx;
    cv::Rect   fast_box;
    bool       has_face         = false;
    int        latest_frame_w   = mambo::AppConfig::kCameraWidth;
    int        latest_frame_h   = mambo::AppConfig::kCameraHeight;
    std::vector<mambo::ObjectResult> slow_objects;
    std::vector<mambo::FaceResult>   slow_faces;
    std::atomic<bool> running(true);
    std::atomic<int>  det_fps(0);

    // 线程1：只读摄像头 + 推流，不做任何推理（保证流帧率稳定）
    std::thread capture_thread([&]() {
        int empty_count = 0;
        auto last_reopen = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        while (running) {
            cv::Mat frame;
            try {
                cap >> frame;
            } catch (...) {
                frame.release();
            }
            if (frame.empty()) {
                ++empty_count;
                // 连续空帧说明 /dev/video0 卡住，尝试重开摄像头恢复推流
                if (empty_count >= 8 &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_reopen).count() > 800) {
                    last_reopen = std::chrono::steady_clock::now();
                    std::cerr << "[Camera] 捕获空帧，尝试重新打开 /dev/video" << mambo::AppConfig::kCameraIndex << "...\n";
                    if (open_camera()) {
                        std::cerr << "[Camera] 摄像头恢复成功\n";
                        empty_count = 0;
                        // warmup：丢弃几帧，避免恢复瞬间第一帧异常
                        for (int i = 0; i < 3; i++) {
                            cv::Mat tmp;
                            try { cap >> tmp; } catch (...) {}
                        }
                    } else {
                        std::cerr << "[Camera] 摄像头恢复失败，稍后重试\n";
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }
            empty_count = 0;

            web.PushVideoFrame(frame); // 异步编码，不阻塞

            {
                std::lock_guard<std::mutex> lk(cap_mtx);
                frame.copyTo(shared_frame);
                ++shared_seq;
            }
            cap_cv.notify_one();
        }
    });

    // 线程2：推理线程，从共享帧做检测，结果直接推送眼睛UI
    std::thread vision_thread([&]() {
        int frame_count = 0, cnt = 0;
        uint64_t last_seq = 0;
        float smooth_cx = mambo::AppConfig::kCameraWidth  / 2.0f;
        float smooth_cy = mambo::AppConfig::kCameraHeight / 2.0f;
        cv::Rect last_person_box;
        bool has_person_box = false;
        auto t0 = std::chrono::steady_clock::now();
        constexpr int kObjectRate = 5; // 物品识别刷新率：每 5 帧更新一次（可调）
        constexpr int kFaceRate = 15;  // 人脸/情绪刷新率：每 15 帧更新一次（保持体验）
        while (running) {
            cv::Mat frame;
            int fw, fh;
            {
                std::unique_lock<std::mutex> lk(cap_mtx);
                cap_cv.wait_for(lk, std::chrono::milliseconds(100),
                    [&]() { return !running || shared_seq != last_seq; });
                if (!running) break;
                if (shared_frame.empty() || shared_seq == last_seq) continue;
                shared_frame.copyTo(frame);
                fw = frame.cols; fh = frame.rows;
                last_seq = shared_seq;
            }
            frame_count++;

            auto boxes = vision.DetectFaceBoxes(frame);

            // 推理完立即更新坐标并推送，不经过主线程
            {
                std::lock_guard<std::mutex> lk(data_mtx);
                has_face       = !boxes.empty();
                if (has_face) {
                    auto best = std::max_element(boxes.begin(), boxes.end(),
                        [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
                    fast_box = (best != boxes.end()) ? *best : boxes[0];
                } else {
                    fast_box = cv::Rect();
                }
                latest_frame_w = fw;
                latest_frame_h = fh;
            }

            // 直接在推理线程推送眼睛坐标，情绪由 LLM 控制
            {
                bool has_target = false;
                float target_cx = fw * 0.5f;
                float target_cy = fh * 0.5f;
                if (!boxes.empty()) {
                    target_cx = boxes[0].x + boxes[0].width  / 2.0f;
                    target_cy = boxes[0].y + boxes[0].height / 2.0f;
                    has_target = true;
                } else if (has_person_box) {
                    target_cx = last_person_box.x + last_person_box.width  / 2.0f;
                    target_cy = last_person_box.y + last_person_box.height / 2.0f;
                    has_target = true;
                }
                if (has_target) {
                    smooth_cx += (target_cx - smooth_cx) * 0.4f;
                    smooth_cy += (target_cy - smooth_cy) * 0.4f;
                }
                float half_w = std::max(1.0f, fw / 2.0f);
                float half_h = std::max(1.0f, fh / 2.0f);
                float nx = -(smooth_cx - half_w) / half_w;
                float ny =  (smooth_cy - half_h) / half_h;
                web.PushEyePos(nx, ny);
            }

            std::vector<mambo::ObjectResult> objects;
            std::vector<mambo::FaceResult>   faces;

            // 物品：高频（只跑 YOLO）
            if (frame_count % kObjectRate == 0) {
                vision.ProcessObjects(frame, objects);
                // 用 YOLO 的 “Ren” 框作为眼睛跟随兜底（YuNet 可能因 OpenCV DNN 不稳定而不可用）
                float best_prob = 0.0f;
                cv::Rect best_box;
                for (const auto& o : objects) {
                    if (o.label != kRenYoloClassId) continue;
                    if (o.prob < best_prob) continue;
                    best_prob = o.prob;
                    best_box = cv::Rect(
                        (int)std::round(o.rect.x),
                        (int)std::round(o.rect.y),
                        (int)std::round(o.rect.width),
                        (int)std::round(o.rect.height)
                    ) & cv::Rect(0, 0, fw, fh);
                }
                if (best_prob > 0.35f && best_box.area() > 0) {
                    last_person_box = best_box;
                    has_person_box = true;
                } else {
                    has_person_box = false;
                }
            }

            // 人脸/情绪：低频（只跑脸，不再重复 YOLO）
            if (frame_count % kFaceRate == 0) {
                vision.ProcessFaces(frame, faces, frame_count);
            }

            {
                std::lock_guard<std::mutex> lk(data_mtx);
                // 物品更新
                if (frame_count % kObjectRate == 0) {
                    slow_objects = objects;
                }
                // 人脸/情绪更新
                if (frame_count % kFaceRate == 0) {
                    slow_faces = faces;
                    if (!faces.empty()) {
                        if (dialog.GetTypedEmotionLock().empty())
                            dialog.SetCurrentEmotion(faces[0].emotion);
                    }
                }
            }

            cnt++;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1000) { det_fps = cnt; cnt = 0; t0 = std::chrono::steady_clock::now(); }
        }
    });

    // 主线程：串口轮询 + 控制台打印 + status推送
    auto last_print = std::chrono::steady_clock::now();
    auto last_object_lib_disk_save = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto last_auto_drive = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto last_scissors_alarm = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    // 运动能量：仅静止且视觉前方有人时从 LD2402 刷新，否则保持上次展示值（避免车体自运动时干扰）
    int radar_energy_display = 0;

    struct AlertEvent {
        std::string ts;
        std::string reason;
        std::string detail;
        std::string evidence_dir;
    };
    std::deque<AlertEvent> recent_alert_events;
    auto push_alert_event = [&](const AlertEvent& e) {
        recent_alert_events.push_back(e);
        while (recent_alert_events.size() > 40) recent_alert_events.pop_front();
    };
    auto now_ts_compact = []() -> std::string {
        auto t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
        return buf;
    };
    auto now_ts_readable = []() -> std::string {
        auto t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        return buf;
    };

    struct EvidenceCapture {
        bool active = false;
        std::string reason;
        std::string dir;
        std::chrono::steady_clock::time_point end_at;
        std::chrono::steady_clock::time_point last_shot;
        int saved_count = 0;
    } evidence;

    while (!g_shutdown) {
        cv::Rect box; bool hf; int fw, fh;
        std::vector<mambo::ObjectResult> objects;
        std::vector<mambo::FaceResult>   faces;
        {
            std::lock_guard<std::mutex> lk(data_mtx);
            box     = fast_box;
            hf      = has_face;
            objects = slow_objects;
            faces   = slow_faces;
            fw      = latest_frame_w;
            fh      = latest_frame_h;
        }

        bool scissors_detected = false;
        float scissors_prob = 0.0f;
        for (const auto& o : objects) {
            if (o.label < 0 || o.label >= (int)mambo::kClassNames.size()) continue;
            const std::string& name = mambo::kClassNames[o.label];
            if (name == "JianDao" || name == "scissors" || name == "Scissors") {
                scissors_detected = true;
                scissors_prob = std::max(scissors_prob, o.prob);
            }
        }

        // 识别到剪刀：告警 + 启动约10秒证据抓拍
        const auto now_wall = std::chrono::steady_clock::now();
        if (scissors_detected &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now_wall - last_scissors_alarm).count() > 15000) {
            last_scissors_alarm = now_wall;
            const std::string ts_compact = now_ts_compact();
            const std::string reason = "检测到剪刀（危险物品）";
            const std::string detail = "置信度 " + std::to_string((int)std::round(scissors_prob * 100)) + "%";
            const std::string dir = "./data/alerts/" + ts_compact + "_scissors";
            try { std::filesystem::create_directories(dir); } catch (...) {}
            evidence.active = true;
            evidence.reason = reason;
            evidence.dir = dir;
            evidence.end_at = now_wall + std::chrono::seconds(10);
            evidence.last_shot = now_wall;
            evidence.saved_count = 0;

            // 立即保存“第一帧”证据图（保证后续10秒从识别瞬间开始）
            cv::Mat first;
            {
                std::lock_guard<std::mutex> lk(cap_mtx);
                if (!shared_frame.empty()) shared_frame.copyTo(first);
            }
            if (!first.empty()) {
                try {
                    std::ostringstream fn;
                    fn << evidence.dir << "/img_" << std::setw(3) << std::setfill('0') << evidence.saved_count << ".jpg";
                    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
                    cv::imwrite(fn.str(), first, params);
                    evidence.saved_count = 1;
                } catch (...) {}
            }
            push_alert_event({now_ts_readable(), reason, detail, dir});
            // 这里原本会触发 TTS 自动播报；已按“取消自动说话”需求移除。
        }

        // 告警证据抓拍：10秒内每500ms保存一帧图片
        if (evidence.active) {
            if (now_wall >= evidence.end_at) {
                std::ostringstream meta;
                meta << "{"
                     << "\"timestamp\":\"" << now_ts_readable() << "\","
                     << "\"reason\":\"" << evidence.reason << "\","
                     << "\"saved_images\":" << evidence.saved_count
                     << "}";
                try {
                    std::ofstream mf(evidence.dir + "/meta.json", std::ios::out | std::ios::trunc);
                    mf << meta.str();
                } catch (...) {}
                evidence.active = false;
            } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now_wall - evidence.last_shot).count() >= 500) {
                evidence.last_shot = now_wall;
                cv::Mat snap;
                {
                    std::lock_guard<std::mutex> lk(cap_mtx);
                    if (!shared_frame.empty()) shared_frame.copyTo(snap);
                }
                if (!snap.empty()) {
                    std::ostringstream fn;
                    fn << evidence.dir << "/img_" << std::setw(3) << std::setfill('0') << evidence.saved_count << ".jpg";
                    try {
                        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
                        cv::imwrite(fn.str(), snap, params);
                        evidence.saved_count++;
                    } catch (...) {}
                }
            }
        }

        mambo::ChatState state = dialog.GetState();

        // 非阻塞控制台：切换 ASR+LLM+TTS 后端
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            int ret = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
            if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
                std::string line;
                if (std::getline(std::cin, line)) dialog.HandleConsoleCommand(line);
            }
        }

        // 轮询串口接收 ESP32 上报
        serial.Poll();
        auto esp = serial.GetEsp32Data();
        {
            const bool robot_still =
                esp.valid && (esp.act == "stop" || esp.act == "blocked" || esp.act.empty());
            const bool person_ahead = !faces.empty();
            if (robot_still && person_ahead)
                radar_energy_display = esp.radar_energy;
        }
        monitor_event_reporter.ReportVisionIfNeeded(faces);
        // 把当前是否在运动告知对话系统（用于“运动时不录音”开关）
        bool moving = esp.valid && !(esp.act == "stop" || esp.act == "blocked" || esp.act.empty());
        dialog.SetIsMoving(moving);

        // 自动避障 + 跟随控制（优先避障）
        const auto now_ctrl = std::chrono::steady_clock::now();
        const long ctrl_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_ctrl - last_auto_drive).count();
        if (ctrl_ms >= 180) {
            const bool auto_modes_on = auto_obstacle_enabled.load() || follow_mode_enabled.load();
            std::string auto_cmd = "stop";
            bool decided = false;
            if (auto_modes_on && auto_obstacle_enabled.load() && esp.valid && esp.radar_dist > 0 && esp.radar_dist < 35) {
                auto_cmd = "stop";
                decided = true;
            }
            const bool in_follow_geom =
                follow_mode_enabled.load() && hf && fw > 0 && fh > 0 && box.width > 0 && box.height > 0;
            float nx_raw = 0.f;
            if (in_follow_geom) {
                const float cx = box.x + box.width * 0.5f;
                nx_raw = (cx - fw * 0.5f) / std::max(1.0f, fw * 0.5f);
                // 跟得上人脸移动，避免 smooth 落后一截还在发转向 → 物理过冲
                follow_nx_smooth = 0.58f * follow_nx_smooth + 0.42f * nx_raw;
            } else {
                follow_nx_smooth = 0.f;
            }
            if (auto_modes_on && !decided && in_follow_geom) {
                const float area_ratio =
                    (float)(box.width * box.height) / std::max(1.0f, (float)(fw * fh));
                const float kAlignHold = 0.15f;
                const float kTurnNeed  = 0.20f;
                // 水平已对准：中间一段距离都先停车，避免框略抖就触发「前进」停不下来
                const float kAreaFwd = 0.042f;
                const float kAreaBwd = 0.22f;
                if (std::fabs(nx_raw) < kAlignHold) {
                    if (area_ratio < kAreaFwd) {
                        auto_cmd = "forward";
                    } else if (area_ratio > kAreaBwd) {
                        auto_cmd = "backward";
                    } else {
                        auto_cmd = "stop";
                    }
                } else if (follow_nx_smooth > kTurnNeed) {
                    auto_cmd = "right";
                } else if (follow_nx_smooth < -kTurnNeed) {
                    auto_cmd = "left";
                } else {
                    if (area_ratio < kAreaFwd) {
                        auto_cmd = "forward";
                    } else if (area_ratio > kAreaBwd) {
                        auto_cmd = "backward";
                    } else {
                        auto_cmd = "stop";
                    }
                }
                decided = true;
            }
            if (auto_modes_on) {
                // 1) 连续确认减轻抖；stop 立即生效，避免人已居中仍多冲半秒
                if (auto_cmd == "stop") {
                    auto_drive_filtered_cmd = "stop";
                    auto_drive_same_cnt     = 0;
                    auto_drive_last_raw     = "stop";
                } else {
                    if (auto_cmd == auto_drive_last_raw) {
                        auto_drive_same_cnt++;
                    } else {
                        auto_drive_same_cnt = 1;
                        auto_drive_last_raw = auto_cmd;
                    }
                    if (auto_drive_same_cnt >= 3) {
                        auto_drive_filtered_cmd = auto_cmd;
                        auto_drive_same_cnt    = 0;
                    }
                }

                // 2) 基于平滑后的指令下发
                if (auto_drive_filtered_cmd != auto_drive_last_cmd) {
                    serial.SendCommand(auto_drive_filtered_cmd);
                    auto_drive_last_cmd = auto_drive_filtered_cmd;
                } else if (decided && auto_drive_filtered_cmd != "stop") {
                    serial.SendCommand(auto_drive_filtered_cmd); // 心跳续命，避免 ESP32 超时自动停
                }
            } else if (auto_drive_last_cmd != "stop") {
                serial.SendCommand("stop");
                auto_drive_last_cmd = "stop";
                auto_drive_filtered_cmd = "stop";
                auto_drive_last_raw = "stop";
                auto_drive_same_cnt = 0;
            }
            last_auto_drive = now_ctrl;
        }

    // 消费警报（仅更新表情/上报，不自动播报 TTS）
        std::string alert = serial.ConsumeAlert();
        // cliff：固件侧红外边沿 + 自动回退，此处不播报
        if (!alert.empty() && alert != "cliff") {
            if (alert == "dizzy") {
                web.PushEyeData(0, 0, "Dizzy"); // 晕眩旋涡表情
            } else if (alert == "agitated") {
                monitor_event_reporter.ReportSensorAlertIfNeeded(alert, esp);
                web.PushEyeData(0, 0, "Worried"); // 担心表情
            }
        }

        // 每秒构建 status JSON + 控制台打印
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print).count() >= 1000) {
            last_print = now;
            std::string ss = (state == mambo::ChatState::kWaiting  ? "waiting"   :
                              state == mambo::ChatState::kListening ? "listening" :
                              state == mambo::ChatState::kThinking  ? "thinking"  : "speaking");

            // 物品库：每秒对「当前帧中」每个非人类别（置信度足够）各计 1 次，避免逐帧爆炸增长
            std::set<int> lib_seen_this_tick;
            for (const auto& o : objects) {
                if (o.label == kRenYoloClassId) continue;
                if (o.label < 0 || o.label >= (int)mambo::kClassNames.size()) continue;
                if (o.prob < kObjectLibraryMinProb) continue;
                lib_seen_this_tick.insert(o.label);
            }
            bool object_lib_changed = false;
            {
                std::lock_guard<std::mutex> lk(object_lib_mtx);
                for (int lb : lib_seen_this_tick) {
                    object_library_counts[lb]++;
                    object_lib_changed = true;
                }
            }
            if (object_lib_changed &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_object_lib_disk_save).count() >= 5000) {
                last_object_lib_disk_save = now;
                std::lock_guard<std::mutex> lk(object_lib_mtx);
                SaveObjectLibraryToFile(object_library_counts);
            }

            // 构建 status JSON
            std::string json = "{";
            json += "\"fps\":" + std::to_string(det_fps.load());
            json += ",\"state\":\"" + ss + "\"";
            json += ",\"backend_selected\":\"" + std::string(dialog.GetBackendSelectedName()) + "\"";
            json += ",\"backend_effective\":\"" + std::string(dialog.GetBackendEffectiveName()) + "\"";
            json += ",\"backend_selected_asr_host\":\"" + std::string(dialog.GetSelectedAsrHost()) + "\"";
            json += ",\"backend_selected_llm_url\":\"" + std::string(dialog.GetSelectedLlmUrl()) + "\"";
            json += ",\"backend_selected_tts_url\":\"" + std::string(dialog.GetSelectedTtsUrl()) + "\"";
            json += ",\"backend_effective_asr_host\":\"" + std::string(dialog.GetEffectiveAsrHost()) + "\"";
            json += ",\"backend_effective_llm_url\":\"" + std::string(dialog.GetEffectiveLlmUrl()) + "\"";
            json += ",\"backend_effective_tts_url\":\"" + std::string(dialog.GetEffectiveTtsUrl()) + "\"";
            json += ",\"muted\":" + std::string(dialog.IsMuted() ? "true" : "false");
            json += ",\"auto_obstacle_enabled\":" + std::string(auto_obstacle_enabled.load() ? "true" : "false");
            json += ",\"follow_mode_enabled\":" + std::string(follow_mode_enabled.load() ? "true" : "false");
            json += ",\"dialog_turn_count\":" + std::to_string(dialog.GetDialogTurnCount());
            json += ",\"mic_rms\":" + std::to_string(dialog.GetMicRms());
            json += ",\"voice_threshold\":" + std::to_string(dialog.GetVoiceThreshold());
            json += ",\"silence_threshold\":" + std::to_string(dialog.GetSilenceThreshold());
            json += ",\"silence_limit_ms\":" + std::to_string(dialog.GetSilenceLimitMs());
            json += ",\"persona_preset\":\"" + std::string(dialog.GetPersonaPresetId()) + "\"";
            json += ",\"llm_max_tokens\":" + std::to_string(dialog.GetLlmMaxTokens());
            if (!faces.empty()) {
                std::string face_emo = faces[0].emotion;
                std::string emo_lock = dialog.GetTypedEmotionLock();
                if (!emo_lock.empty())
                    face_emo = emo_lock;
                char buf[128];
                snprintf(buf, sizeof(buf), ",\"face\":{\"name\":\"%s\",\"emotion\":\"%s\",\"score\":%.2f}",
                         faces[0].name.c_str(), face_emo.c_str(), faces[0].score);
                json += buf;
            } else {
                json += ",\"face\":null";
            }
            json += ",\"objects\":[";
            for (size_t i = 0; i < objects.size(); i++) {
                if (i) json += ",";
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"label\":\"%s\",\"prob\":%.2f}",
                         mambo::kClassNames[objects[i].label].c_str(), objects[i].prob);
                json += buf;
            }
            json += "]";
            json += ",\"object_library\":[";
            {
                std::vector<std::pair<int, int>> lib_items;
                std::lock_guard<std::mutex> lk(object_lib_mtx);
                lib_items.reserve(object_library_counts.size());
                for (const auto& p : object_library_counts) lib_items.push_back({p.second, p.first});
                std::sort(lib_items.begin(), lib_items.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                    if (a.first != b.first) return a.first > b.first;
                    return a.second < b.second;
                });
                bool ol_first = true;
                for (const auto& pr : lib_items) {
                    if (!ol_first) json += ",";
                    ol_first = false;
                    int cnt = pr.first;
                    int lid = pr.second;
                    json += "{\"id\":" + std::to_string(lid) + ",\"label\":\"" + mambo::kClassNames[lid] + "\",\"count\":" + std::to_string(cnt) + "}";
                }
            }
            json += "]";
            json += ",\"alert_events\":[";
            for (size_t i = 0; i < recent_alert_events.size(); ++i) {
                const auto& e = recent_alert_events[recent_alert_events.size() - 1 - i]; // 新的在前
                if (i) json += ",";
                std::string evidence_web_dir;
                auto p = e.evidence_dir.find("alerts/");
                if (p != std::string::npos) {
                    evidence_web_dir = "/alerts/" + e.evidence_dir.substr(p + 7);
                }
                json += "{\"ts\":\"" + e.ts + "\",\"reason\":\"" + e.reason + "\",\"detail\":\"" + e.detail + "\",\"evidence_dir\":\"" + e.evidence_dir + "\",\"evidence_web_dir\":\"" + evidence_web_dir + "\"}";
            }
            json += "]";
            json += ",\"dialog_events\":" + dialog.GetRecentDialogEventsJson();
            std::string espStatus = serial.GetEsp32Status(radar_energy_display);
            if (!espStatus.empty()) json += ",\"esp32\":{" + espStatus + "}";
            json += "}";
            web.PushStatus(json);

            // 控制台打印 ── 香橙派
            std::cout << "\n┌─ 视觉 FPS:" << det_fps << "  状态:" << ss
                      << "  后端:" << dialog.GetBackendSelectedName()
                      << "/" << dialog.GetBackendEffectiveName()
                      << "  静音:" << (dialog.IsMuted() ? "ON" : "OFF")
                      << "  🎤RMS:" << dialog.GetMicRms()
                      << "(阈值:" << dialog.GetVoiceThreshold() << ")";
            if (!faces.empty()) {
                std::string fe = faces[0].emotion;
                std::string lk = dialog.GetTypedEmotionLock();
                if (!lk.empty())
                    fe = lk;
                std::cout << "  人脸:[" << faces[0].name << "|" << fe
                          << "|" << std::fixed << std::setprecision(2) << faces[0].score << "]";
            } else {
                std::cout << "  人脸:无";
            }
            if (!objects.empty()) {
                std::cout << "  物品:";
                for (const auto& o : objects)
                    std::cout << mambo::kClassNames[o.label] << "(" << (int)(o.prob*100) << "%) ";
            }

            // 控制台打印 ── ESP32
            if (esp.valid) {
                std::cout << std::fixed << std::setprecision(2);
                std::cout << "\n└─ ESP32  "
                          << "加速度 X:" << esp.ax << " Y:" << esp.ay << " Z:" << esp.az << " g  "
                          << "陀螺 X:" << std::setprecision(1) << esp.gx
                          << " Y:" << esp.gy << " Z:" << esp.gz << " °/s  "
                          << "悬崖:" << (esp.cliff ? "⚠ 检测到" : "安全") << "  "
                          << "雷达:" << (esp.radar ? "触发" : "无") << "  "
                          << "动作:" << esp.act
                          << "  雷达距离:" << esp.radar_dist << "cm"
                          << "  运动能量:" << radar_energy_display;
            } else {
                std::cout << "\n└─ ESP32  等待连接...";
            }
            std::cout << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    std::cout << "\n[主程序] 正在退出...\n";
    running = false;
    cap_cv.notify_all();
    capture_thread.join();
    vision_thread.join();
    web.Stop();
    curl_global_cleanup();
    return 0;
}
