#pragma once
#include "config.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <curl/curl.h>
#include <netdb.h>
#include <arpa/inet.h>

namespace mambo {
class HttpUtils {
public:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
private:
    static bool IsIpv4Literal(const char* host) {
        if (!host || !*host) return false;
        sockaddr_in sa{};
        return inet_pton(AF_INET, host, &sa.sin_addr) == 1;
    }

    static std::string ResolveIpv4ViaSystemDns(const char* host) {
        if (!host || !*host) return "";
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        int rc = getaddrinfo(host, nullptr, &hints, &res);
        if (rc != 0 || !res) return "";
        char ip[INET_ADDRSTRLEN]{};
        auto* in = (sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);
        return ip;
    }

    static std::string ResolveIpv4ViaDoh(const char* host) {
        if (!host || !*host) return "";
        std::string url = std::string("https://1.1.1.1/dns-query?name=") + host + "&type=A";
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        std::string response;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Accept: application/dns-json");
        hdrs = curl_slist_append(hdrs, "Host: cloudflare-dns.com");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) return "";

        size_t pos = 0;
        while (true) {
            pos = response.find("\"type\":1", pos);
            if (pos == std::string::npos) break;
            auto d = response.find("\"data\":\"", pos);
            if (d == std::string::npos) break;
            d += 8;
            auto e = response.find('"', d);
            if (e == std::string::npos) break;
            std::string ip = response.substr(d, e - d);
            if (IsIpv4Literal(ip.c_str())) return ip;
            pos = e + 1;
        }
        return "";
    }

    /** Google DNS JSON API，部分网络屏蔽 1.1.1.1 时仍可能可用 */
    static std::string ResolveIpv4ViaGoogleDns(const char* host) {
        if (!host || !*host) return "";
        std::string url = std::string("https://dns.google/resolve?name=") + host + "&type=A";
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        CURLcode cres = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (cres != CURLE_OK) return "";
        auto ans = response.find("\"Answer\"");
        if (ans == std::string::npos) return "";
        size_t pos = ans;
        while ((pos = response.find("\"data\":\"", pos)) != std::string::npos) {
            pos += 8;
            auto end = response.find('"', pos);
            if (end == std::string::npos) break;
            std::string ip = response.substr(pos, end - pos);
            if (IsIpv4Literal(ip.c_str())) return ip;
            pos = end + 1;
        }
        return "";
    }

    static bool ParseHostPort(const std::string& url, std::string& host, int& port) {
        auto scheme_pos = url.find("://");
        if (scheme_pos == std::string::npos) return false;
        std::string scheme = url.substr(0, scheme_pos);
        port = (scheme == "https") ? 443 : 80;
        size_t start = scheme_pos + 3;
        size_t end = url.find('/', start);
        std::string hostport = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (hostport.empty()) return false;
        if (hostport.front() == '[') return false;
        auto colon = hostport.find(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            try { port = std::stoi(hostport.substr(colon + 1)); } catch (...) { return false; }
        } else {
            host = hostport;
        }
        return !host.empty();
    }

public:
    /**
     * 解析域名为 IPv4：系统 DNS → Cloudflare DoH → Google DNS；
     * 对本地后端域名可配置 AppConfig::kLocalBackendIpOverride 跳过解析。
     */
    static std::string ResolveIpv4Robust(const char* host) {
        if (!host || !*host) return "";
        if (IsIpv4Literal(host)) return std::string(host);
        if (AppConfig::kLocalBackendIpOverride[0] != '\0' &&
            std::strcmp(host, AppConfig::kLocalBackendHostname) == 0 &&
            IsIpv4Literal(AppConfig::kLocalBackendIpOverride)) {
            std::cerr << "[DNS] 使用配置 kLocalBackendIpOverride -> " << AppConfig::kLocalBackendIpOverride << "\n";
            return std::string(AppConfig::kLocalBackendIpOverride);
        }
        std::string ip = ResolveIpv4ViaSystemDns(host);
        if (!ip.empty()) return ip;
        std::cerr << "[DNS] getaddrinfo 失败: " << host << "，尝试 Cloudflare DoH\n";
        ip = ResolveIpv4ViaDoh(host);
        if (!ip.empty()) return ip;
        std::cerr << "[DNS] Cloudflare DoH 失败，尝试 Google DNS\n";
        ip = ResolveIpv4ViaGoogleDns(host);
        if (!ip.empty()) return ip;
        std::cerr << "[DNS] 所有解析方式均失败: " << host << "\n";
        return "";
    }

