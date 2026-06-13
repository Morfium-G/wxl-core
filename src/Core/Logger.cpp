#include "Core/Logger.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
    FILE*      g_file = nullptr;
    std::mutex g_mutex;
}

namespace wraith::log
{
    void Init()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) return;
        CreateDirectoryA("Logs", nullptr);
        fopen_s(&g_file, "Logs\\Wraith.log", "w");
    }

    void Write(const char* level, const char* fmt, ...)
    {
        char body[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);

        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[1152];
        snprintf(line, sizeof(line), "[%02d:%02d:%02d][%s] %s\n",
                 t.wHour, t.wMinute, t.wSecond, level, body);

        std::lock_guard<std::mutex> lock(g_mutex);
        OutputDebugStringA(line);
        if (g_file) { fputs(line, g_file); fflush(g_file); }
    }
}
