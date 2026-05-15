// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"



#include <safetyhook.hpp>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include "GLua.h"
#include "AssemblyModule.h"

#include "MemoryMgr.h"
#include <shlwapi.h>
#pragma comment(lib,"Shlwapi.lib")
#include <map>
#include "IniReader.h"

#define LEX_VERSION "0.0.1"

#define lextprint(format, ...) \
    do { \
            printf("[LUA Extended] " format, ##__VA_ARGS__); \
    } while(0)

void __declspec(naked) InGamePrintASMSS(int a1, const char* a2, int a3, int a4, float a5) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, __LOCAL_SIZE

        push edi
        push esi
        push eax

        mov edi, a1
        mov esi, a2
        push a5
        push a4
        push a3

        mov eax, 0xD15D00
        call eax

        pop eax
        pop esi
        pop edi

        mov esp, ebp
        pop ebp
        ret
    }
}

int processtextwidth(int width) {
    bool& r_is_widescreen = *(bool*)0x025272DD;
    if (!r_is_widescreen)
        return 0;
    float* currentAR = (float*)0x022FD8EC;
    if (*currentAR >= 1.77777777778f) {
        int offset = (int)(*currentAR * 720);
        offset -= 1280;
        if (offset != 0) {
            width += offset / 2;
        }
    }
    return width;

}

typedef float(__cdecl* ChangeTextColorT)(int R, int G, int B, int Alpha);
ChangeTextColorT ChangeTextColor = (ChangeTextColorT)0xD14840;

using namespace Memory::VP;
static auto HandleDynAddress = GetModuleHandle(nullptr);
template<typename AT>
__declspec(noinline) AT DynAddress(AT address)
{
    static_assert(sizeof(AT) == sizeof(uintptr_t), "AT must be pointer sized");

    uintptr_t inputAddr = std::bit_cast<uintptr_t>(address);

    // This is SR2 exe range, this function should only be really used for the mass conversion of older functions, and from now on we have to rely on MemoryMgr.h from ModUtils -- Clippy95
    if (inputAddr >= 0x00400000ULL && inputAddr <= 0x03559000ULL) {
        uintptr_t baseAddr = std::bit_cast<uintptr_t>(HandleDynAddress);

#ifdef _WIN64
        uintptr_t result = baseAddr - 0x140000000ULL + inputAddr;
#else
        uintptr_t result = baseAddr - 0x400000UL + inputAddr;
#endif
        return std::bit_cast<AT>(result);
    }

    // Return the original address if it's outside the range
    return address;
}

bool isAddrInExe(uintptr_t inputAddr)
{
    return inputAddr >= 0x00400000ULL && inputAddr <= 0x03559000ULL;
}

inline uintptr_t operator""_g(unsigned long long val)
{
    return DynAddress(static_cast<uintptr_t>(val));
}

static size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// cdecl
template<typename Ret, typename... Args>
inline Ret cdecl_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__cdecl*)(Args...)>(addr)(args...);
}

// stdcall
template<typename Ret, typename... Args>
inline Ret stdcall_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__stdcall*)(Args...)>(addr)(args...);
}

// fastcall
template<typename Ret, typename... Args>
inline Ret fastcall_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__fastcall*)(Args...)>(addr)(args...);
}

// thiscall
template<typename Ret, typename... Args>
inline Ret thiscall_call(uintptr_t addr, Args... args) {
    return reinterpret_cast<Ret(__thiscall*)(Args...)>(addr)(args...);
}

struct FILEDATA
{
    std::string FilePath;			// Filepath to redirect to
    unsigned int file_size;			// We will need the file size so store it here
    bool MultiDef;					// For debug purposes
};
std::map<std::string, FILEDATA> DirCache;
std::string StringToLower(std::string strToConvert)
{
    std::transform(strToConvert.begin(), strToConvert.end(), strToConvert.begin(), ::tolower);

    return(strToConvert);
}

