#pragma once


#include <windows.h>
#include <vector>
#pragma comment (lib, "lua50.lib")
#pragma comment (lib, "legacy_stdio_definitions.lib")
#pragma comment (lib, "lua50d.lib")

extern std::vector<char*> g_debugLines;

extern "C"
{
#include <lua.h>
#include <lualib.h>
}
#include <optional>
typedef VOID(__cdecl* fnLuaFile)(lua_State* lua);

typedef VOID(__cdecl* fnLuaPushString)(LPCSTR);
VOID WINAPIV HookedLuaPushString(LPCSTR);
extern fnLuaPushString RealLuaPushString;

typedef DWORD(__cdecl* fnLuaRegister)(DWORD*, DWORD*, DWORD*, DWORD*);
typedef DWORD(__cdecl* fnLuaPushCFunction)(DWORD, DWORD);


typedef union {
    void* gc;
    void* p;
    double n;
    int b;
} Value;

typedef struct lua_TObject {
    int tt;
    Value value;
} TObject;


typedef unsigned char lu_byte;
typedef TObject* StkId;  /* index to stack elements */

#define CommonHeader    void *next; lu_byte tt; lu_byte marked

struct lua_State {
    CommonHeader;
    DWORD* top;  /* first free slot in the stack */
    DWORD* base;  /* base of current function */
    void* l_G;
    void* ci;  /* call info for current function */
    StkId stack_last;  /* last free slot in the stack */
    StkId stack;  /* stack base */
    int stacksize;
    void* end_ci;  /* points after end of ci array*/
    void* base_ci;  /* array of CallInfo's */
    unsigned short size_ci;  /* size of array `base_ci' */
    unsigned short nCcalls;  /* number of nested C calls */
    lu_byte hookmask;
    lu_byte allowhook;
    lu_byte hookinit;
    int basehookcount;
    int hookcount;
    void* hook;
    DWORD _gt;  /* table of globals */
    void* openupval;  /* list of open upvalues in this stack */
    void* gclist;
    void* errorJmp;  /* current error recover point */
    void* errfunc;  /* current error handling function (stack index) */
};
class lua_thread
{
public:
    unsigned __int16 handle;
    lua_State* lua_state;
    lua_State* root_state;
    bool first_run;
    char* func_name;
    unsigned int context;
    int flags;
    int num_pushed_args;
    void pushnil();
    void push_number(float arg);
    void push_string(const char* pushed_string);
};

// note the second template arg
template<typename T, typename Enable = void>
struct lua_type_traits;

// ======================================================
// all integer types except bool
// int, uint32_t, uintptr_t, size_t, int64_t, etc.
// ======================================================

template<typename T>
struct lua_type_traits<
    T,
    std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>
>
{
    static T get(lua_State* L, int index)
    {
        return static_cast<T>(lua_tonumber(L, index));
    }

    static void push(lua_State* L, T value)
    {
        lua_pushnumber(L, static_cast<lua_Number>(value));
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isnumber(L, index);
    }
};

// ======================================================
// floating point
// ======================================================

template<typename T>
struct lua_type_traits<
    T,
    std::enable_if_t<std::is_floating_point_v<T>>
>
{
    static T get(lua_State* L, int index)
    {
        return static_cast<T>(lua_tonumber(L, index));
    }

    static void push(lua_State* L, T value)
    {
        lua_pushnumber(L, static_cast<lua_Number>(value));
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isnumber(L, index);
    }
};

// ======================================================
// bool
// ======================================================

template<>
struct lua_type_traits<bool>
{
    static bool get(lua_State* L, int index)
    {
        return lua_toboolean(L, index) != 0;
    }

    static void push(lua_State* L, bool value)
    {
        lua_pushboolean(L, value);
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isboolean(L, index);
    }
};

// ======================================================
// const char*
// ======================================================

template<>
struct lua_type_traits<const char*>
{
    static const char* get(lua_State* L, int index)
    {
        return lua_tostring(L, index);
    }

