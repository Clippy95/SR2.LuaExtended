#pragma once

#include <cstdint>

namespace LuaExtended::Assembly
{
    uintptr_t CompileAssemblyScript(const char* name, const char* script);
}
