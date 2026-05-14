// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"



#include <safetyhook.hpp>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>

#include <asmjit/x86.h>
#include <asmtk/asmtk.h>

#include <Zydis.h>
#include <regex>
#include <unordered_set>
#include "GLua.h"

#include "MemoryMgr.h"
#include <shlwapi.h>
#pragma comment(lib,"Shlwapi.lib")
#include <map>
#include "IniReader.h"
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

inline uintptr_t operator""_g(unsigned long long val)
{
    return DynAddress(static_cast<uintptr_t>(val));
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
        //Logger::TypedLog(CHN_DLL, "Failed to open directory list file {}\n", DirListFile);
        return(false);
    }

    //Logger::TypedLog(CHN_DLL, "Creating cache directory data from {}\n", DirListFile);

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
            //Logger::TypedLog(CHN_DLL, "Unable to find directory {}\n", CurrentDirectory);
            continue;
        }

        //Logger::TypedLog(CHN_DLL, "Adding contents of directory {}\n", CurrentDirectory);

        do
        {
            // Skip if it's a directory
            if (!strcmp(FileData.cFileName, ".") || !strcmp(FileData.cFileName, "..") || (FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;

            char* Extension = PathFindExtensionA(FileData.cFileName);

            // Blacklist of file extensions to skip
            if (_stricmp(Extension, ".lua") && _stricmp(Extension, ".cts"))
                continue;
            MessageBoxA(0, FileData.cFileName, FileData.cFileName, 0);
            std::string SearchFileName(FileData.cFileName);
            SearchFileName = StringToLower(SearchFileName);
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

struct AssemblyHookBlock
{
    std::string name;
    uintptr_t hook_address{};
    std::string label;
    std::string body;

    uintptr_t codecave_address{};
    uintptr_t return_address{};

    size_t stolen_size{};
    std::vector<uint8_t> relocated_original;
    std::vector<uint8_t> final_bytes;
};

struct InstalledAssemblyHook
{
    std::string name;
    uintptr_t hook_address{};
    uintptr_t codecave_address{};
    size_t stolen_size{};
    size_t used_size{};
};

static std::vector<InstalledAssemblyHook> g_InstalledAssemblyHooks;

static std::string Trim(std::string text)
{
    while (!text.empty() && std::isspace((unsigned char)text.front()))
        text.erase(text.begin());

    while (!text.empty() && std::isspace((unsigned char)text.back()))
        text.pop_back();

    return text;
}

static void ReplaceAll(std::string& text, const std::string& from, const std::string& to)
{
    size_t pos = 0;

    while ((pos = text.find(from, pos)) != std::string::npos)
    {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string HexAddress(uintptr_t address)
{
    return std::format("0{:X}h", address);
}

static bool ParseCodecaveHeader(const std::string& line, AssemblyHookBlock& out)
{
    constexpr std::string_view prefix = "(codecave:jmp)";

    if (!line.starts_with(prefix))
        return false;

    std::string rest = line.substr(prefix.size());

    auto comma = rest.find(',');
    if (comma == std::string::npos)
        return false;

    std::string address_text = Trim(rest.substr(0, comma));
    std::string label_text = Trim(rest.substr(comma + 1));

    if (!label_text.empty() && label_text.back() == ':')
        label_text.pop_back();

    out.hook_address = std::stoul(address_text, nullptr, 0);
    out.label = label_text;

    return out.hook_address != 0 && !out.label.empty();
}

static bool ParseAssemblyScript(const char* name, const char* script, AssemblyHookBlock& out)
{
    out = {};
    out.name = name ? name : "UnnamedAssemblyHook";

    std::istringstream stream(script ? script : "");
    std::string line;

    bool in_block = false;

    while (std::getline(stream, line))
    {
        line = Trim(line);

        if (line.empty())
            continue;

        if (!in_block)
        {
            if (ParseCodecaveHeader(line, out))
            {
                in_block = true;
                continue;
            }
        }
        else
        {
            if (line.find("%end%") != std::string::npos)
                break;

            out.body += line;
            out.body += "\n";
        }
    }

    return out.hook_address != 0 && !out.body.empty();
}

static bool AssembleX86Text(
    const std::string& asm_text,
    uint32_t base_address,
    std::vector<uint8_t>& out_bytes
)
{
    out_bytes.clear();

    asmjit::Environment env;
    env.set_arch(asmjit::Arch::kX86);

    asmjit::CodeHolder code;

    asmjit::Error err = code.init(env, base_address);
    if (err != asmjit::Error::kOk)
    {
        // TODO LOGGER
        //::Logger::TypedLog(
        //    CHN_DEBUG,
        //    "AsmTK: code.init failed: {}",
        //    asmjit::DebugUtils::error_as_string(err)
        //);

        return false;
    }

    asmjit::x86::Assembler assembler(&code);
    asmtk::AsmParser parser(&assembler);

    err = parser.parse(asm_text.c_str());

    if (err != asmjit::Error::kOk)
    {
        // TODO LOGGER
        //::Logger::TypedLog(
        //    CHN_DEBUG,
        //    "AsmTK parse failed: {}",
        //    asmjit::DebugUtils::error_as_string(err)
        //);

        //::Logger::TypedLog(
        //    CHN_DEBUG,
        //    "AsmTK input:\n{}",
        //    asm_text
        //);

        return false;
    }

    asmjit::Section* section = code.section_by_id(0);
    if (!section)
        return false;

    asmjit::CodeBuffer& buffer = section->buffer();

    out_bytes.assign(
        buffer.data(),
        buffer.data() + buffer.size()
    );

    return true;
}

static bool DecodeStolenSize(uintptr_t address, size_t min_size, size_t& out_stolen_size)
{
    out_stolen_size = 0;

    ZydisDecoder decoder;

    if (!ZYAN_SUCCESS(ZydisDecoderInit(
        &decoder,
        ZYDIS_MACHINE_MODE_LEGACY_32,
        ZYDIS_STACK_WIDTH_32
    )))
    {
        return false;
    }

    while (out_stolen_size < min_size)
    {
        ZydisDecodedInstruction instr{};

        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
            &decoder,
            nullptr,
            reinterpret_cast<const void*>(address + out_stolen_size),
            16,
            &instr
        )))
        {
            return false;
        }

        out_stolen_size += instr.length;
    }

    return true;
}

static bool RelocateRelativeInstructionX86(
    const ZydisDecodedInstruction& instr,
    const ZydisDecodedOperand* operands,
    uintptr_t old_ip,
    uintptr_t new_ip,
    std::vector<uint8_t>& bytes
)
{
    for (uint32_t i = 0; i < instr.operand_count; i++)
    {
        const ZydisDecodedOperand& op = operands[i];

        if (op.type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
            continue;

        if (!op.imm.is_relative)
            continue;

        uintptr_t old_next_ip = old_ip + instr.length;
        uintptr_t new_next_ip = new_ip + instr.length;

        int64_t old_disp = op.imm.value.s;
        uintptr_t target = static_cast<uintptr_t>(
            static_cast<int64_t>(old_next_ip) + old_disp
            );

        int64_t new_disp =
            static_cast<int64_t>(target) -
            static_cast<int64_t>(new_next_ip);

        uint8_t offset = instr.raw.imm[0].offset;
        uint8_t size = instr.raw.imm[0].size / 8;

        switch (size)
        {
        case 1:
            if (new_disp < INT8_MIN || new_disp > INT8_MAX)
            {
                // TODO : LOGGER
                //Logger::TypedLog(
                //    CHN_DEBUG,
                //    "Relocate: rel8 out of range at {:X}",
                //    old_ip
                //);
                return false;
            }

            *reinterpret_cast<int8_t*>(&bytes[offset]) =
                static_cast<int8_t>(new_disp);
            return true;

        case 2:
            if (new_disp < INT16_MIN || new_disp > INT16_MAX)
                return false;

            *reinterpret_cast<int16_t*>(&bytes[offset]) =
                static_cast<int16_t>(new_disp);
            return true;

        case 4:
            if (new_disp < INT32_MIN || new_disp > INT32_MAX)
                return false;

            *reinterpret_cast<int32_t*>(&bytes[offset]) =
                static_cast<int32_t>(new_disp);
            return true;
        }

        return false;
    }

    return true;
}

enum class RelocKind
{
    None,

    Rel32,
    JmpShort,
    JccShort,
    LoopShort,
    JecxzShort,
};

struct DecodedRelocInstruction
{
    uintptr_t old_ip{};
    size_t old_offset{};
    size_t new_offset{};

    ZydisDecodedInstruction instr{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};

    std::vector<uint8_t> original_bytes;

    RelocKind kind{ RelocKind::None };

    uintptr_t absolute_target{};
    uint8_t imm_offset{};
    uint8_t imm_size{};
    uint8_t opcode0{};
    uint8_t opcode1{};

    size_t emitted_size{};
};

static bool IsAddressInRange(uintptr_t address, uintptr_t start, uintptr_t end)
{
    return address >= start && address < end;
}

static bool GetRelativeImmediateInfo(
    const ZydisDecodedInstruction& instr,
    const ZydisDecodedOperand* operands,
    uintptr_t old_ip,
    uintptr_t& out_target,
    uint8_t& out_imm_offset,
    uint8_t& out_imm_size
)
{
    for (uint32_t i = 0; i < instr.operand_count; i++)
    {
        const ZydisDecodedOperand& op = operands[i];

        if (op.type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
            continue;

        if (!op.imm.is_relative)
            continue;

        uintptr_t old_next_ip = old_ip + instr.length;

        int64_t old_disp = op.imm.value.s;

        out_target = static_cast<uintptr_t>(
            static_cast<int64_t>(old_next_ip) + old_disp
            );

        out_imm_offset = instr.raw.imm[0].offset;
        out_imm_size = instr.raw.imm[0].size / 8;

        return true;
    }

    return false;
}

static RelocKind ClassifyRelativeInstruction(
    const ZydisDecodedInstruction& instr,
    const std::vector<uint8_t>& bytes,
    uint8_t imm_size
)
{
    if (bytes.empty())
        return RelocKind::None;

    uint8_t op0 = bytes[0];

    // short Jcc: 70 xx through 7F xx
    if (op0 >= 0x70 && op0 <= 0x7F && imm_size == 1)
        return RelocKind::JccShort;

    // short JMP: EB xx
    if (op0 == 0xEB && imm_size == 1)
        return RelocKind::JmpShort;

    // LOOPNZ / LOOPZ / LOOP / JECXZ
    if ((op0 == 0xE0 || op0 == 0xE1 || op0 == 0xE2) && imm_size == 1)
        return RelocKind::LoopShort;

    if (op0 == 0xE3 && imm_size == 1)
        return RelocKind::JecxzShort;

    // near relative call/jmp/jcc
    if (imm_size == 4)
        return RelocKind::Rel32;

    return RelocKind::None;
}

static size_t GetRelocatedInstructionSize(const DecodedRelocInstruction& ri)
{
    switch (ri.kind)
    {
    case RelocKind::JmpShort:
        // E9 rel32
        return 5;

    case RelocKind::JccShort:
        // opposite short jcc + near jmp
        // J!cc +5
        // JMP rel32
        return 7;

    case RelocKind::LoopShort:
    case RelocKind::JecxzShort:
        // loop/jecxz to near-jmp path:
        // loop/jecxz +5
        // jmp +5
        // jmp target
        return 12;

    default:
        return ri.instr.length;
    }
}

static bool BuildDecodedRelocInstructions(
    uintptr_t original_address,
    size_t stolen_size,
    std::vector<DecodedRelocInstruction>& out
)
{
    out.clear();

    ZydisDecoder decoder;

    if (!ZYAN_SUCCESS(ZydisDecoderInit(
        &decoder,
        ZYDIS_MACHINE_MODE_LEGACY_32,
        ZYDIS_STACK_WIDTH_32
    )))
    {
        return false;
    }

    size_t offset = 0;

    while (offset < stolen_size)
    {
        DecodedRelocInstruction ri{};
        ri.old_ip = original_address + offset;
        ri.old_offset = offset;

        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
            &decoder,
            reinterpret_cast<const void*>(ri.old_ip),
            16,
            &ri.instr,
            ri.operands
        )))
        {
            //::Logger::TypedLog(
            //    CHN_DEBUG,
            //    "Relocate: failed to decode instruction at {:X}",
            //    ri.old_ip
            //);

            return false;
        }

        ri.original_bytes.resize(ri.instr.length);

        memcpy(
            ri.original_bytes.data(),
            reinterpret_cast<const void*>(ri.old_ip),
            ri.instr.length
        );

        ri.opcode0 = ri.original_bytes.size() >= 1 ? ri.original_bytes[0] : 0;
        ri.opcode1 = ri.original_bytes.size() >= 2 ? ri.original_bytes[1] : 0;

        uintptr_t target{};
        uint8_t imm_offset{};
        uint8_t imm_size{};

        if (GetRelativeImmediateInfo(
            ri.instr,
            ri.operands,
            ri.old_ip,
            target,
            imm_offset,
            imm_size
        ))
        {
            ri.absolute_target = target;
            ri.imm_offset = imm_offset;
            ri.imm_size = imm_size;
            ri.kind = ClassifyRelativeInstruction(ri.instr, ri.original_bytes, imm_size);
        }

        ri.emitted_size = GetRelocatedInstructionSize(ri);

        out.push_back(std::move(ri));

        offset += out.back().instr.length;
    }

    return offset == stolen_size;
}

static bool AssignNewOffsets(std::vector<DecodedRelocInstruction>& instructions)
{
    size_t new_offset = 0;

    for (auto& ri : instructions)
    {
        ri.new_offset = new_offset;
        ri.emitted_size = GetRelocatedInstructionSize(ri);
        new_offset += ri.emitted_size;
    }

    return true;
}

static bool ResolveRelocatedTarget(
    const std::vector<DecodedRelocInstruction>& instructions,
    uintptr_t original_start,
    uintptr_t original_end,
    uintptr_t relocated_base,
    uintptr_t target,
    uintptr_t& out_target
)
{
    // Branch to the instruction immediately after stolen bytes.
    if (target == original_end)
    {
        out_target = original_end;
        return true;
    }

    // Branch outside the stolen region keeps its original absolute target.
    if (!IsAddressInRange(target, original_start, original_end))
    {
        out_target = target;
        return true;
    }

    // Branch inside stolen region must map to relocated instruction start.
    for (const auto& ri : instructions)
    {
        if (target == ri.old_ip)
        {
            out_target = relocated_base + ri.new_offset;
            return true;
        }
    }

    // Branch into the middle of an instruction is unsafe.
    //::Logger::TypedLog(
    //    CHN_DEBUG,
    //    "Relocate: target {:X} lands inside stolen block but not on instruction boundary",
    //    target
    //);

    return false;
}

static bool WriteRel32(
    std::vector<uint8_t>& bytes,
    size_t offset,
    uintptr_t source_instruction_ip,
    size_t instruction_size,
    uintptr_t absolute_target
)
{
    uintptr_t next_ip = source_instruction_ip + instruction_size;

    int64_t disp64 =
        static_cast<int64_t>(absolute_target) -
        static_cast<int64_t>(next_ip);

    if (disp64 < INT32_MIN || disp64 > INT32_MAX)
        return false;

    int32_t disp32 = static_cast<int32_t>(disp64);

    memcpy(bytes.data() + offset, &disp32, sizeof(disp32));
    return true;
}

static void EmitJmpRel32(
    std::vector<uint8_t>& out,
    uintptr_t instruction_ip,
    uintptr_t target
)
{
    out.push_back(0xE9);

    uintptr_t next_ip = instruction_ip + 5;

    int32_t disp = static_cast<int32_t>(
        static_cast<int64_t>(target) - static_cast<int64_t>(next_ip)
        );

    uint8_t* p = reinterpret_cast<uint8_t*>(&disp);

    out.insert(out.end(), p, p + sizeof(disp));
}

static bool EmitRelocatedInstruction(
    const std::vector<DecodedRelocInstruction>& instructions,
    const DecodedRelocInstruction& ri,
    uintptr_t original_start,
    uintptr_t original_end,
    uintptr_t relocated_base,
    std::vector<uint8_t>& out
)
{
    uintptr_t new_ip = relocated_base + ri.new_offset;

    uintptr_t relocated_target{};

    if (ri.kind != RelocKind::None)
    {
        if (!ResolveRelocatedTarget(
            instructions,
            original_start,
            original_end,
            relocated_base,
            ri.absolute_target,
            relocated_target
        ))
        {
            return false;
        }
    }

    switch (ri.kind)
    {
    case RelocKind::None:
    {
        out.insert(out.end(), ri.original_bytes.begin(), ri.original_bytes.end());
        return true;
    }

    case RelocKind::Rel32:
    {
        std::vector<uint8_t> bytes = ri.original_bytes;

        if (!WriteRel32(
            bytes,
            ri.imm_offset,
            new_ip,
            ri.instr.length,
            relocated_target
        ))
        {
            //::Logger::TypedLog(
            //    CHN_DEBUG,
            //    "Relocate: rel32 out of range at {:X}",
            //    ri.old_ip
            //);

            return false;
        }

        out.insert(out.end(), bytes.begin(), bytes.end());
        return true;
    }

    case RelocKind::JmpShort:
    {
        // jmp short target
        // becomes:
        // jmp rel32 target
        EmitJmpRel32(out, new_ip, relocated_target);
        return true;
    }

    case RelocKind::JccShort:
    {
        // jcc short target
        // becomes:
        // opposite_jcc +5
        // jmp rel32 target
        //
        // Example:
        // je target
        // becomes:
        // jne skip
        // jmp target
        // skip:

        uint8_t original_jcc = ri.opcode0;
        uint8_t opposite_jcc = original_jcc ^ 1;

        out.push_back(opposite_jcc);
        out.push_back(0x05); // skip over 5-byte near jmp

        EmitJmpRel32(out, new_ip + 2, relocated_target);
        return true;
    }

    case RelocKind::LoopShort:
    case RelocKind::JecxzShort:
    {
        // loop/jecxz target
        // becomes:
        //
        // loop/jecxz go_to_target_jmp
        // jmp after
        // go_to_target_jmp:
        // jmp target
        // after:
        //
        // Layout:
        // 0: E0/E1/E2/E3 05
        // 2: E9 05 00 00 00
        // 7: E9 xx xx xx xx
        // 12:

        uint8_t op = ri.opcode0;

        // original conditional loop/jecxz jumps to the near-jmp at +7.
        out.push_back(op);
        out.push_back(0x05);

        // if condition false, jump over the target jmp.
        EmitJmpRel32(out, new_ip + 2, new_ip + 12);

        // if condition true, jump to actual relocated target.
        EmitJmpRel32(out, new_ip + 7, relocated_target);

        return true;
    }
    }

    return false;
}

static bool RelocateOriginalCodeX86(
    uintptr_t original_address,
    uintptr_t relocated_address,
    size_t stolen_size,
    std::vector<uint8_t>& out_bytes
)
{
    out_bytes.clear();

    std::vector<DecodedRelocInstruction> instructions;

    if (!BuildDecodedRelocInstructions(
        original_address,
        stolen_size,
        instructions
    ))
    {
        return false;
    }

    AssignNewOffsets(instructions);

    uintptr_t original_start = original_address;
    uintptr_t original_end = original_address + stolen_size;

    for (const auto& ri : instructions)
    {
        size_t before = out_bytes.size();

        if (!EmitRelocatedInstruction(
            instructions,
            ri,
            original_start,
            original_end,
            relocated_address,
            out_bytes
        ))
        {
            //::Logger::TypedLog(
            //    CHN_DEBUG,
            //    "RelocateOriginalCodeX86: failed at {:X}",
            //    ri.old_ip
            //);

            return false;
        }

        size_t emitted = out_bytes.size() - before;

        if (emitted != ri.emitted_size)
        {
            //::Logger::TypedLog(
            //    CHN_DEBUG,
            //    "RelocateOriginalCodeX86: size mismatch at {:X}. expected={} got={}",
            //    ri.old_ip,
            //    ri.emitted_size,
            //    emitted
            //);

            return false;
        }
    }

    return true;
}

static void* AllocExecutable(size_t size)
{
    return VirtualAlloc(
        nullptr,
        size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE
    );
}

static bool WriteRelativeJump(uintptr_t src, uintptr_t dst, size_t patch_size)
{
    if (patch_size < 5)
        return false;

    DWORD old_protect{};

    if (!VirtualProtect(
        reinterpret_cast<void*>(src),
        patch_size,
        PAGE_EXECUTE_READWRITE,
        &old_protect
    ))
    {
        return false;
    }

    uint8_t* p = reinterpret_cast<uint8_t*>(src);

    p[0] = 0xE9;

    int32_t rel = static_cast<int32_t>(dst - (src + 5));
    memcpy(p + 1, &rel, sizeof(rel));

    for (size_t i = 5; i < patch_size; i++)
        p[i] = 0x90;

    DWORD temp{};
    VirtualProtect(
        reinterpret_cast<void*>(src),
        patch_size,
        old_protect,
        &temp
    );

    FlushInstructionCache(
        GetCurrentProcess(),
        reinterpret_cast<void*>(src),
        patch_size
    );

    return true;
}

static std::string PreprocessGameAddressTokens(const std::string& input)
{
    std::string output;
    output.reserve(input.size());

    // Matches:
    // 0x9B2FE0_g
    // 0X9B2FE0_g
    static const std::regex game_addr_regex(R"(0[xX]([0-9A-Fa-f]+)_g)");

    std::sregex_iterator it(input.begin(), input.end(), game_addr_regex);
    std::sregex_iterator end;

    size_t last_pos = 0;

    for (; it != end; ++it)
    {
        const std::smatch& match = *it;

        output.append(input, last_pos, match.position() - last_pos);

        std::string hex_text = match[1].str();

        uintptr_t raw_address = static_cast<uintptr_t>(
            std::stoull(hex_text, nullptr, 16)
            );

        uintptr_t final_address = DynAddress(raw_address);

        output += HexAddress(final_address);

        last_pos = match.position() + match.length();
    }

    output.append(input, last_pos, std::string::npos);

    return output;
}

static std::string PreprocessCommon(const std::string& input, const AssemblyHookBlock& block)
{
    std::string out = input;

    ReplaceAll(out, "%returnaddress%", HexAddress(block.return_address));
    out = PreprocessGameAddressTokens(out);
    return out;
}

struct CodeCavePage
{
    uint8_t* base{};
    size_t size{};
    size_t used{};
};

static std::vector<CodeCavePage> g_CodeCavePages;

static size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static size_t GetPageSize()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize;
}

