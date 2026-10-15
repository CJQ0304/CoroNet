# ============================================================================
#  单元测试（仅在 -DCORONET_BUILD_TESTS=ON 时被主 CMakeLists 加入构建）
#
#  依赖：GoogleTest。Ubuntu / WSL 上一条命令装好：
#      sudo apt install -y libgtest-dev cmake
#  （Ubuntu 20.04 起 libgtest-dev 自带预编译库与 CMake 配置文件，
#    所以 find_package(GTest) 能直接找到，不需要自己进源码目录编译。）
# ============================================================================

find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(FATAL_ERROR
        "找不到 GoogleTest。请先安装：\n"
        "    sudo apt install -y libgtest-dev\n"
        "（若已装但仍找不到，可改用源码构建：\n"
        "    sudo apt install -y cmake 后，从 https://github.com/google/googletest 拉源码，\n"
        "    在其目录下 cmake -B build && sudo cmake --install build \n"
        " 再重新配置本项目。）")
endif()

# 测试只依赖 coronet_core 里的纯逻辑单元与头文件，
# 但为了链接方便仍然整体链接静态库（未用到的符号不会进最终二进制）。
add_executable(coronet_tests
    http_parser_test.cpp
    buffer_test.cpp
)

target_link_libraries(coronet_tests
    PRIVATE
        coronet_core
        GTest::gtest
        GTest::gtest_main     # 由 gtest 提供 main()，测试文件里不写 main
)

# gtest_discover_tests 会在构建后自动枚举 TEST 用例并注册进 ctest，
# 于是 `ctest --output-on-failure` 就能跑全部用例，新增用例无需改 CMake。
include(GoogleTest)
gtest_discover_tests(coronet_tests)
