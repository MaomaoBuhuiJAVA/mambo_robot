// 串口通信测试工具
// 编译: g++ -o serial_test serial_test.cpp
// 用法: ./serial_test [port] [command]
//   例: ./serial_test /dev/ttyS7 forward
//   不带参数则进入交互模式

#include <iostream>
#include <string>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>

int open_serial(const char* port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        std::cerr << "[Error] 无法打开串口 " << port << " : " << strerror(errno) << std::endl;
        return -1;
    }
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
    options.c_cc[VTIME] = 1; // 100ms 超时
    tcsetattr(fd, TCSANOW, &options);
    tcflush(fd, TCIOFLUSH);
    std::cout << "[OK] 串口 " << port << " 已打开 (115200 8N1)" << std::endl;
    return fd;
}

void send_cmd(int fd, const std::string& cmd) {
    std::string payload = cmd + "\n";
    ssize_t n = write(fd, payload.c_str(), payload.size());
    if (n < 0)
        std::cerr << "[Error] 发送失败: " << strerror(errno) << std::endl;
    else
        std::cout << "[TX] " << cmd << std::endl;
}

int main(int argc, char* argv[]) {
    const char* port = (argc >= 2) ? argv[1] : "/dev/ttyS7";

    int fd = open_serial(port);
    if (fd == -1) return 1;

    // 后台线程持续读取 ESP32 回传数据
    std::atomic<bool> running(true);
    std::thread rx_thread([&]() {
        char buf[256];
        while (running) {
            int n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                std::cout << "[RX] " << buf << std::flush;
            }
        }
    });

    if (argc >= 3) {
        // 单次发送模式
        send_cmd(fd, argv[2]);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        // 交互模式
        std::cout << "\n交互模式 - 输入命令发送给 ESP32，输入 q 退出\n";
        std::cout << "常用命令: forward / backward / left / right / stop\n";
        std::cout << "也可以发任意字符串测试\n\n";

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "q" || line == "quit") break;
            if (!line.empty()) send_cmd(fd, line);
        }
    }

    running = false;
    rx_thread.join();
    close(fd);
    return 0;
}
