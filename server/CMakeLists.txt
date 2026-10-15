cmake_minimum_required(VERSION 3.16)
project(coronet_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

# 源码清单抽成变量：A/B 对照实验需要"同一份源码 + 不同编译开关"编出两份库，
# 用变量列出可以保证两边**永不漂移**（改一处两边同时生效）。
set(CORONET_CORE_SOURCES
    buffer.cpp
    channel.cpp
    poller.cpp
    event_loop.cpp
    event_loop_thread.cpp
    event_loop_thread_pool.cpp
    timer_queue.cpp
    log_file.cpp
    async_logging.cpp
    logger.cpp
    acceptor.cpp
    tcp_connection.cpp
    tcp_server.cpp
    http_parser.cpp
    http_response.cpp
)

add_library(coronet_core STATIC ${CORONET_CORE_SOURCES})
target_include_directories(coronet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(coronet_core PUBLIC Threads::Threads)

add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE coronet_core)

add_executable(coro_echo_server coro_echo_server.cpp)
target_link_libraries(coro_echo_server PRIVATE coronet_core)

add_executable(http_server http_server.cpp)
target_link_libraries(http_server PRIVATE coronet_core)

# ---------------------------------------------------------------------------
#  A/B 对照用的退化版（默认 OFF）
#
#  这个目标把优化点退化回去（一次 writev -> 每段一次 write），其它**一行都不改**，
#  于是"每响应几次写系统调用"成为唯一变量，可以用 strace 数出确定性的差异。
#  它与优化版共用同一份源码清单，两份库不会漂移。
#
#  打开方式：
#      cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORONET_BUILD_AB_DEGRADE=ON
#      cmake --build build -j
#      cd .. && SRV_DIR="$(pwd)/server" bash bench-ab.sh
# ---------------------------------------------------------------------------
option(CORONET_BUILD_AB_DEGRADE "构建 A/B 对照用的退化版（逐段 write 代替 writev）" OFF)

if(CORONET_BUILD_AB_DEGRADE)
    # 同一份源码编第二份库：宏必须作用在**库**的编译上，
    # 否则 tcp_connection.cpp 里的 #ifdef 不会生效
    add_library(coronet_core_nowritev STATIC ${CORONET_CORE_SOURCES})
    target_include_directories(coronet_core_nowritev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(coronet_core_nowritev PUBLIC Threads::Threads)
    target_compile_definitions(coronet_core_nowritev PRIVATE CORONET_AB_NO_WRITEV=1)

    add_executable(http_server_nowritev http_server.cpp)
    target_compile_definitions(http_server_nowritev PRIVATE CORONET_AB_NO_WRITEV=1)
    target_link_libraries(http_server_nowritev PRIVATE coronet_core_nowritev)
endif()

# ---------------------------------------------------------------------------
#  单元测试
#
#  默认 OFF：不加选项时构建行为与以前完全一致（不引入 GoogleTest 依赖）。
#  打开方式：
#      cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORONET_BUILD_TESTS=ON
#      cmake --build build -j
#      cd build && ctest --output-on-failure
# ---------------------------------------------------------------------------
option(CORONET_BUILD_TESTS "构建 GoogleTest 单元测试（需要系统已安装 GoogleTest）" OFF)

if(CORONET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
