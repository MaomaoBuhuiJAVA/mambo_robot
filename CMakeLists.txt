cmake_minimum_required(VERSION 3.10)
project(MamboRobot)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenCV REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Try to locate AWNN headers from common SDK install paths.
# You can also export AWNN_ROOT=/path/to/sdk before running build.sh.
set(AWNN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party")
if(NOT EXISTS "${AWNN_INCLUDE_DIR}/awnn_lib.h")
    find_path(AWNN_INCLUDE_DIR NAMES awnn_lib.h
        HINTS $ENV{AWNN_ROOT} $ENV{AWNN_ROOT}/include
        PATHS /usr/include /usr/local/include /usr/include/awnn /usr/local/include/awnn /opt/awnn/include /opt/include
    )
endif()
if(NOT AWNN_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot find awnn_lib.h. Put it in third_party/, or set AWNN_ROOT.")
endif()

set(AWNN_SOURCES "")
set(AWNN_SOURCE_INCLUDE_DIR "")
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/awnn_lib.c")
    list(APPEND AWNN_SOURCES "${CMAKE_SOURCE_DIR}/third_party/awnn_lib.c")
    set(AWNN_SOURCE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/awnn_quantize.c")
    list(APPEND AWNN_SOURCES "${CMAKE_SOURCE_DIR}/third_party/awnn_quantize.c")
    if(NOT AWNN_SOURCE_INCLUDE_DIR)
        set(AWNN_SOURCE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party")
    endif()
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/ai-sdk/ai-sdk/examples/libawnn_viplite/awnn_lib.c")
    list(APPEND AWNN_SOURCES "${CMAKE_SOURCE_DIR}/ai-sdk/ai-sdk/examples/libawnn_viplite/awnn_lib.c")
    set(AWNN_SOURCE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/ai-sdk/ai-sdk/examples/libawnn_viplite")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/ai-sdk/ai-sdk/examples/libawnn_viplite/awnn_quantize.c")
    list(APPEND AWNN_SOURCES "${CMAKE_SOURCE_DIR}/ai-sdk/ai-sdk/examples/libawnn_viplite/awnn_quantize.c")
    if(NOT AWNN_SOURCE_INCLUDE_DIR)
        set(AWNN_SOURCE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/ai-sdk/ai-sdk/examples/libawnn_viplite")
    endif()
endif()

set(AWNN_LINK_LIBS "")
if(AWNN_SOURCES)
    message(STATUS "Using AWNN source implementation: ${AWNN_SOURCES}")
else()
    find_library(AWNN_LIBRARY
        NAMES awnn awnn_lib
        HINTS
            $ENV{AWNN_ROOT}/lib
            $ENV{AWNN_ROOT}/lib64
        PATHS
            /usr/lib
            /usr/lib/aarch64-linux-gnu
            /usr/local/lib
            /usr/local/lib64
            /opt/awnn/lib
            /opt/lib
    )
    if(NOT AWNN_LIBRARY)
        message(FATAL_ERROR "Cannot find AWNN runtime library (libawnn.so/libawnn_lib.so), and no awnn_lib.c source was found.")
    endif()
    list(APPEND AWNN_LINK_LIBS ${AWNN_LIBRARY})
endif()

include_directories(${OpenCV_INCLUDE_DIRS} ${AWNN_INCLUDE_DIR} ${AWNN_SOURCE_INCLUDE_DIR} ./third_party ./src)

add_executable(mambo_robot
    src/main.cpp
    ${AWNN_SOURCES}
)

target_link_libraries(mambo_robot
    ${OpenCV_LIBS}
    ${CURL_LIBRARIES}
    Threads::Threads
    ${AWNN_LINK_LIBS}
    asound
    NBGlinker
    VIPhal
    websockets
)