static void* AllocateCodeCaveFromPool(size_t used_size, size_t alignment = 16)
{
    if (used_size == 0)
        return nullptr;

    used_size = AlignUp(used_size, alignment);

    for (auto& page : g_CodeCavePages)
    {
        size_t aligned_used = AlignUp(page.used, alignment);

        if (aligned_used + used_size <= page.size)
        {
            void* result = page.base + aligned_used;
            page.used = aligned_used + used_size;
            return result;
        }
    }

    size_t page_size = GetPageSize();
    size_t alloc_size = AlignUp(max(used_size, page_size), page_size);

    void* mem = VirtualAlloc(
        nullptr,
        alloc_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE
    );

    if (!mem)
        return nullptr;

    CodeCavePage page{};
    page.base = static_cast<uint8_t*>(mem);
    page.size = alloc_size;
    page.used = used_size;

    g_CodeCavePages.push_back(page);

    return mem;
}

static bool CompileAssemblyBlock(AssemblyHookBlock& block)
{
    constexpr size_t JMP_SIZE = 5;

    if (!DecodeStolenSize(block.hook_address, JMP_SIZE, block.stolen_size))
    {
        //::Logger::TypedLog(
        //    CHN_DEBUG,
        //    "CompileAssembly: failed to decode stolen size at {:X}",
        //    block.hook_address
        //);

        return false;
    }

    block.return_address = block.hook_address + block.stolen_size;

    std::string body = block.body;

    size_t original_pos = body.find("%originalcode%");

    std::string pre_text;
    std::string post_text;

    if (original_pos == std::string::npos)
    {
        pre_text = body;
        post_text.clear();
    }
    else
    {
        pre_text = body.substr(0, original_pos);
        post_text = body.substr(original_pos + strlen("%originalcode%"));
    }

    pre_text = PreprocessCommon(pre_text, block);
    post_text = PreprocessCommon(post_text, block);

    /*
        First pass:
        Use a fake base just to estimate size.
        This is not final because relative jumps/calls need the real address.
    */
    constexpr uintptr_t TEMP_BASE = 0x50000000;

    std::vector<uint8_t> temp_pre_bytes;
    if (!AssembleX86Text(
        pre_text,
        static_cast<uint32_t>(TEMP_BASE),
        temp_pre_bytes
    ))
    {
        return false;
    }

    std::vector<uint8_t> temp_original;

    if (original_pos != std::string::npos)
    {
        if (!RelocateOriginalCodeX86(
            block.hook_address,
            TEMP_BASE + temp_pre_bytes.size(),
            block.stolen_size,
            temp_original
        ))
        {
            return false;
        }
    }

    std::vector<uint8_t> temp_post_bytes;

    if (!post_text.empty())
    {
        if (!AssembleX86Text(
            post_text,
            static_cast<uint32_t>(TEMP_BASE + temp_pre_bytes.size() + temp_original.size()),
            temp_post_bytes
        ))
        {
            return false;
        }
    }

    size_t estimated_size =
        temp_pre_bytes.size() +
        temp_original.size() +
        temp_post_bytes.size();

    if (estimated_size == 0)
    {
        //::Logger::TypedLog(CHN_DEBUG, "CompileAssembly: estimated size was zero");
        return false;
    }

    /*
        Allocate based on actual used size.
        VirtualAlloc will still round internally to a page, but our hook tracks exact used size.
    */
    size_t alloc_size = 0;
    void* cave = AllocateCodeCaveFromPool(estimated_size);

    if (!cave)
    {
        //::Logger::TypedLog(CHN_DEBUG, "CompileAssembly: codecave pool allocation failed");
        return false;
    }

    if (!cave)
    {
        //::Logger::TypedLog(CHN_DEBUG, "CompileAssembly: VirtualAlloc failed");
        return false;
    }

    block.codecave_address = reinterpret_cast<uintptr_t>(cave);

    /*
        Second pass:
        Reassemble using the real codecave address.
    */
    std::vector<uint8_t> pre_bytes;

    if (!AssembleX86Text(
        pre_text,
        static_cast<uint32_t>(block.codecave_address),
        pre_bytes
    ))
    {
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }

    uintptr_t relocated_original_address =
        block.codecave_address + pre_bytes.size();

    block.relocated_original.clear();

    if (original_pos != std::string::npos)
    {
        if (!RelocateOriginalCodeX86(
            block.hook_address,
            relocated_original_address,
            block.stolen_size,
            block.relocated_original
        ))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return false;
        }
    }

    uintptr_t post_address =
        block.codecave_address +
        pre_bytes.size() +
        block.relocated_original.size();

    std::vector<uint8_t> post_bytes;

    if (!post_text.empty())
    {
        if (!AssembleX86Text(
            post_text,
            static_cast<uint32_t>(post_address),
            post_bytes
        ))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return false;
        }
    }

    block.final_bytes.clear();
    block.final_bytes.insert(block.final_bytes.end(), pre_bytes.begin(), pre_bytes.end());
    block.final_bytes.insert(block.final_bytes.end(), block.relocated_original.begin(), block.relocated_original.end());
    block.final_bytes.insert(block.final_bytes.end(), post_bytes.begin(), post_bytes.end());

    if (block.final_bytes.empty())
    {
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }

    /*
        If second pass got bigger than first pass, reallocate and compile again.
        This can happen if instruction encoding changes due to address distance.
    */
    if (block.final_bytes.size() > estimated_size)
    {
        VirtualFree(cave, 0, MEM_RELEASE);

        estimated_size = block.final_bytes.size();
        cave = AllocateCodeCaveFromPool(estimated_size);

        if (!cave)
        {
            //::Logger::TypedLog(CHN_DEBUG, "CompileAssembly: codecave pool allocation failed");
            return false;
        }

        if (!cave)
            return false;

        block.codecave_address = reinterpret_cast<uintptr_t>(cave);

        // Re-run second pass with new real address.
        pre_bytes.clear();
        post_bytes.clear();
        block.relocated_original.clear();
        block.final_bytes.clear();

        if (!AssembleX86Text(
            pre_text,
            static_cast<uint32_t>(block.codecave_address),
            pre_bytes
        ))
        {
            VirtualFree(cave, 0, MEM_RELEASE);
            return false;
        }

        relocated_original_address = block.codecave_address + pre_bytes.size();

        if (original_pos != std::string::npos)
        {
            if (!RelocateOriginalCodeX86(
                block.hook_address,
                relocated_original_address,
                block.stolen_size,
                block.relocated_original
            ))
            {
                VirtualFree(cave, 0, MEM_RELEASE);
                return false;
            }
        }

        post_address =
            block.codecave_address +
            pre_bytes.size() +
            block.relocated_original.size();

        if (!post_text.empty())
        {
            if (!AssembleX86Text(
                post_text,
                static_cast<uint32_t>(post_address),
                post_bytes
            ))
            {
                VirtualFree(cave, 0, MEM_RELEASE);
                return false;
            }
        }

        block.final_bytes.insert(block.final_bytes.end(), pre_bytes.begin(), pre_bytes.end());
        block.final_bytes.insert(block.final_bytes.end(), block.relocated_original.begin(), block.relocated_original.end());
        block.final_bytes.insert(block.final_bytes.end(), post_bytes.begin(), post_bytes.end());
    }

    memcpy(cave, block.final_bytes.data(), block.final_bytes.size());

    FlushInstructionCache(
        GetCurrentProcess(),
        cave,
        block.final_bytes.size()
    );

    if (!WriteRelativeJump(
        block.hook_address,
        block.codecave_address,
        block.stolen_size
    ))
    {
        //::Logger::TypedLog(CHN_DEBUG, "CompileAssembly: failed to write JMP");
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }

    g_InstalledAssemblyHooks.push_back({
        block.name,
        block.hook_address,
        block.codecave_address,
        block.stolen_size,
        block.final_bytes.size()
        });

    //::Logger::TypedLog(
    //    CHN_DEBUG,
    //    "CompileAssembly: {} hooked {:X} -> {:X}, stolen={} used={} alloc={}",
    //    block.name,
    //    block.hook_address,
    //    block.codecave_address,
    //    block.stolen_size,
    //    block.final_bytes.size(),
    //    alloc_size
    //);

    return true;
}

