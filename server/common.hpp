#pragma once

// ============================================================================
//  common.hpp —— 通用基础设施
//
//  日志设施（LogLevel / LogStream / LOG_* 宏）在 logger.hpp 中定义，
//  这里统一 include 一次，全项目其余文件只依赖 common.hpp。
//
//  宏名与调用形态同日志实现解耦：LOG_INFO/LOG_ERROR/LOG_DEBUG 不随实现变化，
//  所以替换日志实现时，全项目几十个调用点一处都不用改。
// ============================================================================

#include "logger.hpp"   // LogStream / LogMessage / Logger 以及 LOG_* 宏

class NonCopyable {
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};
