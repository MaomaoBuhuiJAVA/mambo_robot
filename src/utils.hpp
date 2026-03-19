#pragma once
#include <iostream>
#include <string>
#include <vector>
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
public:
    SerialManager(const char* port) {
        fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
        if (fd == -1) { std::cerr << "[Error] 无法打开串口 " << port << std::endl; return; }
        struct termios options;
        tcgetattr(fd, &options);
        cfsetispeed(&options, B115200);
        cfsetospeed(&options, B115200);
        options.c_cflag |= (CLOCAL | CREAD | CS8);
        tcsetattr(fd, TCSANOW, &options);
    }
    ~SerialManager() { if (fd != -1) close(fd); }
    
    void SendCommand(const std::string& cmd) {
        if (fd == -1) return;
        std::string payload = cmd + "\n";
        write(fd, payload.c_str(), payload.length());
    }
};
}