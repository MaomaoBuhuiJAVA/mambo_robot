#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <curl/curl.h>

namespace mambo {
class HttpUtils {
public:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    static std::string Post(const std::string& url, const std::string& data, const std::vector<std::string>& headers) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        std::string response;
        struct curl_slist* chunk = nullptr;
        for (const auto& h : headers) chunk = curl_slist_append(chunk, h.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_perform(curl);
        curl_slist_free_all(chunk);
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
        float v = 0, c = 0;                      // 电压(V), 电流(A)
        float ax = 0, ay = 0, az = 0;            // 加速度(g)
        float gx = 0, gy = 0, gz = 0;            // 角速度(dps)
        bool  cliff = false, radar = false;       // 跌落/雷达
        std::string act = "stop";                 // 当前动作
        bool valid = false;                       // 是否收到过数据
    };

private:
    Esp32Data esp_data_;

    // 极简 JSON 字段提取，不引入 json 库
    static float jsonFloat(const std::string& s, const std::string& key) {
        auto pos = s.find("\"" + key + "\":");
        if (pos == std::string::npos) return 0.0f;
        pos += key.size() + 3;
        return std::stof(s.substr(pos));
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
                    esp_data_.v     = jsonFloat(line, "v");
                    esp_data_.c     = jsonFloat(line, "c");
                    esp_data_.ax    = jsonFloat(line, "ax");
                    esp_data_.ay    = jsonFloat(line, "ay");
                    esp_data_.az    = jsonFloat(line, "az");
                    esp_data_.gx    = jsonFloat(line, "gx");
                    esp_data_.gy    = jsonFloat(line, "gy");
                    esp_data_.gz    = jsonFloat(line, "gz");
                    esp_data_.cliff = jsonInt(line, "cliff") != 0;
                    esp_data_.radar = jsonInt(line, "radar") != 0;
                    esp_data_.act   = jsonStr(line, "act");
                    esp_data_.valid = true;
                } catch (...) {}
            }
        }
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
                 "\"v\":%.2f,\"c\":%.4f,"
                 "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                 "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f,"
                 "\"cliff\":%d,\"radar\":%d,\"act\":\"%s\"",
                 esp_data_.v, esp_data_.c,
                 esp_data_.ax, esp_data_.ay, esp_data_.az,
                 esp_data_.gx, esp_data_.gy, esp_data_.gz,
                 esp_data_.cliff ? 1 : 0,
                 esp_data_.radar ? 1 : 0,
                 esp_data_.act.c_str());
        return buf;
    }
};
}