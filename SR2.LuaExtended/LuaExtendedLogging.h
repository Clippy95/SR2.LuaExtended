#pragma once

namespace LuaExtended
{
    void LogPrintf(const char* format, ...);
    void FlushDebugLog();
}

#define lextprint(...) ::LuaExtended::LogPrintf(__VA_ARGS__)
