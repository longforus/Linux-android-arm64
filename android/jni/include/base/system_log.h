#pragma once
#include <cerrno>
#include <format>
#include <string>
#include <string_view>

#include "../../driver/driver.h"

namespace SyscallLog
{
    inline std::string ReadDmesg(std::string_view tag = {}, int *status = nullptr)
    {
        std::string out;
        const int result = dr ? dr->ReadMonitorLog(tag, out) : -ENODEV;
        if (status) *status = result;
        if (result < 0 && !status) return std::format("读取驱动监控日志失败 status={}，请使用配套新版内核", result);
        return out;
    }
} // namespace SyscallLog
