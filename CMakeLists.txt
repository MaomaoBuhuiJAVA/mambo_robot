cmake_minimum_required(VERSION 3.10)
project(MamboRobot)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenCV REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include_directories(${OpenCV_INCLUDE_DIRS} ./third_party ./src)

# awnn 源文件一起编译
add_executable(mambo_robot
    src/main.cpp
    third_party/awnn_lib.c
    third_party/awnn_quantize.c
)

target_link_libraries(mambo_robot
    ${OpenCV_LIBS}
    ${CURL_LIBRARIES}
    Threads::Threads
    asound
    NBGlinker
    VIPhal
)
