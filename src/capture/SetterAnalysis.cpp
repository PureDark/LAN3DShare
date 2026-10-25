#include "SetterAnalysis.h"
#include "../../third_party/minhook/src/hde/hde64.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace lan3d::capture::modern {
namespace {
constexpr int64_t MaxOffset = 0x10000;
constexpr unsigned MaxSteps = 4096, MaxStates = 512, MaxDepth = 16, MaxReads = 4;
bool ReadMemory(uintptr_t address, void* output, size_t bytes) noexcept {
    if (!address || bytes > SIZE_MAX - address) return false;
    for (auto p = address; p < address + bytes;) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        const auto end = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (end <= p) return false;
        p = end;
    }
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), output, bytes, &read) && read == bytes;
}
enum class Kind { Unknown, Constant, This, Stack, Argument, Add, Subtract, Load };
struct Expr;
using E = std::shared_ptr<const Expr>;
struct Expr {
    Kind kind{};
    int64_t constant{};
    E a, b;
    unsigned bytes{}, depth{}, reads{};
    bool sign{}, object{}, stack{}, argument{};
    std::string key;
};
E Atom(Kind kind, int64_t n = 0) {
    auto e = std::make_shared<Expr>(); e->kind = kind; e->constant = n; e->depth = 1;
    e->object = kind == Kind::This; e->stack = kind == Kind::Stack; e->argument = kind == Kind::Argument;
    e->key = std::to_string(static_cast<int>(kind)) + ":" + std::to_string(n); return e;
}
E Unknown() { static const auto e = Atom(Kind::Unknown); return e; }
bool Known(const E& e) { return e && e->kind != Kind::Unknown; }
E Binary(Kind kind, E a, E b) {
    if (!Known(a) || !Known(b)) return Unknown();
    if (b->kind == Kind::Constant && !b->constant) return a;
    if (a->kind == Kind::Constant && b->kind == Kind::Constant) {
        const auto n = kind == Kind::Add ? a->constant + b->constant : a->constant - b->constant;
        return n >= -MaxOffset && n <= MaxOffset ? Atom(Kind::Constant, n) : Unknown();
    }
    // Normalize offsets so stack aliases and repeated field stores deduplicate.
    if (kind == Kind::Add && a->kind == Kind::Add && a->b->kind == Kind::Constant && b->kind == Kind::Constant)
        return Binary(kind, a->a, Binary(Kind::Add, a->b, b));
    if (kind == Kind::Subtract && b->kind == Kind::Constant) return Binary(Kind::Add, a, Atom(Kind::Constant, -b->constant));
    if (kind == Kind::Add && a->kind == Kind::Constant) std::swap(a, b);
    auto e = std::make_shared<Expr>(); e->kind = kind; e->a = std::move(a); e->b = std::move(b);
    e->depth = 1 + std::max(e->a->depth, e->b->depth); e->reads = e->a->reads + e->b->reads;
    e->object = e->a->object || e->b->object; e->stack = e->a->stack || e->b->stack;
    e->argument = e->a->argument || e->b->argument;
    if (e->depth > MaxDepth || e->reads > MaxReads) return Unknown();
    e->key = (kind == Kind::Add ? "+(" : "-(") + e->a->key + "," + e->b->key + ")"; return e;
}
E Offset(E a, int64_t n) { return n >= -MaxOffset && n <= MaxOffset ? Binary(Kind::Add, std::move(a), Atom(Kind::Constant, n)) : Unknown(); }
bool StackOffset(const E& e, int64_t& offset) {
    if (!e) return false;
    if (e->kind == Kind::Stack) { offset = 0; return true; }
    if (e->kind == Kind::Add && e->a->kind == Kind::Stack && e->b->kind == Kind::Constant) { offset = e->b->constant; return true; }
    return false;
}
bool ObjectAddress(const E& e) { return Known(e) && e->object && !e->stack && !e->argument; }
E Load(E address, unsigned bytes, bool sign = false) {
    if (!ObjectAddress(address) || address->depth >= MaxDepth || address->reads >= MaxReads) return Unknown();
    auto e = std::make_shared<Expr>(); e->kind = Kind::Load; e->a = std::move(address); e->bytes = bytes; e->sign = sign;
    e->depth = e->a->depth + 1; e->reads = e->a->reads + 1; e->object = true;
    e->key = "L" + std::to_string(bytes) + (sign ? "s(" : "u(") + e->a->key + ")"; return e;
}
bool Evaluate(const E& e, uintptr_t self, uint64_t& value) noexcept {
    if (!e) return false;
    uint64_t a{}, b{};
    switch (e->kind) {
    case Kind::This: value = self; return self != 0;
    case Kind::Constant: value = static_cast<uint64_t>(e->constant); return true;
    case Kind::Load:
        if (!Evaluate(e->a, self, a)) return false;
        value = 0;
        if (!ReadMemory(a, &value, e->bytes)) return false;
        if (e->sign && e->bytes == 4) value = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
        return true;
    case Kind::Add: case Kind::Subtract:
        if (!Evaluate(e->a, self, a) || !Evaluate(e->b, self, b)) return false;
        // Every nonconstant adjustment is a small signed vtordisp, not an
        // arbitrary pointer used to turn an object read into a memory scan.
        if (static_cast<int64_t>(b) < -MaxOffset || static_cast<int64_t>(b) > MaxOffset) return false;
        value = e->kind == Kind::Add ? a + b : a - b;
        return value >= 0x10000 && value < 0x0000800000000000ull;
    default: return false;
    }
}
struct StackValue { E expression; unsigned bytes{}; };
struct State {
    uintptr_t pc{};
    std::array<E, 16> reg;
    std::map<int64_t, StackValue> stack;
};
struct FieldWrite { E address, value; unsigned bytes{}; };
std::pair<E, int64_t> FieldBase(const E& address) {
    if (address->kind == Kind::Add && address->b->kind == Kind::Constant) return {address->a, address->b->constant};
    return {address, 0};
}
struct Analyzer {
    uintptr_t allocation{}, entry{};
    unsigned bytes{}, argument{}, steps{};
    bool returned{};
    std::string error;
    std::map<std::string, E> candidates;
    std::vector<FieldWrite> writes;
    std::set<std::string> callArguments;
    std::set<std::string> visited;
    std::deque<State> pending;
    bool Code(uintptr_t pc) const {
        MEMORY_BASIC_INFORMATION m{};
        return VirtualQuery(reinterpret_cast<void*>(pc), &m, sizeof(m)) && m.Type == MEM_IMAGE && m.State == MEM_COMMIT &&
            reinterpret_cast<uintptr_t>(m.AllocationBase) == allocation && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    }
    std::string StateKey(const State& s) {
        std::string key = std::to_string(s.pc);
        for (const auto& e : s.reg) key += ";" + e->key;
        for (const auto& [offset, value] : s.stack) key += ";" + std::to_string(offset) + ":" + std::to_string(value.bytes) + ":" + value.expression->key;
        return key;
    }
    void KillVolatile(State& s) {
        // Keep exact addresses handed to opaque callees. Passing a COM value
        // loaded from a field is different from giving the callee the field's
        // address, through which it could replace the stored input.
        for (const auto r : {1, 2, 8, 9}) if (ObjectAddress(s.reg[r])) callArguments.insert(s.reg[r]->key);
        for (const auto r : {0, 1, 2, 8, 9, 10, 11}) s.reg[r] = Unknown();
        // Callee-owned home space cannot retain caller symbolic values.
        int64_t sp{};
        if (StackOffset(s.reg[4], sp)) std::erase_if(s.stack, [&](const auto& v) { return v.first < sp + 32 && v.first + v.second.bytes > sp; });
    }
    E Read(const State& s, E address, unsigned width, bool sign = false) {
        int64_t offset{};
        if (StackOffset(address, offset)) {
            if (const auto it = s.stack.find(offset); it != s.stack.end()) {
                if (it->second.bytes == width || (it->second.bytes == 8 && width == 4 && bytes == 4 && it->second.expression->kind == Kind::Argument))
                    return it->second.expression;
                return Unknown();
            }
            return Unknown();
        }
        return Load(std::move(address), width, sign);
    }
    void Write(State& s, E address, E value, unsigned width) {
        int64_t offset{};
        if (StackOffset(address, offset)) {
            std::erase_if(s.stack, [&](const auto& v) { return v.first < offset + width && v.first + v.second.bytes > offset; });
            if (Known(value)) s.stack[offset] = {value, width};
            return;
        }
        if (ObjectAddress(address)) writes.push_back({address, value, width});
        if (width == bytes && value->kind == Kind::Argument && ObjectAddress(address)) candidates.emplace(address->key, std::move(address));
    }
    void SetReg(State& s, unsigned r, E value, unsigned width) {
        // Truncating an address or a 64-bit input must destroy its provenance.
        if (width != 8 && !(width == bytes && value->kind == Kind::Argument) && value->kind != Kind::Constant) value = Unknown();
        if (width == 4 && value->kind == Kind::Constant) {
            // Every 32-bit GPR write zero-extends, including mov eax,-1 and
            // arithmetic wrapping below zero. Never reinterpret it as this-1.
            const auto n = static_cast<uint32_t>(value->constant);
            value = n <= MaxOffset ? Atom(Kind::Constant, n) : Unknown();
        }
        s.reg[r] = width < 4 ? Unknown() : std::move(value);
    }
    unsigned RmReg(const hde64s& h, unsigned width) {
        if (width == 1 && !h.rex && h.modrm_rm >= 4) return h.modrm_rm - 4;
        return h.modrm_rm + (h.rex_b ? 8 : 0);
    }
    E Address(const State& s, const hde64s& h) {
        int64_t displacement = h.flags & F_DISP8 ? static_cast<int8_t>(h.disp.disp8) :
            h.flags & F_DISP32 ? static_cast<int32_t>(h.disp.disp32) : 0;
        if (h.modrm_mod == 3) return Unknown();
        if (!(h.flags & F_SIB)) {
            if (!h.modrm_mod && h.modrm_rm == 5) return Unknown(); // RIP-relative data is not an object path.
            return Offset(s.reg[h.modrm_rm + (h.rex_b ? 8 : 0)], displacement);
        }
        if (!h.modrm_mod && h.sib_base == 5) return Unknown();
        E address = s.reg[h.sib_base + (h.rex_b ? 8 : 0)];
        if (h.sib_index != 4 || h.rex_x) {
            if (h.sib_scale) return Unknown();
            address = Binary(Kind::Add, address, s.reg[h.sib_index + (h.rex_x ? 8 : 0)]);
        }
        return Offset(address, displacement);
    }
    E Operand(const State& s, const hde64s& h, unsigned width, bool sign = false) {
        if (h.modrm_mod != 3) return Read(s, Address(s, h), width, sign);
        const auto value = s.reg[RmReg(h, width)];
        if (width < 8 && value->kind != Kind::Constant && !(value->kind == Kind::Argument && bytes <= width)) return Unknown();
        return value;
    }
    void StoreOperand(State& s, const hde64s& h, E value, unsigned width) {
        if (h.modrm_mod == 3) SetReg(s, RmReg(h, width), std::move(value), width);
        else Write(s, Address(s, h), std::move(value), width);
    }
    bool ConsistentFields() {
        // A temporary argument store followed by some unrelated value does not
        // prove that this field still contains input. Null clearing is allowed
        // only when it covers the entire field, never a pointer's low half.
        for (const auto& [key, field] : candidates) {
            if (callArguments.contains(key)) {
                error = "argument field address escapes to an unknown call"; return false;
            }
            const auto [base, offset] = FieldBase(field);
            for (const auto& write : writes) {
                const auto [otherBase, otherOffset] = FieldBase(write.address);
                if (base->key != otherBase->key || otherOffset >= offset + bytes || otherOffset + write.bytes <= offset) continue;
                if (otherOffset != offset || write.bytes != bytes ||
                    (write.value->kind != Kind::Argument && !(write.value->kind == Kind::Constant && write.value->constant == 0))) {
                    error = "argument field also receives a different or partial value"; return false;
                }
            }
        }
        return true;
    }
    bool Jump(uintptr_t from, uintptr_t target) {
        if (target <= from || target - from > MaxOffset || !Code(target)) { error = "unsupported backward or out-of-module branch"; return false; }
        return true;
    }
    bool Run(State s) {
        for (;;) {
            if (++steps > MaxSteps || visited.size() > MaxStates || candidates.size() > 4) { error = "setter analysis limit exceeded"; return false; }
            if (!Code(s.pc)) { error = "setter left its executable image"; return false; }
            if (!visited.insert(StateKey(s)).second) return true;
            std::array<uint8_t, 16> code{};
            if (!ReadMemory(s.pc, code.data(), 15)) { error = "unreadable setter code"; return false; }
            hde64s h{}; hde64_disasm(code.data(), &h);
            const bool atomicCounter = h.p_lock && h.opcode == 0xff && h.modrm_reg <= 1 && h.modrm_mod != 3;
            if (!h.len || (h.flags & F_ERROR) || h.p_67 || h.p_seg || (h.p_lock && !atomicCounter)) { error = "unsupported setter instruction encoding"; return false; }
            const auto next = s.pc + h.len;
            const unsigned width = h.rex_w ? 8 : h.p_66 ? 2 : 4;
            const unsigned r = h.modrm_reg + (h.rex_r ? 8 : 0);
            const auto op = h.opcode;
            if (op == 0xc3 || op == 0xc2) { returned = true; return true; }
            if (op == 0xe9 || op == 0xeb) {
                const auto delta = op == 0xeb ? static_cast<int64_t>(static_cast<int8_t>(h.imm.imm8)) : static_cast<int64_t>(static_cast<int32_t>(h.imm.imm32));
                const auto target = next + delta;
                // Tail thunks may jump backwards into a shared implementation;
                // already visited instruction states bound such cycles.
                if (!Code(target) || (target > s.pc ? target - s.pc : s.pc - target) > MaxOffset) { error = "out-of-module setter thunk"; return false; }
                s.pc = target; continue;
            }
            if ((op >= 0x70 && op <= 0x7f) || (op == 0x0f && h.opcode2 >= 0x80 && h.opcode2 <= 0x8f)) {
                const auto delta = op == 0x0f ? static_cast<int64_t>(static_cast<int32_t>(h.imm.imm32)) : static_cast<int64_t>(static_cast<int8_t>(h.imm.imm8));
                const auto target = next + delta;
                if (!Jump(s.pc, target)) return false;
                auto branch = s; branch.pc = target; pending.push_back(std::move(branch)); s.pc = next; continue;
            }
            if (op == 0xe8 || (op == 0xff && h.modrm_reg == 2)) KillVolatile(s);
            else if (op >= 0x50 && op <= 0x57) {
                auto value = s.reg[(op - 0x50) + (h.rex_b ? 8 : 0)]; s.reg[4] = Offset(s.reg[4], -8); Write(s, s.reg[4], value, 8);
            } else if (op >= 0x58 && op <= 0x5f) {
                auto value = Read(s, s.reg[4], 8); s.reg[4] = Offset(s.reg[4], 8); s.reg[(op - 0x58) + (h.rex_b ? 8 : 0)] = value;
            } else if (op == 0x68 || op == 0x6a) {
                s.reg[4] = Offset(s.reg[4], -8); Write(s, s.reg[4], Unknown(), 8);
            } else if (op == 0xff && h.modrm_reg <= 1) StoreOperand(s, h, Unknown(), width);
            else if (op == 0x89 || op == 0x88) StoreOperand(s, h, s.reg[r], op == 0x88 ? 1 : width);
            else if (op == 0x8b || op == 0x8a) SetReg(s, r, Operand(s, h, op == 0x8a ? 1 : width), op == 0x8a ? 1 : width);
            else if (op == 0x8d) SetReg(s, r, Address(s, h), width);
            else if (op == 0x63 && h.rex_w) SetReg(s, r, Operand(s, h, 4, true), 8);
            else if (op >= 0xb8 && op <= 0xbf) {
                const auto n = h.rex_w ? static_cast<int64_t>(h.imm.imm64) : static_cast<int64_t>(static_cast<int32_t>(h.imm.imm32));
                SetReg(s, (op - 0xb8) + (h.rex_b ? 8 : 0), n >= -MaxOffset && n <= MaxOffset ? Atom(Kind::Constant, n) : Unknown(), width);
            } else if (op == 0xc7 || op == 0xc6) {
                if (h.modrm_reg) { error = "unsupported immediate store"; return false; }
                const auto n = op == 0xc6 ? static_cast<int64_t>(h.imm.imm8) : static_cast<int64_t>(static_cast<int32_t>(h.imm.imm32));
                StoreOperand(s, h, n >= -MaxOffset && n <= MaxOffset ? Atom(Kind::Constant, n) : Unknown(), op == 0xc6 ? 1 : width);
            } else if (op == 0x81 || op == 0x83 || op == 0x80) {
                if (h.modrm_reg != 7) {
                    const auto n = op == 0x81 ? static_cast<int64_t>(static_cast<int32_t>(h.imm.imm32)) : static_cast<int64_t>(static_cast<int8_t>(h.imm.imm8));
                    E value = Unknown();
                    if (h.modrm_reg == 0 || h.modrm_reg == 5) value = Offset(Operand(s, h, width), h.modrm_reg == 5 ? -n : n);
                    StoreOperand(s, h, value, op == 0x80 ? 1 : width);
                }
            } else if (op == 0x01 || op == 0x03 || op == 0x29 || op == 0x2b || op == 0x31 || op == 0x33 || op == 0x09 || op == 0x0b || op == 0x21 || op == 0x23) {
                const bool toReg = (op & 2) != 0;
                const auto left = toReg ? s.reg[r] : Operand(s, h, width), right = toReg ? Operand(s, h, width) : s.reg[r];
                E value = Unknown();
                if (op == 0x01 || op == 0x03 || op == 0x29 || op == 0x2b) value = Binary(op < 0x20 ? Kind::Add : Kind::Subtract, left, right);
                else if ((op == 0x31 || op == 0x33) && h.modrm_mod == 3 && r == RmReg(h, width)) value = Atom(Kind::Constant, 0);
                if (toReg) SetReg(s, r, value, width); else StoreOperand(s, h, value, width);
            } else if (op == 0x38 || op == 0x39 || op == 0x3a || op == 0x3b || op == 0x84 || op == 0x85 || op == 0x90 || op == 0x3c || op == 0x3d || op == 0xa8 || op == 0xa9) { /* compares */ }
            else if ((op == 0xf6 || op == 0xf7) && h.modrm_reg <= 1) { /* TEST */ }
            else if (op == 0x0f && h.opcode2 == 0x1f) { /* NOP */ }
            else if (op == 0x0f && (h.opcode2 == 0xb6 || h.opcode2 == 0xb7 || h.opcode2 == 0xbe || h.opcode2 == 0xbf || h.opcode2 == 0xaf || (h.opcode2 >= 0x40 && h.opcode2 <= 0x4f))) s.reg[r] = Unknown();
            else if (op == 0x0f && h.opcode2 >= 0x90 && h.opcode2 <= 0x9f) StoreOperand(s, h, Unknown(), 1);
            else if (op == 0x0f && (h.opcode2 == 0x10 || h.opcode2 == 0x28 || h.opcode2 == 0x6f || h.opcode2 == 0x57 || h.opcode2 == 0xef)) { /* XMM destination only */ }
            else if (op == 0x0f && (h.opcode2 == 0x11 || h.opcode2 == 0x29 || h.opcode2 == 0x7f)) {
                if (h.modrm_mod != 3) Write(s, Address(s, h), Unknown(), h.opcode2 == 0x11 && h.p_rep == 0xf3 ? 4 : h.opcode2 == 0x11 && h.p_rep == 0xf2 ? 8 : 16);
            } else {
                std::ostringstream out; out << "unsupported setter opcode 0x" << std::hex << unsigned(op);
                if (op == 0x0f) out << ":" << unsigned(h.opcode2);
                error = out.str(); return false;
            }
            if (!Known(s.reg[4])) { error = "lost setter stack provenance"; return false; }
            s.pc = next;
        }
    }
};
}
struct ReadPlanData { unsigned bytes{}; std::vector<E> fields; };
ReadPlan AnalyzeSetter(void* code, unsigned argument, unsigned bytes) {
    ReadPlan result;
    if (!code || (bytes != 4 && bytes != 8) || argument < 1 || argument > 4) { result.diagnostic = "invalid setter analysis arguments"; return result; }
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(code, &m, sizeof(m)) || m.Type != MEM_IMAGE) { result.diagnostic = "setter is not in a loaded PE image"; return result; }
    Analyzer a; a.allocation = reinterpret_cast<uintptr_t>(m.AllocationBase); a.entry = reinterpret_cast<uintptr_t>(code); a.argument = argument; a.bytes = bytes;
    State first; first.pc = a.entry; first.reg.fill(Unknown()); first.reg[1] = Atom(Kind::This); first.reg[4] = Atom(Kind::Stack);
    if (argument < 4) first.reg[argument == 1 ? 2 : argument == 2 ? 8 : 9] = Atom(Kind::Argument);
    else first.stack[0x28] = {Atom(Kind::Argument), bytes};
    a.pending.push_back(std::move(first));
    while (!a.pending.empty()) { auto state = std::move(a.pending.front()); a.pending.pop_front(); if (!a.Run(std::move(state))) break; }
    if (a.error.empty()) a.ConsistentFields();
    if (!a.error.empty() || !a.returned || a.candidates.empty()) { result.diagnostic = a.error.empty() ? "no proven argument field store" : a.error; return result; }
    auto data = std::make_shared<ReadPlanData>(); data->bytes = bytes;
    for (const auto& [key, field] : a.candidates) data->fields.push_back(field);
    result.data_ = std::move(data); result.valid = true;
    result.diagnostic = "read-only setter plan: " + std::to_string(result.data_->fields.size()) + " agreeing field(s)";
    return result;
}
bool ReadPlan::Read(void* interfaceThis, uint64_t& out) const noexcept {
    out = 0;
    if (!valid || !data_ || !interfaceThis || data_->fields.empty()) return false;
    bool first = true;
    for (const auto& field : data_->fields) {
        uint64_t address{}, value{};
        if (!Evaluate(field, reinterpret_cast<uintptr_t>(interfaceThis), address) || !ReadMemory(address, &value, data_->bytes)) { out = 0; return false; }
        if (!first && value != out) { out = 0; return false; }
        out = value; first = false;
    }
    return true;
}
}
