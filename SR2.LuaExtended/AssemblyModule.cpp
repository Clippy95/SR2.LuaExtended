#include "pch.h"

#include "AssemblyModule.h"

#include <asmjit/x86.h>
#include <asmtk/asmtk.h>

#include <Zydis.h>

#include <cctype>
#include <format>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#define lextprint(format, ...) \
    do { \
            printf("[LUA Extended] " format, ##__VA_ARGS__); \
    } while(0)

namespace LuaExtended
{
    uintptr_t ResolveGameAddress(uintptr_t address);
    uintptr_t ResolveNamedAllocationAddress(std::string_view block_name, std::string_view var_name);
}

namespace
{
    struct AssemblyHookBlock
    {
        std::string name;
        enum class Mode
        {
            Standalone,
            CodecaveJmp,
        };

        Mode mode{ Mode::Standalone };
        uintptr_t hook_address{};
        std::string label;
        std::string body;

        uintptr_t compiled_address{};
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

    std::vector<InstalledAssemblyHook> g_InstalledAssemblyHooks;

    std::string Trim(std::string text)
    {
        while (!text.empty() && std::isspace((unsigned char)text.front()))
            text.erase(text.begin());

        while (!text.empty() && std::isspace((unsigned char)text.back()))
            text.pop_back();

        return text;
    }

    void ReplaceAll(std::string& text, const std::string& from, const std::string& to)
    {
        size_t pos = 0;

        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    std::string HexAddress(uintptr_t address)
    {
        return std::format("0{:X}h", address);
    }

    bool ParseCodecaveHeader(const std::string& line, AssemblyHookBlock& out)
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

        out.mode = AssemblyHookBlock::Mode::CodecaveJmp;
        out.hook_address = std::stoul(address_text, nullptr, 0);
        out.label = label_text;

        return out.hook_address != 0 && !out.label.empty();
    }

    bool ParseAssemblyScript(const char* name, const char* script, AssemblyHookBlock& out)
    {
        out = {};
        out.name = name ? name : "UnnamedAssemblyHook";

        std::istringstream stream(script ? script : "");
        std::string line;
        std::vector<std::string> lines;

        while (std::getline(stream, line))
        {
            line = Trim(line);

            if (line.empty())
                continue;

            lines.push_back(line);
        }

        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (ParseCodecaveHeader(lines[i], out))
            {
                for (size_t j = i + 1; j < lines.size(); ++j)
                {
                    if (lines[j].find("%end%") != std::string::npos)
                        break;

                    out.body += lines[j];
                    out.body += "\n";
                }

                return out.hook_address != 0 && !out.body.empty();
            }
        }

        out.mode = AssemblyHookBlock::Mode::Standalone;

        for (const auto& trimmed_line : lines)
        {
            if (trimmed_line.find("%end%") != std::string::npos)
                break;

            out.body += trimmed_line;
            out.body += "\n";
        }

        return !out.body.empty();
    }

    bool AssembleX86Text(
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
            lextprint(
                "AsmTK: code.init failed: %s\n",
                asmjit::DebugUtils::error_as_string(err)
            );

            return false;
        }

        asmjit::x86::Assembler assembler(&code);
        asmtk::AsmParser parser(&assembler);

        err = parser.parse(asm_text.c_str());

        if (err != asmjit::Error::kOk)
        {
            lextprint(
                "AsmTK parse failed: %s\n",
                asmjit::DebugUtils::error_as_string(err)
            );

            lextprint(
                "AsmTK input:\n%s\n",
                asm_text.c_str()
            );

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

    bool DecodeStolenSize(uintptr_t address, size_t min_size, size_t& out_stolen_size)
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

    bool RelocateRelativeInstructionX86(
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
                    lextprint(
                        "Relocate: rel8 out of range at 0x%llX\n",
                        static_cast<unsigned long long>(old_ip)
                    );
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

    bool IsAddressInRange(uintptr_t address, uintptr_t start, uintptr_t end)
    {
        return address >= start && address < end;
    }

    bool GetRelativeImmediateInfo(
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

    RelocKind ClassifyRelativeInstruction(
        const ZydisDecodedInstruction& instr,
        const std::vector<uint8_t>& bytes,
        uint8_t imm_size
    )
    {
        if (bytes.empty())
            return RelocKind::None;

        uint8_t op0 = bytes[0];

        if (op0 >= 0x70 && op0 <= 0x7F && imm_size == 1)
            return RelocKind::JccShort;

        if (op0 == 0xEB && imm_size == 1)
            return RelocKind::JmpShort;

        if ((op0 == 0xE0 || op0 == 0xE1 || op0 == 0xE2) && imm_size == 1)
            return RelocKind::LoopShort;

        if (op0 == 0xE3 && imm_size == 1)
            return RelocKind::JecxzShort;

        if (imm_size == 4)
            return RelocKind::Rel32;

        return RelocKind::None;
    }

    size_t GetRelocatedInstructionSize(const DecodedRelocInstruction& ri)
    {
        switch (ri.kind)
        {
        case RelocKind::JmpShort:
            return 5;

        case RelocKind::JccShort:
            return 7;

        case RelocKind::LoopShort:
        case RelocKind::JecxzShort:
            return 12;

        default:
            return ri.instr.length;
        }
    }

    bool BuildDecodedRelocInstructions(
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
                lextprint(
                    "Relocate: failed to decode instruction at 0x%llX\n",
                    static_cast<unsigned long long>(ri.old_ip)
                );

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

    bool AssignNewOffsets(std::vector<DecodedRelocInstruction>& instructions)
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

    bool ResolveRelocatedTarget(
        const std::vector<DecodedRelocInstruction>& instructions,
        uintptr_t original_start,
        uintptr_t original_end,
        uintptr_t relocated_base,
        uintptr_t target,
        uintptr_t& out_target
    )
    {
        if (target == original_end)
        {
            out_target = original_end;
            return true;
        }

        if (!IsAddressInRange(target, original_start, original_end))
        {
            out_target = target;
            return true;
        }

        for (const auto& ri : instructions)
        {
            if (target == ri.old_ip)
            {
                out_target = relocated_base + ri.new_offset;
                return true;
            }
        }

        lextprint(
            "Relocate: target 0x%llX lands inside stolen block but not on instruction boundary\n",
            static_cast<unsigned long long>(target)
        );

        return false;
    }

    bool WriteRel32(
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

    void EmitJmpRel32(
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

    bool EmitRelocatedInstruction(
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
            std::vector<uint8_t> bytes = ri.original_bytes;
            if (!RelocateRelativeInstructionX86(ri.instr, ri.operands, ri.old_ip, new_ip, bytes))
                return false;
            out.insert(out.end(), bytes.begin(), bytes.end());
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
                lextprint(
                    "Relocate: rel32 out of range at 0x%llX\n",
                    static_cast<unsigned long long>(ri.old_ip)
                );

                return false;
            }

            out.insert(out.end(), bytes.begin(), bytes.end());
            return true;
        }

        case RelocKind::JmpShort:
            EmitJmpRel32(out, new_ip, relocated_target);
            return true;

        case RelocKind::JccShort:
        {
            uint8_t original_jcc = ri.opcode0;
            uint8_t opposite_jcc = original_jcc ^ 1;

            out.push_back(opposite_jcc);
            out.push_back(0x05);

            EmitJmpRel32(out, new_ip + 2, relocated_target);
            return true;
        }

        case RelocKind::LoopShort:
        case RelocKind::JecxzShort:
        {
            uint8_t op = ri.opcode0;

            out.push_back(op);
            out.push_back(0x05);

            EmitJmpRel32(out, new_ip + 2, new_ip + 12);
            EmitJmpRel32(out, new_ip + 7, relocated_target);

            return true;
        }
        }

        return false;
    }

    bool RelocateOriginalCodeX86(
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
                lextprint(
                    "RelocateOriginalCodeX86: failed at 0x%llX\n",
                    static_cast<unsigned long long>(ri.old_ip)
                );

                return false;
            }

            size_t emitted = out_bytes.size() - before;

            if (emitted != ri.emitted_size)
            {
                lextprint(
                    "RelocateOriginalCodeX86: size mismatch at 0x%llX. expected=%zu got=%zu\n",
                    static_cast<unsigned long long>(ri.old_ip),
                    ri.emitted_size,
                    emitted
                );

                return false;
            }
        }

        return true;
    }

    bool WriteRelativeJump(uintptr_t src, uintptr_t dst, size_t patch_size)
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

    std::string PreprocessGameAddressTokens(const std::string& input)
    {
        std::string output;
        output.reserve(input.size());

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

            uintptr_t final_address = LuaExtended::ResolveGameAddress(raw_address);

            output += HexAddress(final_address);

            last_pos = match.position() + match.length();
        }

        output.append(input, last_pos, std::string::npos);

        return output;
    }

    std::string PreprocessAllocationTokens(const std::string& input)
    {
        std::string output;
        output.reserve(input.size());

        static const std::regex allocation_regex(
            R"(\(allocation\)([A-Za-z_][A-Za-z0-9_]*)->([A-Za-z_][A-Za-z0-9_]*))"
        );

        std::sregex_iterator it(input.begin(), input.end(), allocation_regex);
        std::sregex_iterator end;

        size_t last_pos = 0;

        for (; it != end; ++it)
        {
            const std::smatch& match = *it;

            output.append(input, last_pos, match.position() - last_pos);

            std::string block_name = match[1].str();
            std::string var_name = match[2].str();

            uintptr_t final_address = LuaExtended::ResolveNamedAllocationAddress(
                block_name,
                var_name
            );

            if (final_address == 0)
            {
                lextprint(
                    "CompileAssembly: unresolved allocation token %s->%s\n",
                    block_name.c_str(),
                    var_name.c_str()
                );

                output += match.str();
            }
            else
            {
                output += HexAddress(final_address);
            }

            last_pos = match.position() + match.length();
        }

        output.append(input, last_pos, std::string::npos);
        return output;
    }

    std::string PreprocessCommon(const std::string& input, const AssemblyHookBlock& block)
    {
        std::string out = input;

        ReplaceAll(out, "%returnaddress%", HexAddress(block.return_address));
        out = PreprocessGameAddressTokens(out);
        out = PreprocessAllocationTokens(out);
        return out;
    }

    struct ExecutablePage
    {
        uint8_t* base{};
        size_t size{};
        size_t used{};
    };

    struct ExecutableAllocation
    {
        uint8_t* ptr{};
        size_t page_index{};
        size_t previous_used{};
        size_t end_used{};
        size_t reserved_size{};
    };

    std::vector<ExecutablePage> g_ExecutablePages;

    size_t AlignUp(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    size_t GetPageSize()
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return si.dwPageSize;
    }

    ExecutableAllocation AllocateExecutableFromPool(size_t used_size, size_t alignment = 16)
    {
        ExecutableAllocation allocation{};

        if (used_size == 0)
            return allocation;

        used_size = AlignUp(used_size, alignment);

        for (size_t i = 0; i < g_ExecutablePages.size(); ++i)
        {
            auto& page = g_ExecutablePages[i];
            size_t aligned_used = AlignUp(page.used, alignment);

            if (aligned_used + used_size <= page.size)
            {
                allocation.ptr = page.base + aligned_used;
                allocation.page_index = i;
                allocation.previous_used = page.used;
                allocation.end_used = aligned_used + used_size;
                allocation.reserved_size = page.size;
                page.used = allocation.end_used;
                return allocation;
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
            return allocation;

        ExecutablePage page{};
        page.base = static_cast<uint8_t*>(mem);
        page.size = alloc_size;
        page.used = used_size;

        g_ExecutablePages.push_back(page);

        allocation.ptr = page.base;
        allocation.page_index = g_ExecutablePages.size() - 1;
        allocation.previous_used = 0;
        allocation.end_used = used_size;
        allocation.reserved_size = alloc_size;
        return allocation;
    }

    void RollbackExecutableAllocation(const ExecutableAllocation& allocation)
    {
        if (!allocation.ptr || allocation.page_index >= g_ExecutablePages.size())
            return;

        auto& page = g_ExecutablePages[allocation.page_index];
        if (page.used == allocation.end_used)
            page.used = allocation.previous_used;
    }

    bool CompileStandaloneAssemblyBlock(AssemblyHookBlock& block)
    {
        constexpr uintptr_t TEMP_BASE = 0x50000000;

        std::string body = PreprocessGameAddressTokens(block.body);
        body = PreprocessAllocationTokens(body);
        std::vector<uint8_t> temp_bytes;

        if (!AssembleX86Text(
            body,
            static_cast<uint32_t>(TEMP_BASE),
            temp_bytes
        ))
        {
            return false;
        }

        size_t estimated_size = temp_bytes.size();
        if (estimated_size == 0)
        {
            lextprint("CompileAssembly: estimated size was zero\n");
            return false;
        }

        ExecutableAllocation allocation = AllocateExecutableFromPool(estimated_size);
        if (!allocation.ptr)
        {
            lextprint("CompileAssembly: executable pool allocation failed\n");
            return false;
        }

        block.compiled_address = reinterpret_cast<uintptr_t>(allocation.ptr);

        std::vector<uint8_t> final_bytes;
        if (!AssembleX86Text(
            body,
            static_cast<uint32_t>(block.compiled_address),
            final_bytes
        ))
        {
            RollbackExecutableAllocation(allocation);
            return false;
        }

        if (final_bytes.empty())
        {
            RollbackExecutableAllocation(allocation);
            return false;
        }

        if (final_bytes.size() > estimated_size)
        {
            RollbackExecutableAllocation(allocation);

            estimated_size = final_bytes.size();
            allocation = AllocateExecutableFromPool(estimated_size);
            if (!allocation.ptr)
            {
                lextprint("CompileAssembly: executable pool allocation failed\n");
                return false;
            }

            block.compiled_address = reinterpret_cast<uintptr_t>(allocation.ptr);

            final_bytes.clear();
            if (!AssembleX86Text(
                body,
                static_cast<uint32_t>(block.compiled_address),
                final_bytes
            ))
            {
                RollbackExecutableAllocation(allocation);
                return false;
            }

            if (final_bytes.empty())
            {
                RollbackExecutableAllocation(allocation);
                return false;
            }
        }

        memcpy(allocation.ptr, final_bytes.data(), final_bytes.size());

        FlushInstructionCache(
            GetCurrentProcess(),
            allocation.ptr,
            final_bytes.size()
        );

        block.final_bytes = std::move(final_bytes);

        lextprint(
            "CompileAssembly: %s assembled at 0x%llX used=%zu alloc=%zu\n",
            block.name.c_str(),
            static_cast<unsigned long long>(block.compiled_address),
            block.final_bytes.size(),
            allocation.reserved_size
        );

        return true;
    }

    bool CompileCodecaveAssemblyBlock(AssemblyHookBlock& block)
    {
        constexpr size_t JMP_SIZE = 5;

        if (!DecodeStolenSize(block.hook_address, JMP_SIZE, block.stolen_size))
        {
            lextprint(
                "CompileAssembly: failed to decode stolen size at 0x%llX\n",
                static_cast<unsigned long long>(block.hook_address)
            );

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
            lextprint("CompileAssembly: estimated size was zero\n");
            return false;
        }

        ExecutableAllocation allocation = AllocateExecutableFromPool(estimated_size);
        if (!allocation.ptr)
        {
            lextprint("CompileAssembly: codecave pool allocation failed\n");
            return false;
        }

        block.codecave_address = reinterpret_cast<uintptr_t>(allocation.ptr);
        block.compiled_address = block.codecave_address;

        std::vector<uint8_t> pre_bytes;

        if (!AssembleX86Text(
            pre_text,
            static_cast<uint32_t>(block.codecave_address),
            pre_bytes
        ))
        {
            RollbackExecutableAllocation(allocation);
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
                RollbackExecutableAllocation(allocation);
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
                RollbackExecutableAllocation(allocation);
                return false;
            }
        }

        block.final_bytes.clear();
        block.final_bytes.insert(block.final_bytes.end(), pre_bytes.begin(), pre_bytes.end());
        block.final_bytes.insert(block.final_bytes.end(), block.relocated_original.begin(), block.relocated_original.end());
        block.final_bytes.insert(block.final_bytes.end(), post_bytes.begin(), post_bytes.end());

        if (block.final_bytes.empty())
        {
            RollbackExecutableAllocation(allocation);
            return false;
        }

        if (block.final_bytes.size() > estimated_size)
        {
            RollbackExecutableAllocation(allocation);

            estimated_size = block.final_bytes.size();
            allocation = AllocateExecutableFromPool(estimated_size);
            if (!allocation.ptr)
            {
                lextprint("CompileAssembly: codecave pool allocation failed\n");
                return false;
            }

            block.codecave_address = reinterpret_cast<uintptr_t>(allocation.ptr);
            block.compiled_address = block.codecave_address;

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
                RollbackExecutableAllocation(allocation);
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
                    RollbackExecutableAllocation(allocation);
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
                    RollbackExecutableAllocation(allocation);
                    return false;
                }
            }

            block.final_bytes.insert(block.final_bytes.end(), pre_bytes.begin(), pre_bytes.end());
            block.final_bytes.insert(block.final_bytes.end(), block.relocated_original.begin(), block.relocated_original.end());
            block.final_bytes.insert(block.final_bytes.end(), post_bytes.begin(), post_bytes.end());
        }

        memcpy(allocation.ptr, block.final_bytes.data(), block.final_bytes.size());

        FlushInstructionCache(
            GetCurrentProcess(),
            allocation.ptr,
            block.final_bytes.size()
        );

        if (!WriteRelativeJump(
            block.hook_address,
            block.codecave_address,
            block.stolen_size
        ))
        {
            lextprint("CompileAssembly: failed to write JMP\n");
            RollbackExecutableAllocation(allocation);
            return false;
        }

        g_InstalledAssemblyHooks.push_back({
            block.name,
            block.hook_address,
            block.codecave_address,
            block.stolen_size,
            block.final_bytes.size()
        });

        lextprint(
            "CompileAssembly: %s hooked 0x%llX -> 0x%llX, stolen=%zu used=%zu alloc=%zu\n",
            block.name.c_str(),
            static_cast<unsigned long long>(block.hook_address),
            static_cast<unsigned long long>(block.codecave_address),
            block.stolen_size,
            block.final_bytes.size(),
            allocation.reserved_size
        );

        return true;
    }

    bool CompileAssemblyBlock(AssemblyHookBlock& block, uintptr_t& out_address)
    {
        if (block.mode == AssemblyHookBlock::Mode::CodecaveJmp)
        {
            if (!CompileCodecaveAssemblyBlock(block))
                return false;
        }
        else
        {
            if (!CompileStandaloneAssemblyBlock(block))
                return false;
        }

        out_address = block.compiled_address;
        return out_address != 0;
    }
}

namespace LuaExtended::Assembly
{
    uintptr_t CompileAssemblyScript(const char* name, const char* script)
    {
        AssemblyHookBlock block;

        if (!ParseAssemblyScript(name, script, block))
        {
            lextprint("CompileAssembly: parse failed\n");
            return 0;
        }

        uintptr_t compiled_address = 0;
        if (!CompileAssemblyBlock(block, compiled_address))
            return 0;

        return compiled_address;
    }
}