bool CreateCache(const char* DirListFile)
{
    FILE* DirListHandle = fopen(DirListFile, "r");
    if (!DirListHandle)
    {
        lextprint("Failed to open directory list file %s\n", DirListFile);
        return(false);
    }

    lextprint("Creating cache directory data from %s\n", DirListFile);

    char CurrentDirectory[MAX_PATH];
    char CurrentSearch[MAX_PATH];

    char PathBuffer[MAX_PATH];

    bool SearchRootVPP = false;

    while (fgets(CurrentDirectory, MAX_PATH, DirListHandle) != NULL)
    {

        // Remove any control codes from the end of the file path string
        for (int i = strlen(CurrentDirectory) - 1; i >= 0; i--)
        {
            if (CurrentDirectory[i] > 31)
                break;
            CurrentDirectory[i] = 0;
        }

        // If the line is blank or a comment (#) then skip

        if (!CurrentDirectory[0] || CurrentDirectory[0] == '#')
            continue;

        // If the file path is "." then set it to the current directory otherwise FindFirstFileA will search the root directory
        // of your drive.

        //if (!strcmp(CurrentDirectory, "."))
            //GetCurrentDirectoryA(MAX_PATH, CurrentDirectory);

        HANDLE SearchDirHandle;
        WIN32_FIND_DATAA FileData;

        strcpy_s(CurrentSearch, MAX_PATH, CurrentDirectory);
        PathAppendA(CurrentSearch, "*");

        SearchDirHandle = FindFirstFileA(CurrentSearch, &FileData);

        // Check for errors searching the directory
        if (SearchDirHandle == INVALID_HANDLE_VALUE)
        {
            lextprint("Unable to find directory %s\n", CurrentDirectory);
            continue;
        }

        lextprint("Adding contents of directory %s\n", CurrentDirectory);

        do
        {
            // Skip if it's a directory
            if (!strcmp(FileData.cFileName, ".") || !strcmp(FileData.cFileName, "..") || (FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;

            std::string SearchFileName(FileData.cFileName);
            SearchFileName = StringToLower(SearchFileName);
            auto filenamev = std::string_view(SearchFileName);
            if (!filenamev.ends_with("_gs.lua") &&
                !filenamev.ends_with("_ui.lua") &&
                !filenamev.ends_with(".cts"))
                continue;
            //MessageBoxA(0, FileData.cFileName, FileData.cFileName, 0);
            std::map<std::string, FILEDATA>::iterator itDirCache;

            itDirCache = DirCache.find(SearchFileName);

            if (itDirCache == DirCache.end())
            {
                FILEDATA PushData;
                strcpy_s(PathBuffer, MAX_PATH, CurrentDirectory);
                PathAppendA(PathBuffer, FileData.cFileName);
                std::string FullFindFilePath(PathBuffer);
                PushData.FilePath = FullFindFilePath;
                PushData.file_size = FileData.nFileSizeLow;
                PushData.MultiDef = false;
                DirCache[SearchFileName] = PushData;
            }
            else
                itDirCache->second.MultiDef = true;

        } while (FindNextFileA(SearchDirHandle, &FileData));

        FindClose(SearchDirHandle);
    }

    if (DirListHandle)
        fclose(DirListHandle);

    if (DirCache.empty())
        return(false);

    return(true);
}

union SemInfo
{
    long double r;
    void* ts;
};


struct Token
{
    int token;
    SemInfo seminfo;
};

struct Zio
{
    unsigned int n;
    const char* p;
    const char* (__cdecl* reader)(lua_State*, void*, unsigned int*);
    void* data;
    const char* name;
};

struct Mbuffer
{
    char* buffer;
    unsigned int buffsize;
};


struct LexState
{
    int current;
    int linenumber;
    int lastline;
    Token t;
    Token lookahead;
    void* fs;
    lua_State* L;
    Zio* z;
    Mbuffer* buff;
    void* source;
    int nestlevel;
};

struct luaL_Reg
{
    const char* name;
    lua_CFunction func;
};

struct mouse_button
{
    int last_click_time;
    bool just_pressed;
    bool just_released;
    bool double_clicked;
    bool down;
};

mouse_button* get_mouse_button(int index)
{
    mouse_button* buttons = (mouse_button*)0x0234F46C;
    if (index >= 0 && index < 3)
        return &buttons[index];
    return &buttons[2];
}

struct key
{
    bool down;
    bool just_down;
    bool masked;
    int time_down;
    int last_repeat;
};

key* get_keystate(int index)
{
    key* key_states = (key*)0x02348B80;
    if (index >= 0 && index < 256)
        return &key_states[index];
    return &key_states[255];
}

namespace LuaExtended
{

    int __cdecl luaZ_fill(Zio* z)
    {
        unsigned int size;
        const char* buff;

        buff = z->reader(0, z->data, &size);
        if (!buff || !size)
            return -1;
        z->n = size - 1;
        z->p = buff;
        return *(unsigned __int8*)z->p++;
    }

    static int lua_next_char(LexState* LS)
    {
        unsigned int n = LS->z->n;
        LS->z->n = n - 1;

        int c;
        if (n)
            c = *(unsigned char*)LS->z->p++;
        else
            c = luaZ_fill(LS->z);

        LS->current = c;
        return c;
    }

    static bool is_hex_digit_lua(int c)
    {
        return (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
    }

    static int hex_value_lua(int c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';

        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');

        if (c >= 'A' && c <= 'F')
            return 10 + (c - 'A');

        return -1;
    }

    static bool is_ident_char(int c)
    {
        return std::isalnum((unsigned char)c) || c == '_';
    }

    static bool try_read_hex_numeral(LexState* LS, int comma, SemInfo* seminfo)
    {
        /*
            Lua calls read_numeral(..., comma = 1) for numbers like .123.
            We only want normal 0x123 syntax, not weird .0x123 syntax.
        */
        if (comma)
            return false;

        if (LS->current != '0')
            return false;

        /*
            Peek after the 0.

            Important:
            luaZ_fill can mutate ZIO internals, so this simple save/restore peek
            is safest when the next character is already buffered.
        */
        unsigned int saved_n = LS->z->n;
        const char* saved_p = LS->z->p;
        int saved_current = LS->current;

        int after_zero = lua_next_char(LS);

        LS->z->n = saved_n;
        LS->z->p = saved_p;
        LS->current = saved_current;

        if (after_zero != 'x' && after_zero != 'X')
            return false;

        /*
            Confirmed hex literal.
            Now actually consume:
                0
                x/X
                hex digits
        */

        // consume '0'
        lua_next_char(LS);

        // consume 'x' or 'X'
        lua_next_char(LS);

        lua_Number value = 0.0;
        int digits = 0;

        while (is_hex_digit_lua(LS->current))
        {
            int hv = hex_value_lua(LS->current);

            value = (value * 16.0) + hv;
            digits++;

            lua_next_char(LS);
        }

        if (digits == 0)
        {
            __debugbreak();
            return true;
        }

        /*
            Reject things like:
                0x123g
                0x123_abc

            Without this, Lua could parse it as:
                number 0x123
                name g
            which is ugly and misleading.
        */
        if (is_ident_char(LS->current))
        {
            //luaX_lexerror(LS, "malformed number", TK_NUMBER_LUA50);
            __debugbreak();
            return true;
        }

        seminfo->r = value;
        return true;
    }
    SafetyHookInline o_read_numeral;
    void __cdecl hk_read_numeral(LexState* LS, int comma, SemInfo* seminfo)
    {
        if (try_read_hex_numeral(LS, comma, seminfo))
            return;

        o_read_numeral.unsafe_ccall<void>(LS, comma, seminfo);
    }

    // like a _lib so globals do savey!!
    uintptr_t luaL_loadbuffer_gameaddr = 0xCD9FD0_g;
    int luaL_loadbuffer_game(void* buffer, size_t size_buffer, lua_State* ls, const char* name)
    {
        int result;
        __asm {
            pushad
            pushfd
            mov eax, buffer
            mov ecx, size_buffer
            mov esi, ls
            push name
            call luaL_loadbuffer_gameaddr
            mov result, eax
            add esp, 4
            popfd
            popad
        }
    }

    struct lua_thread
    {
        uint16_t handle;
        lua_State* ls;
        lua_State* rs;
        bool un1;
        char* function_name;
        uint32_t context_handle;
        uint32_t flags;
        int num_pushed_args;
    };

    uintptr_t lua_execute_thread_immediate_game = 0xCE0740_g;
    int lua_execute_thread_immediate(lua_thread** t)
    {
        __asm
        {
            pushad
            mov eax, t
            call lua_execute_thread_immediate_game
            popad
        }
        return 0;
    }

    uintptr_t new_thread_internal_game = 0xCE01E0_g;
    lua_thread* new_thread_internal(lua_State* ls, const char* function_name, lua_State* sm, int main, int init)
    {
        lua_thread* l_thread;
        __asm
        {
            pushad
            mov eax, ls
            push init
            push main
            push sm
            push function_name
            call new_thread_internal_game
            mov l_thread, eax
            add esp, 0x10
            popad
        }
        return l_thread;
    }

    uintptr_t lua_does_function_exist_game = 0xCE0120_g;
    bool lua_does_function_exist(lua_State* ls, const char* function_name)
    {
        bool result;
        __asm
        {
            pushad
            mov eax, ls
            push function_name
            call lua_does_function_exist_game
            add esp, 0x4
            mov byte ptr[result], al
            popad
        }
        return result;
    }
    lua_State** Vint_lua_state = (lua_State**)0x0252A1B8_g;
    static bool read_file_binary(const std::string& path, std::vector<char>& out)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return false;

        std::streamsize size = file.tellg();
        if (size <= 0)
            return false;

        file.seekg(0, std::ios::beg);

        out.resize((size_t)size);

        if (!file.read(out.data(), size))
            return false;

        return true;
    }

    static std::string make_func_name(std::string_view filename, std::string_view prefix)
    {
        // "main_menu_ui.lua" -> "main_menu_ui_init"

        std::string name(filename);

        if (name.ends_with(".lua"))
            name.resize(name.size() - 4);

        name += prefix;
        return name;
    }

    void LoadExtendedLuaFiles(lua_State* ls, const char* prefix)
    {


        if (ls == nullptr)
        {
            lextprint("LuaExtended: Vint lua state is null\n");
            return;
        }

        for (auto& entry : DirCache)
        {
            const std::string& filename = entry.first; // should already be lowercase
            const std::string& filepath = entry.second.FilePath;

            // Only load loose VINT UI lua files.
            if (!std::string_view(filename).ends_with(prefix))
                continue;

            std::vector<char> buffer;

            if (!read_file_binary(filepath, buffer))
            {
                lextprint("LuaExtended: failed to read %s\n", filepath.c_str());
                continue;
            }
            //General::generalluaLoadBuff_disabled = true;
            int result = luaL_loadbuffer_game(
                buffer.data(),
                buffer.size(),
                ls,
                filename.c_str()
            );
            //General::generalluaLoadBuff_disabled = false;
            lextprint(
                "LuaExtended: loaded %s result=%d\n",
                filename.c_str(),
                result
            );

            auto init = make_func_name(filename, "_init");
            auto main = make_func_name(filename, "_main");
            auto thread = new_thread_internal(ls, init.c_str(), nullptr, 0, 1);
            if (thread) {
                lua_execute_thread_immediate(&thread);
            }

            auto thread_main = new_thread_internal(ls, main.c_str(), nullptr, 1, 0);

        }

    }

    void LoadVintExtended()
    {
        if (Vint_lua_state != nullptr)
        {
            LoadExtendedLuaFiles(*Vint_lua_state, "_ui.lua");
        }
    }

    uintptr_t luaL_openlib_retail = 0xCD9A30_g;
    void* luaL_openlib(lua_State* L, luaL_Reg* reg, const char* eh = "_G")
    {
        __asm
        {
            pushad
            mov ecx, L
            mov eax, reg
            push eh
            call luaL_openlib_retail
            add esp, 4
            popad
        }
        return NULL;
    }
    SafetyHookInline register_vint_lua_funcsD;

    //    static luaL_Reg lua_patching_functions[] = {
    //{"vint_subscribe_to_mouse_input", VintSubscribeToMouseInput},
    //{"vint_unsubscribe_to_mouse_input", VintUnsubscribeToMouseInput},
    //{"vint_get_current_clickable_element", VintGetCurrentClickableElement},
    //{"vint_get_object_bbox", VintGetObjectBBox},
    //{"vint_force_mouse_move_event", VintForceMouseMoveEvent},
    //{NULL, NULL}
    //};

    static std::string GetCurrentLuaSource(lua_State* L)
    {
        lua_Debug ar{};

        // level 0 = this C function
        // level 1 = Lua function that called this C function
        if (!lua_getstack(L, 1, &ar))
            return {};

        // "S" = source info
        if (!lua_getinfo(L, "S", &ar))
            return {};

        if (ar.source)
            return ar.source;

        if (ar.short_src)
            return ar.short_src;

        return {};
    }

    struct MemoryVar
    {
        enum class Type
        {
            Int,
            Int8,
            UInt8,
            Int16,
            UInt16,
            Int32,
            UInt32,
            Float,
            Double,
        };

        std::string name;
        Type type{};
        size_t offset{};

        union
        {
            int32_t i;
            int8_t i8;
            uint8_t u8;
            int16_t i16;
            uint16_t u16;
            int32_t i32;
            uint32_t u32;
            float f;
            double d;
        } value{};
    };

    struct MemoryBlock
    {
        std::string name;
        size_t requested_size{};
        size_t current_offset{};
        size_t allocated_size{};
        void* allocated_memory{};
        bool persistent{};
        std::vector<MemoryVar> vars;
    };

    static std::unordered_map<std::string, MemoryBlock*> g_RegisteredMemoryBlocks;
    static int g_NextMemoryHandle = 1;
    static std::unordered_map<int, MemoryBlock*> g_MemoryBlocksByHandle;
    static std::unordered_map<MemoryBlock*, int> g_MemoryHandlesByBlock;
    static std::unordered_map<std::string, int> g_MemoryHandlesByName;

    enum class IniValueType
    {
        Integer,
        Float,
        Double,
        Boolean,
        String,
    };

    struct IniMemoryBinding
    {
        std::string section;
        std::string key;
        IniValueType value_type{};
        int memory_handle{};
        std::string variable_name;
    };

    struct LuaIniHandle
    {
        std::unique_ptr<CIniReader> reader;
        std::vector<IniMemoryBinding> bindings;
    };

    static int g_NextIniHandle = 1;
    static std::unordered_map<int, LuaIniHandle> g_IniHandles;

    static int luaext_errorf(lua_State* L, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        lua_pushvfstring(L, fmt, args);
        va_end(args);
        return lua_error(L);
    }

    static const char* luaext_checkstring(lua_State* L, int index)
    {
        const char* value = lua_tostring(L, index);
        if (!value)
            luaext_errorf(L, "bad argument #%d (string expected)", index);
        return value;
    }

    static lua_Number luaext_checknumber(lua_State* L, int index)
    {
        if (!lua_isnumber(L, index))
            luaext_errorf(L, "bad argument #%d (number expected)", index);
        return lua_tonumber(L, index);
    }

    static int luaext_checkint(lua_State* L, int index)
    {
        return static_cast<int>(luaext_checknumber(L, index));
    }

    static void luaext_argcheck(lua_State* L, bool condition, int index, const char* message)
    {
        if (!condition)
            luaext_errorf(L, "bad argument #%d (%s)", index, message);
    }

    static uintptr_t get_memory_block_address(const MemoryBlock* block)
    {
        return block && block->allocated_memory
            ? reinterpret_cast<uintptr_t>(block->allocated_memory)
            : 0;
    }

    uintptr_t ResolveGameAddress(uintptr_t address)
    {
        return DynAddress(address);
    }

    uintptr_t ResolveNamedAllocationAddress(std::string_view block_name, std::string_view var_name)
    {
        auto it = g_RegisteredMemoryBlocks.find(std::string(block_name));
        if (it == g_RegisteredMemoryBlocks.end() || !it->second || !it->second->allocated_memory)
            return 0;

        MemoryBlock* block = it->second;
        for (const auto& var : block->vars)
        {
            if (var.name == var_name)
            {
                return reinterpret_cast<uintptr_t>(block->allocated_memory) + var.offset;
            }
        }

        return 0;
    }

    static size_t find_memory_var_index(const MemoryBlock* block, std::string_view name)
    {
        if (!block)
            return static_cast<size_t>(-1);

        for (size_t i = 0; i < block->vars.size(); ++i)
        {
            if (block->vars[i].name == name)
                return i;
        }

        return static_cast<size_t>(-1);
    }

    static int ensure_memory_block_handle(MemoryBlock* block)
    {
        if (!block)
            return 0;

        if (auto it = g_MemoryHandlesByBlock.find(block); it != g_MemoryHandlesByBlock.end())
            return it->second;

        int handle = g_NextMemoryHandle++;
        g_MemoryBlocksByHandle[handle] = block;
        g_MemoryHandlesByBlock[block] = handle;
        if (!block->name.empty())
            g_MemoryHandlesByName[block->name] = handle;
        return handle;
    }

    static MemoryBlock* get_memory_block_by_handle(lua_State* L, int index)
    {
        int handle = luaext_checkint(L, index);
        auto it = g_MemoryBlocksByHandle.find(handle);
        if (it == g_MemoryBlocksByHandle.end() || !it->second)
            luaext_errorf(L, "invalid memory handle");
        return it->second;
    }

    static LuaIniHandle& get_ini_handle(lua_State* L, int index)
    {
        int handle = luaext_checkint(L, index);
        auto it = g_IniHandles.find(handle);
        if (it == g_IniHandles.end() || !it->second.reader)
            luaext_errorf(L, "invalid ini handle");
        return it->second;
    }

    static void upsert_ini_binding(
        LuaIniHandle& ini,
        std::string_view section,
        std::string_view key,
        IniValueType value_type,
        int memory_handle,
        std::string_view variable_name)
    {
        for (auto& binding : ini.bindings)
        {
            if (binding.section == section && binding.key == key)
            {
                binding.value_type = value_type;
                binding.memory_handle = memory_handle;
                binding.variable_name = variable_name;
                return;
            }
        }

        IniMemoryBinding binding{};
        binding.section = std::string(section);
        binding.key = std::string(key);
        binding.value_type = value_type;
        binding.memory_handle = memory_handle;
        binding.variable_name = std::string(variable_name);
        ini.bindings.push_back(std::move(binding));
    }

    static void parse_ini_binding_args(
        lua_State* L,
        int first_optional_index,
        bool& create_if_missing,
        bool& has_binding,
        int& memory_handle,
        std::string& variable_name)
    {
        create_if_missing = true;
        has_binding = false;
        memory_handle = 0;
        variable_name.clear();

        int top = lua_gettop(L);
        int next_index = first_optional_index;

        if (top >= next_index && lua_isboolean(L, next_index))
        {
            create_if_missing = lua_toboolean(L, next_index) != 0;
            ++next_index;
        }

        if (top >= next_index + 1)
        {
            memory_handle = luaext_checkint(L, next_index);
            variable_name = luaext_checkstring(L, next_index + 1);
            has_binding = true;
        }
    }

    static size_t get_memory_var_alignment(MemoryVar::Type type)
    {
        switch (type)
        {
        case MemoryVar::Type::Int:
        case MemoryVar::Type::Int32:
            return alignof(int32_t);

        case MemoryVar::Type::Int8:
            return alignof(int8_t);

        case MemoryVar::Type::UInt8:
            return alignof(uint8_t);

        case MemoryVar::Type::Int16:
            return alignof(int16_t);

        case MemoryVar::Type::UInt16:
            return alignof(uint16_t);

        case MemoryVar::Type::UInt32:
            return alignof(uint32_t);

        case MemoryVar::Type::Float:
            return alignof(float);

        case MemoryVar::Type::Double:
            return alignof(double);
        }

        return 1;
    }

    static size_t get_memory_var_size(MemoryVar::Type type)
    {
        switch (type)
        {
        case MemoryVar::Type::Int:
        case MemoryVar::Type::Int32:
            return sizeof(int32_t);

        case MemoryVar::Type::Int8:
            return sizeof(int8_t);

        case MemoryVar::Type::UInt8:
            return sizeof(uint8_t);

        case MemoryVar::Type::Int16:
            return sizeof(int16_t);

        case MemoryVar::Type::UInt16:
            return sizeof(uint16_t);

        case MemoryVar::Type::UInt32:
            return sizeof(uint32_t);

        case MemoryVar::Type::Float:
            return sizeof(float);

        case MemoryVar::Type::Double:
            return sizeof(double);
        }

        return 0;
    }

    static void write_memory_var_value(const MemoryBlock& block, const MemoryVar& var)
    {
        if (!block.allocated_memory)
            return;

        uint8_t* dst = static_cast<uint8_t*>(block.allocated_memory) + var.offset;

        switch (var.type)
        {
        case MemoryVar::Type::Int:
            *reinterpret_cast<int32_t*>(dst) = var.value.i;
            break;

        case MemoryVar::Type::Int8:
            *reinterpret_cast<int8_t*>(dst) = var.value.i8;
            break;

        case MemoryVar::Type::UInt8:
            *reinterpret_cast<uint8_t*>(dst) = var.value.u8;
            break;

        case MemoryVar::Type::Int16:
            *reinterpret_cast<int16_t*>(dst) = var.value.i16;
            break;

        case MemoryVar::Type::UInt16:
            *reinterpret_cast<uint16_t*>(dst) = var.value.u16;
            break;

        case MemoryVar::Type::Int32:
            *reinterpret_cast<int32_t*>(dst) = var.value.i32;
            break;

        case MemoryVar::Type::UInt32:
            *reinterpret_cast<uint32_t*>(dst) = var.value.u32;
            break;

        case MemoryVar::Type::Float:
            *reinterpret_cast<float*>(dst) = var.value.f;
            break;

        case MemoryVar::Type::Double:
            *reinterpret_cast<double*>(dst) = var.value.d;
            break;
        }
    }

    static size_t add_memory_var(MemoryBlock* block, const char* name, MemoryVar::Type type)
    {
        if (!block || !name)
            return static_cast<size_t>(-1);

        if (find_memory_var_index(block, name) != static_cast<size_t>(-1))
            return static_cast<size_t>(-1);

        MemoryVar var{};
        var.name = name;
        var.type = type;
        var.offset = AlignUp(
            block->current_offset,
            get_memory_var_alignment(type)
        );

        block->current_offset = var.offset;

        switch (type)
        {
        case MemoryVar::Type::Int:
            var.value.i = 0;
            break;

        case MemoryVar::Type::Int8:
            var.value.i8 = 0;
            break;

        case MemoryVar::Type::UInt8:
            var.value.u8 = 0;
            break;

        case MemoryVar::Type::Int16:
            var.value.i16 = 0;
            break;

        case MemoryVar::Type::UInt16:
            var.value.u16 = 0;
            break;

        case MemoryVar::Type::Int32:
            var.value.i32 = 0;
            break;

        case MemoryVar::Type::UInt32:
            var.value.u32 = 0;
            break;

        case MemoryVar::Type::Float:
            var.value.f = 0.0f;
            break;

        case MemoryVar::Type::Double:
            var.value.d = 0.0;
            break;
        }

        block->current_offset += get_memory_var_size(type);
        block->vars.push_back(std::move(var));
        return block->vars.size() - 1;
    }

    static int Lua_MemoryCreateFn(lua_State* L)
    {
        const char* name = luaext_checkstring(L, 1);
        int requested_size = luaext_checkint(L, 2);

        luaext_argcheck(L, requested_size >= 0, 2, "size must be non-negative");

        if (auto existing = g_MemoryHandlesByName.find(name); existing != g_MemoryHandlesByName.end())
        {
            lua_pushnumber(L, static_cast<lua_Number>(existing->second));
            return 1;
        }

        if (auto registered = g_RegisteredMemoryBlocks.find(name); registered != g_RegisteredMemoryBlocks.end() && registered->second)
        {
            int handle = ensure_memory_block_handle(registered->second);
            lua_pushnumber(L, static_cast<lua_Number>(handle));
            return 1;
        }

        auto block = new MemoryBlock();
        block->name = name ? name : "MemoryBlock";
        block->requested_size = static_cast<size_t>(requested_size);

        int handle = ensure_memory_block_handle(block);
        lua_pushnumber(L, static_cast<lua_Number>(handle));
        return 1;
    }

    static int Lua_MemoryFindFn(lua_State* L)
    {
        const char* name = luaext_checkstring(L, 1);

        if (auto existing = g_MemoryHandlesByName.find(name); existing != g_MemoryHandlesByName.end())
        {
            lua_pushnumber(L, static_cast<lua_Number>(existing->second));
            return 1;
        }

        auto it = g_RegisteredMemoryBlocks.find(name);
        if (it == g_RegisteredMemoryBlocks.end() || !it->second)
        {
            lua_pushnil(L);
            return 1;
        }

        int handle = ensure_memory_block_handle(it->second);
        lua_pushnumber(L, static_cast<lua_Number>(handle));
        return 1;
    }

    static int Lua_MemoryPushVarFn(lua_State* L, MemoryVar::Type type)
    {
        auto block = get_memory_block_by_handle(L, 1);
        const char* name = luaext_checkstring(L, 2);

        if (block->allocated_memory)
            return luaext_errorf(L, "cannot add variables after Allocate");

        size_t index = add_memory_var(block, name, type);
        if (index == static_cast<size_t>(-1))
            return luaext_errorf(L, "variable '%s' already exists", name);

        lua_pushnumber(L, static_cast<lua_Number>(block->vars[index].offset));
        return 1;
    }

    static int Lua_MemoryPushI8Fn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::Int8); }
    static int Lua_MemoryPushU8Fn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::UInt8); }
    static int Lua_MemoryPushI16Fn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::Int16); }
    static int Lua_MemoryPushU16Fn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::UInt16); }
    static int Lua_MemoryPushI32Fn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::Int32); }
    static int Lua_MemoryPushU32Fn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::UInt32); }
    static int Lua_MemoryPushIntFn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::Int); }
    static int Lua_MemoryPushFloatFn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::Float); }
    static int Lua_MemoryPushDoubleFn(lua_State* L) { return Lua_MemoryPushVarFn(L, MemoryVar::Type::Double); }

    template <typename TValue>
    static int Lua_MemorySetValueFn(lua_State* L, MemoryVar::Type expected_type)
    {
        auto block = get_memory_block_by_handle(L, 1);
        const char* name = luaext_checkstring(L, 2);
        size_t index = find_memory_var_index(block, name);
        if (index == static_cast<size_t>(-1))
            return luaext_errorf(L, "variable '%s' does not exist", name);

        auto& var = block->vars[index];
        if (var.type != expected_type)
            return luaext_errorf(L, "setter called on incompatible variable type");

        if constexpr (std::is_same_v<TValue, int8_t>)
            var.value.i8 = static_cast<int8_t>(luaext_checkint(L, 3));
        else if constexpr (std::is_same_v<TValue, uint8_t>)
            var.value.u8 = static_cast<uint8_t>(luaext_checkint(L, 3));
        else if constexpr (std::is_same_v<TValue, int16_t>)
            var.value.i16 = static_cast<int16_t>(luaext_checkint(L, 3));
        else if constexpr (std::is_same_v<TValue, uint16_t>)
            var.value.u16 = static_cast<uint16_t>(luaext_checkint(L, 3));
        else if constexpr (std::is_same_v<TValue, int32_t>)
        {
            if (expected_type == MemoryVar::Type::Int)
                var.value.i = static_cast<int32_t>(luaext_checkint(L, 3));
            else
                var.value.i32 = static_cast<int32_t>(luaext_checkint(L, 3));
        }
        else if constexpr (std::is_same_v<TValue, uint32_t>)
            var.value.u32 = static_cast<uint32_t>(luaext_checknumber(L, 3));
        else if constexpr (std::is_same_v<TValue, float>)
            var.value.f = static_cast<float>(luaext_checknumber(L, 3));
        else if constexpr (std::is_same_v<TValue, double>)
            var.value.d = static_cast<double>(luaext_checknumber(L, 3));

        write_memory_var_value(*block, var);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_MemorySetI8Fn(lua_State* L) { return Lua_MemorySetValueFn<int8_t>(L, MemoryVar::Type::Int8); }
    static int Lua_MemorySetU8Fn(lua_State* L) { return Lua_MemorySetValueFn<uint8_t>(L, MemoryVar::Type::UInt8); }
    static int Lua_MemorySetI16Fn(lua_State* L) { return Lua_MemorySetValueFn<int16_t>(L, MemoryVar::Type::Int16); }
    static int Lua_MemorySetU16Fn(lua_State* L) { return Lua_MemorySetValueFn<uint16_t>(L, MemoryVar::Type::UInt16); }
    static int Lua_MemorySetI32Fn(lua_State* L) { return Lua_MemorySetValueFn<int32_t>(L, MemoryVar::Type::Int32); }
    static int Lua_MemorySetU32Fn(lua_State* L) { return Lua_MemorySetValueFn<uint32_t>(L, MemoryVar::Type::UInt32); }
    static int Lua_MemorySetIntFn(lua_State* L) { return Lua_MemorySetValueFn<int32_t>(L, MemoryVar::Type::Int); }
    static int Lua_MemorySetFloatFn(lua_State* L) { return Lua_MemorySetValueFn<float>(L, MemoryVar::Type::Float); }
    static int Lua_MemorySetDoubleFn(lua_State* L) { return Lua_MemorySetValueFn<double>(L, MemoryVar::Type::Double); }

    static int Lua_MemoryAllocateFn(lua_State* L)
    {
        auto block = get_memory_block_by_handle(L, 1);

        size_t final_size = block->requested_size;
        if (final_size == 0)
            final_size = block->current_offset;

        if (final_size == 0)
            return luaext_errorf(L, "memory block '%s' has no size", block->name.c_str());

        if (final_size < block->current_offset)
        {
            return luaext_errorf(
                L,
                "memory block '%s' requested size %d is smaller than used size %d",
                block->name.c_str(),
                static_cast<int>(final_size),
                static_cast<int>(block->current_offset)
            );
        }

        if (!block->allocated_memory)
        {
            block->allocated_memory = VirtualAlloc(
                nullptr,
                final_size,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE
            );

            if (!block->allocated_memory)
            {
                lua_pushnil(L);
                return 1;
            }

            block->allocated_size = final_size;
        }

        if (!block->persistent)
        {
            auto [it, inserted] = g_RegisteredMemoryBlocks.emplace(block->name, block);
            if (!inserted && it->second != block)
            {
                return luaext_errorf(
                    L,
                    "memory block '%s' is already registered",
                    block->name.c_str()
                );
            }

            block->persistent = true;
        }

        ensure_memory_block_handle(block);

        for (const auto& var : block->vars)
            write_memory_var_value(*block, var);

        lua_pushnumber(L, static_cast<lua_Number>(get_memory_block_address(block)));
        return 1;
    }

    static int Lua_MemoryGetAddressFn(lua_State* L)
    {
        auto block = get_memory_block_by_handle(L, 1);
        uintptr_t address = get_memory_block_address(block);
        if (!address)
            lua_pushnil(L);
        else
            lua_pushnumber(L, static_cast<lua_Number>(address));
        return 1;
    }

    static int Lua_MemoryGetSizeFn(lua_State* L)
    {
        auto block = get_memory_block_by_handle(L, 1);
        size_t size = block->allocated_size ? block->allocated_size :
            (block->requested_size ? block->requested_size : block->current_offset);
        lua_pushnumber(L, static_cast<lua_Number>(size));
        return 1;
    }

    static int Lua_MemoryGetFieldAddressFn(lua_State* L)
    {
        auto block = get_memory_block_by_handle(L, 1);
        const char* name = luaext_checkstring(L, 2);
        size_t index = find_memory_var_index(block, name);
        if (index == static_cast<size_t>(-1))
            return luaext_errorf(L, "variable '%s' does not exist", name);

        if (!block->allocated_memory)
        {
            lua_pushnil(L);
            return 1;
        }

        uintptr_t address = reinterpret_cast<uintptr_t>(block->allocated_memory) + block->vars[index].offset;
        lua_pushnumber(L, static_cast<lua_Number>(address));
        return 1;
    }

    static int Lua_MemoryFreeFn(lua_State* L)
    {
        auto block = get_memory_block_by_handle(L, 1);

        if (auto it = g_RegisteredMemoryBlocks.find(block->name);
            it != g_RegisteredMemoryBlocks.end() && it->second == block)
        {
            g_RegisteredMemoryBlocks.erase(it);
        }

        block->persistent = false;

        if (block->allocated_memory)
        {
            VirtualFree(block->allocated_memory, 0, MEM_RELEASE);
            block->allocated_memory = nullptr;
        }

        block->allocated_size = 0;
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniOpenFn(lua_State* L)
    {
        const char* path = luaext_checkstring(L, 1);

        int handle = g_NextIniHandle++;
        auto& ini = g_IniHandles[handle];
        ini.reader = std::make_unique<CIniReader>(path ? path : "");
        ini.bindings.clear();

        lua_pushnumber(L, static_cast<lua_Number>(handle));
        return 1;
    }

    static int Lua_IniCloseFn(lua_State* L)
    {
        int handle = luaext_checkint(L, 1);
        auto it = g_IniHandles.find(handle);
        if (it == g_IniHandles.end())
        {
            lua_pushboolean(L, 0);
            return 1;
        }

        g_IniHandles.erase(it);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniGetPathFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        lua_pushstring(L, ini.reader->GetIniPath().string().c_str());
        return 1;
    }

    static int Lua_IniGetIntFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        int default_value = luaext_checkint(L, 4);

        bool create_if_missing{};
        bool has_binding{};
        int memory_handle{};
        std::string variable_name;
        parse_ini_binding_args(L, 5, create_if_missing, has_binding, memory_handle, variable_name);

        int value = ini.reader->ReadInteger(section, key, default_value, create_if_missing);
        if (has_binding)
            upsert_ini_binding(ini, section, key, IniValueType::Integer, memory_handle, variable_name);

        lua_pushnumber(L, static_cast<lua_Number>(value));
        return 1;
    }

    static int Lua_IniGetFloatFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        float default_value = static_cast<float>(luaext_checknumber(L, 4));

        bool create_if_missing{};
        bool has_binding{};
        int memory_handle{};
        std::string variable_name;
        parse_ini_binding_args(L, 5, create_if_missing, has_binding, memory_handle, variable_name);

        float value = ini.reader->ReadFloat(section, key, default_value, create_if_missing);
        if (has_binding)
            upsert_ini_binding(ini, section, key, IniValueType::Float, memory_handle, variable_name);

        lua_pushnumber(L, static_cast<lua_Number>(value));
        return 1;
    }

    static int Lua_IniGetDoubleFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        double default_value = static_cast<double>(luaext_checknumber(L, 4));

        bool create_if_missing{};
        bool has_binding{};
        int memory_handle{};
        std::string variable_name;
        parse_ini_binding_args(L, 5, create_if_missing, has_binding, memory_handle, variable_name);

        double value = ini.reader->ReadDouble(section, key, default_value, create_if_missing);
        if (has_binding)
            upsert_ini_binding(ini, section, key, IniValueType::Double, memory_handle, variable_name);

        lua_pushnumber(L, static_cast<lua_Number>(value));
        return 1;
    }

    static int Lua_IniGetBoolFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        bool default_value = lua_toboolean(L, 4) != 0;

        bool create_if_missing{};
        bool has_binding{};
        int memory_handle{};
        std::string variable_name;
        parse_ini_binding_args(L, 5, create_if_missing, has_binding, memory_handle, variable_name);

        bool value = ini.reader->ReadBoolean(section, key, default_value, create_if_missing);
        if (has_binding)
            upsert_ini_binding(ini, section, key, IniValueType::Boolean, memory_handle, variable_name);

        lua_pushboolean(L, value ? 1 : 0);
        return 1;
    }

    static int Lua_IniGetStringFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        const char* default_value = luaext_checkstring(L, 4);

        bool create_if_missing{};
        bool has_binding{};
        int memory_handle{};
        std::string variable_name;
        parse_ini_binding_args(L, 5, create_if_missing, has_binding, memory_handle, variable_name);

        std::string value = ini.reader->ReadString(section, key, default_value, create_if_missing);
        if (has_binding)
            upsert_ini_binding(ini, section, key, IniValueType::String, memory_handle, variable_name);

        lua_pushstring(L, value.c_str());
        return 1;
    }

    static int Lua_IniSetIntFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        int value = luaext_checkint(L, 4);
        bool pretty = lua_gettop(L) >= 5 ? (lua_toboolean(L, 5) != 0) : false;

        ini.reader->WriteInteger(section, key, value, pretty);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniSetFloatFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        float value = static_cast<float>(luaext_checknumber(L, 4));
        bool pretty = lua_gettop(L) >= 5 ? (lua_toboolean(L, 5) != 0) : false;

        ini.reader->WriteFloat(section, key, value, pretty);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniSetDoubleFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        double value = static_cast<double>(luaext_checknumber(L, 4));
        bool pretty = lua_gettop(L) >= 5 ? (lua_toboolean(L, 5) != 0) : false;

        ini.reader->WriteDouble(section, key, value, pretty);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniSetBoolFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        bool value = lua_toboolean(L, 4) != 0;
        bool pretty = lua_gettop(L) >= 5 ? (lua_toboolean(L, 5) != 0) : false;

        ini.reader->WriteBoolean(section, key, value, pretty);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniSetStringFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        const char* value = luaext_checkstring(L, 4);
        bool pretty = lua_gettop(L) >= 5 ? (lua_toboolean(L, 5) != 0) : false;

        ini.reader->WriteString(section, key, value, pretty);
        lua_pushboolean(L, 1);
        return 1;
    }

    static int Lua_IniBindMemoryFn(lua_State* L)
    {
        auto& ini = get_ini_handle(L, 1);
        const char* section = luaext_checkstring(L, 2);
        const char* key = luaext_checkstring(L, 3);
        const char* type_name = luaext_checkstring(L, 4);
        int memory_handle = luaext_checkint(L, 5);
        const char* variable_name = luaext_checkstring(L, 6);

        IniValueType value_type{};
        if (_stricmp(type_name, "int") == 0 || _stricmp(type_name, "integer") == 0)
            value_type = IniValueType::Integer;
        else if (_stricmp(type_name, "float") == 0)
            value_type = IniValueType::Float;
        else if (_stricmp(type_name, "double") == 0)
            value_type = IniValueType::Double;
        else if (_stricmp(type_name, "bool") == 0 || _stricmp(type_name, "boolean") == 0)
            value_type = IniValueType::Boolean;
        else if (_stricmp(type_name, "string") == 0)
            value_type = IniValueType::String;
        else
            return luaext_errorf(L, "unknown ini binding type '%s'", type_name);

        upsert_ini_binding(ini, section, key, value_type, memory_handle, variable_name);
        lua_pushboolean(L, 1);
        return 1;
    }


    template <typename T>
    static int PatchValue(lua_State* L)
    {
        LuaArgs args(L);

        auto address = args.get<uintptr_t>();
        auto value = args.get<T>();
        auto vp = args.get_or<bool>(false);
        auto dyn = args.get_or<bool>(true);
        auto source = GetCurrentLuaSource(L);
        lextprint(
            "PatchValue called from Lua source: %s\n",
            source.c_str()
        );
        if (!isAddrInExe(address))
            dyn = false;
        if (vp && dyn)
            Memory::VP::DynBase::Patch(address, value);
        else if (vp && !dyn)
            Memory::VP::Patch(address, value);
        else if (!vp && dyn)
            Memory::DynBase::Patch(address, value);
        else
            Memory::Patch(address, value);

        return 0;
    }

    template <typename T>
    static int ReadValue(lua_State* L)
    {
        LuaArgs args(L);

        auto address = args.get<uintptr_t>();
        auto vp = args.get_or<bool>(false);
        auto dyn = args.get_or<bool>(true);
        T value{};

        if (!isAddrInExe(address))
            dyn = false;

        auto source = GetCurrentLuaSource(L);
        lextprint(
            "ReadValue called from Lua source: %s\n",
            source.c_str()
        );

        if (vp && dyn)
            Memory::VP::DynBase::Read(address, value);
        else if (vp && !dyn)
            Memory::VP::Read(address, value);
        else if (!vp && dyn)
            Memory::DynBase::Read(address, value);
        else
            Memory::Read(address, value);

        LuaReturns ret(L);
        ret.push(value);
        return ret.count();
    }

    int Patch_bool(lua_State* L)
    {
        return PatchValue<bool>(L);
    }

    int Read_bool(lua_State* L)
    {
        return ReadValue<bool>(L);
    }

    int Patch_int8_t(lua_State* L)
    {
        return PatchValue<int8_t>(L);
    }

    int Read_int8_t(lua_State* L)
    {
        return ReadValue<int8_t>(L);
    }

    int Patch_uint8_t(lua_State* L)
    {
        return PatchValue<uint8_t>(L);
    }

    int Read_uint8_t(lua_State* L)
    {
        return ReadValue<uint8_t>(L);
    }

    int Patch_int16_t(lua_State* L)
    {
        return PatchValue<int16_t>(L);
    }

    int Read_int16_t(lua_State* L)
    {
        return ReadValue<int16_t>(L);
    }

    int Patch_uint16_t(lua_State* L)
    {
        return PatchValue<uint16_t>(L);
    }

    int Read_uint16_t(lua_State* L)
    {
        return ReadValue<uint16_t>(L);
    }

    int Patch_int32_t(lua_State* L)
    {
        return PatchValue<int32_t>(L);
    }

    int Read_int32_t(lua_State* L)
    {
        return ReadValue<int32_t>(L);
    }

    int Patch_uint32_t(lua_State* L)
    {
        return PatchValue<uint32_t>(L);
    }

    int Read_uint32_t(lua_State* L)
    {
        return ReadValue<uint32_t>(L);
    }

    int Patch_int64_t(lua_State* L)
    {
        return PatchValue<int64_t>(L);
    }

    int Read_int64_t(lua_State* L)
    {
        return ReadValue<int64_t>(L);
    }

    int Patch_uint64_t(lua_State* L)
    {
        return PatchValue<uint64_t>(L);
    }

    int Read_uint64_t(lua_State* L)
    {
        return ReadValue<uint64_t>(L);
    }

    int Patch_float(lua_State* L)
    {
        return PatchValue<float>(L);
    }

    int Read_float(lua_State* L)
    {
        return ReadValue<float>(L);
    }

    int Patch_double(lua_State* L)
    {
        return PatchValue<double>(L);
    }

    int Read_double(lua_State* L)
    {
        return ReadValue<double>(L);
    }

    int Patch_uintptr_t(lua_State* L)
    {
        return PatchValue<uintptr_t>(L);
    }

    int Read_uintptr_t(lua_State* L)
    {
        return ReadValue<uintptr_t>(L);
    }

    int Patch_intptr_t(lua_State* L)
    {
        return PatchValue<intptr_t>(L);
    }

    int Read_intptr_t(lua_State* L)
    {
        return ReadValue<intptr_t>(L);
    }

    int Patch_size_t(lua_State* L)
    {
        return PatchValue<size_t>(L);
    }

    int Read_size_t(lua_State* L)
    {
        return ReadValue<size_t>(L);
    }

    int Lua_IsMousePressed(lua_State* L)
    {
        int index = luaext_checkint(L, 1);
        mouse_button* button = get_mouse_button(index);
        lua_pushboolean(L, button && button->just_pressed);
        return 1;
    }

    int Lua_IsMouseDown(lua_State* L)
    {
        int index = luaext_checkint(L, 1);
        mouse_button* button = get_mouse_button(index);
        lua_pushboolean(L, button && button->down);
        return 1;
    }

    int Lua_IsKeyDown(lua_State* L)
    {
        int index = luaext_checkint(L, 1);
        key* state = get_keystate(index);
        lua_pushboolean(L, state && state->down);
        return 1;
    }

    int Lua_IsKeyJustDown(lua_State* L)
    {
        int index = luaext_checkint(L, 1);
        key* state = get_keystate(index);
        lua_pushboolean(L, state && state->just_down);
        return 1;
    }

    int Lua_CompileAssembly(lua_State* L)
    {
        LuaArgs args(L);

        const char* name = args.get<const char*>();
        const char* asm_text = args.get<const char*>();

        uintptr_t compiled_address = LuaExtended::Assembly::CompileAssemblyScript(name, asm_text);

        if (compiled_address)
        {
            LuaReturns ret(L);
            ret.push(compiled_address);
            return ret.count();
        }

        lua_pushnil(L);
        return 1;
    }

    static luaL_Reg lua_patching_functions[] =
    {
        { "CompileAssembly", Lua_CompileAssembly },
        { "IsMousePressed", Lua_IsMousePressed },
        { "IsMouseDown",    Lua_IsMouseDown },
        { "IsKeyDown",      Lua_IsKeyDown },
        { "IsKeyJustDown",  Lua_IsKeyJustDown },
        { "PatchBool",   Patch_bool },
        { "ReadBool",    Read_bool },

        { "PatchI8",     Patch_int8_t },
        { "PatchU8",     Patch_uint8_t },
        { "ReadI8",      Read_int8_t },
        { "ReadU8",      Read_uint8_t },

        { "PatchI16",    Patch_int16_t },
        { "PatchU16",    Patch_uint16_t },
        { "ReadI16",     Read_int16_t },
        { "ReadU16",     Read_uint16_t },

        { "PatchI32",    Patch_int32_t },
        { "PatchU32",    Patch_uint32_t },
        { "ReadI32",     Read_int32_t },
        { "ReadU32",     Read_uint32_t },

        { "PatchI64",    Patch_int64_t },
        { "PatchU64",    Patch_uint64_t },
        { "ReadI64",     Read_int64_t },
        { "ReadU64",     Read_uint64_t },

        { "PatchFloat",  Patch_float },
        { "PatchDouble", Patch_double },
        { "ReadFloat",   Read_float },
        { "ReadDouble",  Read_double },

        { "PatchPtr",    Patch_uintptr_t },
        { "PatchSize",   Patch_size_t },
        { "ReadPtr",     Read_uintptr_t },
        { "ReadIPtr",    Read_intptr_t },
        { "ReadSize",    Read_size_t },

        { "MemoryCreate",          Lua_MemoryCreateFn },
        { "MemoryFind",            Lua_MemoryFindFn },
        { "MemoryPushI8",          Lua_MemoryPushI8Fn },
        { "MemoryPushU8",          Lua_MemoryPushU8Fn },
        { "MemoryPushI16",         Lua_MemoryPushI16Fn },
        { "MemoryPushU16",         Lua_MemoryPushU16Fn },
        { "MemoryPushI32",         Lua_MemoryPushI32Fn },
        { "MemoryPushU32",         Lua_MemoryPushU32Fn },
        { "MemoryPushInt",         Lua_MemoryPushIntFn },
        { "MemoryPushFloat",       Lua_MemoryPushFloatFn },
        { "MemoryPushDouble",      Lua_MemoryPushDoubleFn },
        { "MemorySetI8",           Lua_MemorySetI8Fn },
        { "MemorySetU8",           Lua_MemorySetU8Fn },
        { "MemorySetI16",          Lua_MemorySetI16Fn },
        { "MemorySetU16",          Lua_MemorySetU16Fn },
        { "MemorySetI32",          Lua_MemorySetI32Fn },
        { "MemorySetU32",          Lua_MemorySetU32Fn },
        { "MemorySetInt",          Lua_MemorySetIntFn },
        { "MemorySetFloat",        Lua_MemorySetFloatFn },
        { "MemorySetDouble",       Lua_MemorySetDoubleFn },
        { "MemoryWriteI8",         Lua_MemorySetI8Fn },
        { "MemoryWriteU8",         Lua_MemorySetU8Fn },
        { "MemoryWriteI16",        Lua_MemorySetI16Fn },
        { "MemoryWriteU16",        Lua_MemorySetU16Fn },
        { "MemoryWriteI32",        Lua_MemorySetI32Fn },
        { "MemoryWriteU32",        Lua_MemorySetU32Fn },
        { "MemoryWriteInt",        Lua_MemorySetIntFn },
        { "MemoryWriteFloat",      Lua_MemorySetFloatFn },
        { "MemoryWriteDouble",     Lua_MemorySetDoubleFn },
        { "MemoryAllocate",        Lua_MemoryAllocateFn },
        { "MemoryGetAddress",      Lua_MemoryGetAddressFn },
        { "MemoryGetSize",         Lua_MemoryGetSizeFn },
        { "MemoryGetFieldAddress", Lua_MemoryGetFieldAddressFn },
        { "MemoryFree",            Lua_MemoryFreeFn },

        { "IniOpen",               Lua_IniOpenFn },
        { "IniClose",              Lua_IniCloseFn },
        { "IniGetPath",            Lua_IniGetPathFn },
        { "IniGetInt",             Lua_IniGetIntFn },
        { "IniGetFloat",           Lua_IniGetFloatFn },
        { "IniGetDouble",          Lua_IniGetDoubleFn },
        { "IniGetBool",            Lua_IniGetBoolFn },
        { "IniGetString",          Lua_IniGetStringFn },
        { "IniSetInt",             Lua_IniSetIntFn },
        { "IniSetFloat",           Lua_IniSetFloatFn },
        { "IniSetDouble",          Lua_IniSetDoubleFn },
        { "IniSetBool",            Lua_IniSetBoolFn },
        { "IniSetString",          Lua_IniSetStringFn },
        { "IniWriteInt",           Lua_IniSetIntFn },
        { "IniWriteFloat",         Lua_IniSetFloatFn },
        { "IniWriteDouble",        Lua_IniSetDoubleFn },
        { "IniWriteBool",          Lua_IniSetBoolFn },
        { "IniWriteString",        Lua_IniSetStringFn },
        { "IniBindMemory",         Lua_IniBindMemoryFn },

        { NULL, NULL }
    };

    SAFETYHOOK_NOINLINE void register_vint_lua_funcs(lua_State* L)
    {
        register_vint_lua_funcsD.unsafe_ccall<void>();
        luaL_openlib(L, lua_patching_functions, "_G");

    }

    static void CallLuaDebugPrintOnly(lua_State* L, const char* message)
    {
        if (!L)
            return;

        int top = lua_gettop(L);

        lua_getglobal(L, "debug_print");

        if (!lua_isfunction(L, -1))
        {
            lua_settop(L, top);
            return;
        }

        lua_pushstring(L, message);

        int result = lua_pcall(L, 1, 0, 0);

        if (result != 0)
        {
            const char* err = lua_tostring(L, -1);

            lextprint(
                "debug_print pcall failed: %s\n",
                err ? err : "unknown error"
            );
        }

        lua_settop(L, top);
    }
    void Init()
    {
        //register_vint_lua_funcsD = safetyhook::create_inline(0x7F35A0, register_vint_lua_funcs);
        static auto vint_lib_hook = safetyhook::create_mid(0xB9155D, [](SafetyHookContext& ctx) {
            LoadVintExtended();
            });
        static auto system_lib = safetyhook::create_mid(0xA23E72, [](SafetyHookContext& ctx) {
            if (ctx.esi)
            {
                LoadExtendedLuaFiles((lua_State*)ctx.esi, "_gs.lua");
            }
            });

        static auto system_main2 = safetyhook::create_mid(0xCDE44F, [](SafetyHookContext& ctx) {
            luaL_openlib((lua_State*)ctx.esi, lua_patching_functions, "_G");


            });

        //static auto register_main = safetyhook::create_mid(0x89DA60, [](SafetyHookContext& ctx) {
        //    //luaL_openlib((lua_State*)ctx.eax, lua_patching_functions, "_G");
        //    luaL_openlib((lua_State*)ctx.eax, lua_patching_functions, "_G");

        //    });

        //static auto register_main2 = safetyhook::create_mid(0x7F3669, [](SafetyHookContext& ctx) {
        //    luaL_openlib((lua_State*)ctx.esi, lua_patching_functions, "_G");


        //    });

    }

    static std::string trim_copy(std::string s)
    {
        while (!s.empty() && std::isspace((unsigned char)s.front()))
            s.erase(s.begin());

        while (!s.empty() && std::isspace((unsigned char)s.back()))
            s.pop_back();

        return s;
    }

    static std::string lowercase_copy(std::string s)
    {
        std::ranges::transform(s, s.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
            });

        return s;
    }

    static std::string normalize_cts_name(std::string name)
    {
        name = lowercase_copy(trim_copy(name));

        std::replace(name.begin(), name.end(), '\\', '/');

        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos)
            name = name.substr(slash + 1);

        if (name.ends_with(".cts"))
            name.resize(name.size() - 4);

        return name;
    }

    static bool read_first_line(const std::string& path, std::string& out_line)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;

        return (bool)std::getline(file, out_line);
    }

    static bool read_cts_name_header(const std::string& path, std::string& out_name)
    {
        std::string line;

        if (!read_first_line(path, line))
            return false;

        line = trim_copy(line);

        if (!line.starts_with("//"))
            return false;

        out_name = normalize_cts_name(line.substr(2));
        return !out_name.empty();
    }

    struct LooseGsCtsFile
    {
        std::string filename;    // whatever_gs.cts
        std::string load_name;   // whatever_gs
        std::string filepath;
        std::string header_name;
    };

    uintptr_t load_cts_addr;

    char __cdecl load_cts(const char* name)
    {
        char result = cdecl_call<char>(load_cts_addr, name);
        if (!name)
            return result;

        const std::string requested_name = normalize_cts_name(name);

        std::vector<LooseGsCtsFile> gs_cts_files;

        for (auto& entry : DirCache)
        {
            const std::string& filename = entry.first; // should already be lowercase
            const std::string& filepath = entry.second.FilePath;

            if (!std::string_view(filename).ends_with("_gs.cts"))
                continue;

            LooseGsCtsFile file;
            file.filename = filename;
            file.filepath = filepath;
            file.load_name = filename;

            if (file.load_name.ends_with(".cts"))
                file.load_name.resize(file.load_name.size() - 4);

            read_cts_name_header(filepath, file.header_name);

            gs_cts_files.push_back(std::move(file));
        }

        std::unordered_set<std::string> loaded;

        auto load_extra_cts = [&](const LooseGsCtsFile& file)
            {
                if (!loaded.insert(file.filename).second)
                    return;

                lextprint(
                    "CTS: loading loose gs cts %s header=%s success=%d\n",
                    file.filename.c_str(),
                    file.header_name.c_str(),
                    static_cast<int>(cdecl_call<char>(load_cts_addr, file.load_name.c_str()))
                );

                ;
            };

        // First load the matching *_gs.cts.
        // Example:
        // requested name: sr2_city
        // file: whatever_gs.cts
        // first line: // sr2_city
        for (const auto& file : gs_cts_files)
        {
            if (!file.header_name.empty() && file.header_name == requested_name)
                load_extra_cts(file);
        }

        // Then load all other *_gs.cts files.
        for (const auto& file : gs_cts_files)
        {
            load_extra_cts(file);
        }

        return result;
    }

    //class LUAX {
    //public:
    //    LUAX() {
    //        Juiced::onAttach() += []() {

    //            };
    //    }
    //}LUAX;

    static bool HasConsole()
    {
        if (GetConsoleWindow() != nullptr)
            return true;

        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE)
            return false;

        DWORD mode = 0;
        return GetConsoleMode(out, &mode) != 0;
    }

    void OpenConsoleSafe()
    {
        static bool initialized = false;
        if (initialized)
            return;

        initialized = true;

        bool already_has_console = HasConsole();

        if (!already_has_console)
        {
            if (!AllocConsole())
            {
                if (GetLastError() != ERROR_ACCESS_DENIED)
                    return;
            }

            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);

            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);

            std::ios::sync_with_stdio(true);
        }

        lextprint("Oh, Hi Mark\n");
    }

    SafetyHookInline main_menu_renderD;

    void InGamePrintScale(int font, const char* a2, int a3, int a4, float a5) {
        InGamePrintASMSS(font, a2, a3, a4, a5);
    }

    void __cdecl main_menu_render_hook()
    {
        if (*(BYTE*)0x02527B75 == 1 && *(BYTE*)0xE8D56B == 1) {
            ChangeTextColor(255 / 2, 255 / 2, 255 / 2, 255);
            static int fuck1;
            static int fuck2;
            int gs_count = 0;
            int ui_count = 0;
            for (auto& entry : DirCache)
            {
                const std::string& filename = entry.first; // should already be lowercase
                const std::string& filepath = entry.second.FilePath;
                if (filename.ends_with("_gs.lua"))
                    gs_count++;
                else if (filename.ends_with("_ui.lua"))
                    ui_count++;
            }
            static char buffer[1024];
            sprintf_s(buffer, sizeof(buffer), "LuaExtended %s\nui: %d\ngs: %d", LEX_VERSION,ui_count , gs_count);

            InGamePrintScale(*(int*)0xE98A24, buffer, processtextwidth(0), 640, 0.7f);

        }
        main_menu_renderD.unsafe_ccall<void>();

    }
    void Attach()
    {

        OpenConsoleSafe();
        CIniReader ini;
        auto FileToParse = ini.ReadString("MAIN", "FileToParse", "loose.txt");
        CreateCache(FileToParse.c_str());
        o_read_numeral = safetyhook::create_inline(0xD70450, hk_read_numeral);
        LuaExtended::Init();
        InterceptCall(0xA248A3, load_cts_addr, load_cts);
        main_menu_renderD = safetyhook::create_inline(0x75B270, main_menu_render_hook);

    }
}



BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH: {
        static auto fuck_this_game = safetyhook::create_mid(0x51F449, [](SafetyHookContext& ctx) {
            LuaExtended::Attach();
            });
        

        HMODULE moduleHandle;
        // idk why but this makes it not DETATCH prematurely
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)DllMain, &moduleHandle);

    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