static bool CompileAssemblyScript(const char* name, const char* script)
{
    AssemblyHookBlock block;

    if (!ParseAssemblyScript(name, script, block))
    {
        //Logger::TypedLog(CHN_DEBUG, "CompileAssembly: parse failed");
        return false;
    }

    return CompileAssemblyBlock(block);
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
            //Logger::TypedLog(CHN_DEBUG, "LuaExtended: Vint lua state is null");
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
                //Logger::TypedLog(CHN_DEBUG, "LuaExtended: failed to read {}", filepath);
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
            //Logger::TypedLog(
            //    CHN_DEBUG,
            //    "LuaExtended: loaded {} result={}",
            //    filename,
            //    result
            //);

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
    void* luaL_openlib(lua_State* L, luaL_Reg* reg, const char* eh)
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

    template <typename T>
    static int PatchValue(lua_State* L)
    {
        LuaArgs args(L);

        auto address = args.get<uintptr_t>();
        auto value = args.get<T>();
        auto source = GetCurrentLuaSource(L);
        //Logger::TypedLog(
        //    CHN_DEBUG,
        //    "PatchValue called from Lua source: {}",
        //    source
        //);

        Patch<T>(address, value);

        return 0;
    }

    int Patch_bool(lua_State* L)
    {
        return PatchValue<bool>(L);
    }

    int Patch_int8_t(lua_State* L)
    {
        return PatchValue<int8_t>(L);
    }

    int Patch_uint8_t(lua_State* L)
    {
        return PatchValue<uint8_t>(L);
    }

    int Patch_int16_t(lua_State* L)
    {
        return PatchValue<int16_t>(L);
    }

    int Patch_uint16_t(lua_State* L)
    {
        return PatchValue<uint16_t>(L);
    }

    int Patch_int32_t(lua_State* L)
    {
        return PatchValue<int32_t>(L);
    }

    int Patch_uint32_t(lua_State* L)
    {
        return PatchValue<uint32_t>(L);
    }

    int Patch_int64_t(lua_State* L)
    {
        return PatchValue<int64_t>(L);
    }

    int Patch_uint64_t(lua_State* L)
    {
        return PatchValue<uint64_t>(L);
    }

    int Patch_float(lua_State* L)
    {
        return PatchValue<float>(L);
    }

    int Patch_double(lua_State* L)
    {
        return PatchValue<double>(L);
    }

    int Patch_uintptr_t(lua_State* L)
    {
        return PatchValue<uintptr_t>(L);
    }

    int Patch_intptr_t(lua_State* L)
    {
        return PatchValue<intptr_t>(L);
    }

    int Patch_size_t(lua_State* L)
    {
        return PatchValue<size_t>(L);
    }

    int Lua_CompileAssembly(lua_State* L)
    {
        LuaArgs args(L);

        const char* name = args.get<const char*>();
        const char* asm_text = args.get<const char*>();

        bool ok = CompileAssemblyScript(name, asm_text);

        LuaReturns ret(L);
        ret.push(ok);
        return ret.count();
    }

    static luaL_Reg lua_patching_functions[] =
    {
        { "CompileAssembly", Lua_CompileAssembly },
        { "PatchBool",   Patch_bool },

        { "PatchI8",     Patch_int8_t },
        { "PatchU8",     Patch_uint8_t },

        { "PatchI16",    Patch_int16_t },
        { "PatchU16",    Patch_uint16_t },

        { "PatchI32",    Patch_int32_t },
        { "PatchU32",    Patch_uint32_t },

        { "PatchI64",    Patch_int64_t },
        { "PatchU64",    Patch_uint64_t },

        { "PatchFloat",  Patch_float },
        { "PatchDouble", Patch_double },

        { "PatchPtr",    Patch_uintptr_t },
        { "PatchSize",   Patch_size_t },

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

            //::Logger::TypedLog(
            //    CHN_DEBUG,
            //    "debug_print pcall failed: {}",
            //    err ? err : "unknown error"
            //);
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

        static auto register_main = safetyhook::create_mid(0x89DA60, [](SafetyHookContext& ctx) {
            //luaL_openlib((lua_State*)ctx.eax, lua_patching_functions, "_G");


            });

        static auto register_main2 = safetyhook::create_mid(0x7F3669, [](SafetyHookContext& ctx) {
            luaL_openlib((lua_State*)ctx.esi, lua_patching_functions, "_G");


            });

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

                //Logger::TypedLog(
                //    CHN_DEBUG,
                //    "CTS: loading loose gs cts {} header={} success={}",
                //    file.filename,
                //    file.header_name,
                //    cdecl_call<char>(load_cts_addr, file.load_name.c_str())
                //);

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
    void Attach()
    {
        CIniReader ini;
        auto FileToParse = ini.ReadString("MAIN", "FileToParse", "loose.txt");
        CreateCache(FileToParse.c_str());
        o_read_numeral = safetyhook::create_inline(0xD70450, hk_read_numeral);
        LuaExtended::Init();
        InterceptCall(0xA248A3, load_cts_addr, load_cts);

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