    static std::string Post(const std::string& url, const std::string& data,
                            const std::vector<std::string>& headers, long timeout_sec = 15) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        std::string response;
        struct curl_slist* chunk = nullptr;
        struct curl_slist* resolve = nullptr;
        std::string host;
        int port = 0;

        for (const auto& h : headers) chunk = curl_slist_append(chunk, h.c_str());
        if (ParseHostPort(url, host, port) && !IsIpv4Literal(host.c_str())) {
            std::string ip = ResolveIpv4Robust(host.c_str());
            if (!ip.empty()) {
                std::string entry = host + ":" + std::to_string(port) + ":" + ip;
                resolve = curl_slist_append(resolve, entry.c_str());
                curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
            }
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 8L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 32L);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
            std::cerr << "[HTTP Error] " << url.substr(0, 50) << " -> " << curl_easy_strerror(res) << "\n";
        curl_slist_free_all(chunk);
        curl_slist_free_all(resolve);
        curl_easy_cleanup(curl);
        return response;
    }

    /** GET 请求（用于百度 TTS 等） */
    static std::string Get(const std::string& url, long timeout_sec = 30) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        std::string response;
        struct curl_slist* resolve = nullptr;
        std::string host;
        int port = 0;

        if (ParseHostPort(url, host, port) && !IsIpv4Literal(host.c_str())) {
            std::string ip = ResolveIpv4Robust(host.c_str());
            if (!ip.empty()) {
                std::string entry = host + ":" + std::to_string(port) + ":" + ip;
                resolve = curl_slist_append(resolve, entry.c_str());
                curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
            }
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
            std::cerr << "[HTTP GET Error] " << url.substr(0, 80) << " -> " << curl_easy_strerror(res) << "\n";
        curl_slist_free_all(resolve);
        curl_easy_cleanup(curl);
        return response;
    }
};

class SerialManager {
private:
    int fd;
    std::mutex rx_mtx_;
    std::string rx_buf_;

public:
    // ESP32 上报的状态（Poll() 解析后更新）
    struct Esp32Data {
        float ax = 0, ay = 0, az = 0;
        float gx = 0, gy = 0, gz = 0;
        bool  cliff = false, radar = false;
        std::string alert;   // "cliff" / "fall" / "dizzy" / "agitated" / ""
        std::string act = "stop";
        int   radar_dist   = 0;  // LD2402 目标距离（cm）
        int   radar_energy = 0;  // LD2402 运动能量总和
        bool valid = false;
    };

private:
    Esp32Data esp_data_;