    static void push(lua_State* L, const char* value)
    {
        lua_pushstring(L, value ? value : "");
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isstring(L, index);
    }
};

// ======================================================
// char*
// ======================================================

template<>
struct lua_type_traits<char*>
{
    static char* get(lua_State* L, int index)
    {
        return const_cast<char*>(lua_tostring(L, index));
    }

    static void push(lua_State* L, const char* value)
    {
        lua_pushstring(L, value ? value : "");
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isstring(L, index);
    }
};

// ======================================================
// std::string
// ======================================================

template<>
struct lua_type_traits<std::string>
{
    static std::string get(lua_State* L, int index)
    {
        const char* str = lua_tostring(L, index);
        return str ? std::string(str) : std::string{};
    }

    static void push(lua_State* L, const std::string& value)
    {
        lua_pushstring(L, value.c_str());
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isstring(L, index);
    }
};

// ======================================================
// void*
// ======================================================

template<>
struct lua_type_traits<void*>
{
    static void* get(lua_State* L, int index)
    {
        return reinterpret_cast<void*>(
            static_cast<uintptr_t>(lua_tonumber(L, index))
            );
    }

    static void push(lua_State* L, void* value)
    {
        lua_pushnumber(
            L,
            static_cast<lua_Number>(reinterpret_cast<uintptr_t>(value))
        );
    }

    static bool is_valid_type(lua_State* L, int index)
    {
        return lua_isnumber(L, index);
    }
};

// ======================================================
// LuaArgs
// ======================================================

class LuaArgs {
    lua_State* L;
    int current_index;
    int total_args;

public:
    LuaArgs(lua_State* L)
        : L(L), current_index(1), total_args(lua_gettop(L)) {
    }

    template<typename T>
    T get() {
        if (current_index > total_args) {
            return T{};
        }

        T result = lua_type_traits<T>::get(L, current_index);
        current_index++;
        return result;
    }

    template<typename T>
    T get_or(T default_value) {
        if (current_index > total_args) {
            return default_value;
        }

        if (!lua_type_traits<T>::is_valid_type(L, current_index)) {
            current_index++;
            return default_value;
        }

        T result = lua_type_traits<T>::get(L, current_index);
        current_index++;
        return result;
    }

    template<typename T>
    T get_or_unsafe(T default_value) {
        if (current_index > total_args) {
            return default_value;
        }

        T result = lua_type_traits<T>::get(L, current_index);
        current_index++;
        return result;
    }

    template<typename T>
    std::optional<T> get_optional() {
        if (current_index > total_args) {
            return std::nullopt;
        }

        if (!lua_type_traits<T>::is_valid_type(L, current_index)) {
            current_index++;
            return std::nullopt;
        }

        T result = lua_type_traits<T>::get(L, current_index);
        current_index++;
        return result;
    }

    bool has_more() const {
        return current_index <= total_args;
    }

    bool has_arg(int offset = 0) const {
        return (current_index + offset) <= total_args;
    }

    template<typename T>
    T peek(int offset = 0) {
        int index = current_index + offset;

        if (index > total_args) {
            return T{};
        }

        return lua_type_traits<T>::get(L, index);
    }

    int remaining() const {
        return total_args - current_index + 1;
    }

    int total() const {
        return total_args;
    }
};

// ======================================================
// LuaReturns
// ======================================================

class LuaReturns {
    lua_State* L;
    int return_count;

public:
    LuaReturns(lua_State* L)
        : L(L), return_count(0) {
    }

    template<typename T>
    LuaReturns& push(T value) {
        lua_type_traits<T>::push(L, value);
        return_count++;
        return *this;
    }

    template<typename T, typename... Args>
    LuaReturns& push_all(T first, Args... rest) {
        push(first);

        if constexpr (sizeof...(rest) > 0) {
            push_all(rest...);
        }

        return *this;
    }

    int count() const {
        return return_count;
    }
};
