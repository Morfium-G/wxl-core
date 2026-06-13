#pragma once

// WRAITH logging. printf-style, written to Logs/Wraith.log next to the client and mirrored to the
// debugger. Use the WLOG_* macros.
namespace wraith::log
{
    void Init();
    void Write(const char* level, const char* fmt, ...);
}

#define WLOG_INFO(...)  ::wraith::log::Write("INFO",  __VA_ARGS__)
#define WLOG_WARN(...)  ::wraith::log::Write("WARN",  __VA_ARGS__)
#define WLOG_ERROR(...) ::wraith::log::Write("ERROR", __VA_ARGS__)