    // 极简 JSON 字段提取，不引入 json 库
    static float jsonFloat(const std::string& s, const std::string& key) {
        auto pos = s.find("\"" + key + "\":");
        if (pos == std::string::npos) return 0.0f;
        pos += key.size() + 3;
        try { return std::stof(s.substr(pos)); } catch (...) { return 0.0f; }
    }
    static int jsonInt(const std::string& s, const std::string& key) {
        return (int)jsonFloat(s, key);
    }
    static std::string jsonStr(const std::string& s, const std::string& key) {
        auto pos = s.find("\"" + key + "\":\"");
        if (pos == std::string::npos) return "";
        pos += key.size() + 4;
        auto end = s.find('"', pos);
        return end == std::string::npos ? "" : s.substr(pos, end - pos);
    }

public:
    SerialManager(const char* port) {
        fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
        if (fd == -1) { std::cerr << "[Error] 无法打开串口 " << port << std::endl; return; }
        struct termios options;
        tcgetattr(fd, &options);
        cfsetispeed(&options, B115200);
        cfsetospeed(&options, B115200);
        options.c_cflag |= (CLOCAL | CREAD | CS8);
        options.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
        options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        options.c_iflag &= ~(IXON | IXOFF | IXANY);
        options.c_oflag &= ~OPOST;
        options.c_cc[VMIN]  = 0;
        options.c_cc[VTIME] = 1;
        tcsetattr(fd, TCSANOW, &options);
        tcflush(fd, TCIOFLUSH);
    }
    ~SerialManager() { if (fd != -1) close(fd); }

    void SendCommand(const std::string& cmd) {
        if (fd == -1) return;
        std::string payload = cmd + "\n";
        write(fd, payload.c_str(), payload.length());
    }

    void Poll() {
        if (fd == -1) return;
        char buf[256];
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) return;
        // 防止 rx_buf_ 无限增长（正常每行 < 200 字节，4096 足够）
        if (rx_buf_.size() + n > 4096) rx_buf_.clear();
        rx_buf_.append(buf, n);
        size_t pos;
        while ((pos = rx_buf_.find('\n')) != std::string::npos) {
            std::string line = rx_buf_.substr(0, pos);
            rx_buf_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // ESP32 发来的 JSON
            if (!line.empty() && line.front() == '{') {
                std::lock_guard<std::mutex> lk(rx_mtx_);
                try {
                    esp_data_.ax    = jsonFloat(line, "ax");
                    esp_data_.ay    = jsonFloat(line, "ay");
                    esp_data_.az    = jsonFloat(line, "az");
                    esp_data_.gx    = jsonFloat(line, "gx");
                    esp_data_.gy    = jsonFloat(line, "gy");
                    esp_data_.gz    = jsonFloat(line, "gz");
                    esp_data_.cliff = jsonInt(line, "cliff") != 0;
                    esp_data_.radar = jsonInt(line, "radar") != 0;
                    esp_data_.radar_dist   = jsonInt(line, "radar_dist");
                    esp_data_.radar_energy = jsonInt(line, "radar_energy");
                    // 累积 alert，消费后清除
                    if (esp_data_.alert.empty()) {
                        std::string a = jsonStr(line, "alert");
                        if (!a.empty()) esp_data_.alert = a;
                    }
                    esp_data_.act = jsonStr(line, "act");
                    esp_data_.valid = true;
                } catch (...) {}
            }
        }
    }

    // 消费警报（读一次后清除）
    // 返回 "cliff"、"fall" 或 ""
    std::string ConsumeAlert() {
        std::lock_guard<std::mutex> lk(rx_mtx_);
        if (!esp_data_.alert.empty()) {
            std::string a = esp_data_.alert;
            esp_data_.alert.clear();
            return a;
        }
        return "";
    }

    Esp32Data GetEsp32Data() {
        std::lock_guard<std::mutex> lk(rx_mtx_);
        return esp_data_;
    }

    // 兼容旧接口，给 web_server 用
    std::string GetEsp32Status() {
        std::lock_guard<std::mutex> lk(rx_mtx_);
        if (!esp_data_.valid) return "";
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                 "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f,"
                 "\"cliff\":%d,\"radar\":%d,\"act\":\"%s\","
                 "\"radar_dist\":%d,\"radar_energy\":%d",
                 esp_data_.ax, esp_data_.ay, esp_data_.az,
                 esp_data_.gx, esp_data_.gy, esp_data_.gz,
                 esp_data_.cliff ? 1 : 0,
                 esp_data_.radar ? 1 : 0,
                 esp_data_.act.c_str(),
                 esp_data_.radar_dist,
                 esp_data_.radar_energy);
        return buf;
    }
};
}
