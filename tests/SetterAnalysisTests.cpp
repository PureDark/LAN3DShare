#include "capture/SetterAnalysis.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using lan3d::capture::modern::AnalyzeSetter;
using lan3d::capture::modern::ReadPlan;
namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Pimpl { uint64_t gap[7]{}; void* input{}; uint32_t format{}; };
struct Object { void* input{}; uint32_t format{}; uint64_t padding[9]{}; Pimpl* impl{}; };
__declspec(noinline) void Direct(Object* self, void* input) { self->input = input; }
__declspec(noinline) void Indirect(Object* self, void* input) { self->impl->input = input; }
__declspec(noinline) void Format(Object* self, void*, int, int, uint32_t format) { self->impl->format = format; }
__declspec(noinline) void Transformed(Object* self, void* input) { self->input = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(input) ^ 3); }

// These are instruction fixtures only. They live in executable PE sections so
// the production image-boundary checks remain active; none is ever executed.
// Named source functions above separately exercise actual MSVC output.
#pragma section(".safix", read, execute)
#define FIXTURE __declspec(allocate(".safix")) const unsigned char
FIXTURE duplicate[] = {0x48,0x89,0x51,0x08, 0x48,0x89,0x51,0x18, 0xc3};
FIXTURE callPreserved[] = {
    0x57, 0x53, 0x48,0x83,0xec,0x28, // push rdi/rbx; sub rsp,40
    0x48,0x8b,0xf9, 0x48,0x8b,0xda, // rdi=this; rbx=input
    0xff,0x50,0x08, // opaque COM AddRef call; not executed
    0x48,0x89,0x5f,0x08,
    0x48,0x83,0xc4,0x28, 0x5b,0x5f,0xc3};
FIXTURE callClobbered[] = {0xff,0x50,0x08, 0x48,0x89,0x51,0x08, 0xc3};
FIXTURE vtordisp[] = {
    0x48,0x63,0x41,0xfc, // movsxd rax,[rcx-4]
    0x48,0x2b,0xc8, // sub rcx,rax
    0xeb,0x02, 0xcc,0xcc, // jump over padding
    0x48,0x8b,0x49,0xf8, // rcx=[rcx-8] (private implementation inferred from code)
    0x48,0x89,0x51,0x20, 0xc3};
FIXTURE conditional[] = {
    0x48,0x85,0xd2, 0x74,0x05, // test input; je clear
    0x48,0x89,0x51,0x08, 0xc3,
    0x48,0xc7,0x41,0x08,0,0,0,0, 0xc3};
FIXTURE branchFields[] = {
    0x45,0x85,0xc0, 0x74,0x05,
    0x48,0x89,0x51,0x08, 0xc3,
    0x48,0x89,0x51,0x18, 0xc3};
FIXTURE stackSaved[] = {
    0x48,0x89,0x4c,0x24,0x08, // save this in home slot
    0x48,0x89,0x54,0x24,0x10, // save input in home slot
    0x48,0x83,0xec,0x28,
    0xff,0x50,0x08,
    0x48,0x8b,0x4c,0x24,0x30,
    0x48,0x8b,0x54,0x24,0x38,
    0x48,0x89,0x51,0x08,
    0x48,0x83,0xc4,0x28,0xc3};
FIXTURE unknownWrite[] = {0x48,0xc1,0xe2,0x01, 0x48,0x89,0x51,0x08,0xc3}; // shl rdx,1
FIXTURE truncated[] = {0x8b,0xd2, 0x48,0x89,0x51,0x08,0xc3}; // mov edx,edx loses pointer upper bits
FIXTURE loop[] = {0xeb,0xfe};
FIXTURE externalJump[] = {0xff,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
FIXTURE excessiveOffset[] = {0x48,0x89,0x91,0xff,0xff,0xff,0x7f,0xc3};
FIXTURE fakeCounter[] = {0x48,0x89,0x51,0x08, 0xf0,0xff,0x05,0,0,0,0, 0xc3};
FIXTURE zeroExtendedImmediate[] = {0xb8,0xff,0xff,0xff,0xff, 0x48,0x03,0xc8, 0x48,0x89,0x11,0xc3};
FIXTURE zeroExtendedArithmetic[] = {0x33,0xc0, 0x83,0xe8,0x01, 0x48,0x03,0xc8, 0x48,0x89,0x11,0xc3};
FIXTURE positiveImmediate[] = {0xb8,0x08,0,0,0, 0x48,0x03,0xc8, 0x48,0x89,0x11,0xc3};
FIXTURE overwrittenField[] = {0x48,0x89,0x51,0x08, 0x48,0x89,0x41,0x08,0xc3};
FIXTURE partialClear[] = {0x48,0x89,0x51,0x08, 0xc7,0x41,0x0c,0,0,0,0,0xc3};
FIXTURE helperFieldAddress[] = {
    0x48,0x89,0x51,0x08, // store input
    0x48,0x8d,0x49,0x08, // pass &input to an unknown helper, which may overwrite it
    0xff,0x50,0x08,0xc3};
FIXTURE helperComValue[] = {
    0x48,0x89,0x51,0x08,
    0x48,0x8b,0x49,0x08, // pass input itself, as in COM AddRef/Release
    0xff,0x50,0x08,0xc3};
#undef FIXTURE

template<class Function> void* Code(Function f) { return reinterpret_cast<void*>(f); }
template<size_t N> void* Code(const unsigned char (&bytes)[N]) { return const_cast<unsigned char*>(bytes); }
ReadPlan Plan(void* code, unsigned argument = 1, unsigned bytes = 8) {
    auto p = AnalyzeSetter(code, argument, bytes);
    if (!p.valid) throw std::runtime_error(p.diagnostic);
    return p;
}
void CheckRead(const ReadPlan& p, void* self, uint64_t expected, const char* message) {
    uint64_t value{}; Check(p.Read(self, value) && value == expected, message);
}
void TestCompiled() {
    Pimpl impl{}; Object object{}; object.impl = &impl;
    object.input = reinterpret_cast<void*>(uintptr_t(0x123400)); impl.input = reinterpret_cast<void*>(uintptr_t(0x567800)); impl.format = 87;
    CheckRead(Plan(Code(&Direct)), &object, 0x123400, "compiled direct setter");
    CheckRead(Plan(Code(&Indirect)), &object, 0x567800, "compiled pimpl setter");
    CheckRead(Plan(Code(&Format), 4, 4), &object, 87, "compiled fifth argument format");
    Check(!AnalyzeSetter(Code(&Transformed)).valid, "transformed argument must not be mistaken for input");
    Check(object.input == reinterpret_cast<void*>(uintptr_t(0x123400)) && impl.input == reinterpret_cast<void*>(uintptr_t(0x567800)) && impl.format == 87, "analysis changed the object");
}
void TestInstructions() {
    std::array<uint64_t, 8> object{}; object[1] = object[3] = 0x1122334455667788ull;
    const auto p = Plan(Code(duplicate)); CheckRead(p, object.data(), object[1], "duplicate input aliases");
    object[3] = 0; uint64_t value{};
    Check(!p.Read(object.data(), value), "null candidate must not be silently ignored");
    object[1] = 0; CheckRead(p, object.data(), 0, "consistent null fields");
    object[1] = object[3] = 0x889900;
    CheckRead(Plan(Code(callPreserved)), object.data(), object[1], "nonvolatile input across COM call");
    Check(!AnalyzeSetter(Code(callClobbered)).valid, "volatile provenance survived unknown call");
    CheckRead(Plan(Code(stackSaved)), object.data(), object[1], "stack saved arguments");
    CheckRead(Plan(Code(conditional)), object.data(), object[1], "conditional clear branch");
    CheckRead(Plan(Code(fakeCounter)), object.data(), object[1], "unrelated atomic counter rejected");
    CheckRead(Plan(Code(positiveImmediate)), object.data(), object[1], "small zero-extended address offset");
    CheckRead(Plan(Code(helperComValue)), object.data(), object[1], "COM value confused with address of input field");
    const auto branches = Plan(Code(branchFields)); object[3] = 0x77;
    Check(!branches.Read(object.data(), value), "ambiguous branch fields accepted");
    for (auto code : {Code(unknownWrite), Code(truncated), Code(loop), Code(externalJump), Code(excessiveOffset), Code(zeroExtendedImmediate), Code(zeroExtendedArithmetic), Code(overwrittenField), Code(partialClear), Code(helperFieldAddress)})
        Check(!AnalyzeSetter(code).valid, "unsupported setter was not rejected");

    alignas(8) std::array<unsigned char, 64> objectBytes{};
    alignas(8) std::array<uint64_t, 8> impl{}; impl[4] = 0xabcdef;
    auto implementation = impl.data(); std::memcpy(objectBytes.data() + 8, &implementation, sizeof(implementation));
    const int32_t adjustment = 16; std::memcpy(objectBytes.data() + 28, &adjustment, sizeof(adjustment));
    auto adjusted = Plan(Code(vtordisp)); CheckRead(adjusted, objectBytes.data() + 32, 0xabcdef, "vtordisp and pimpl chain");
    auto impossible = reinterpret_cast<void*>(uintptr_t(1)); std::memcpy(objectBytes.data() + 8, &impossible, sizeof(impossible));
    Check(!adjusted.Read(objectBytes.data() + 32, value), "invalid pimpl accepted");
    Check(!adjusted.Read(nullptr, value), "null this accepted");
    void* inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Check(inaccessible != nullptr, "allocate inaccessible test page");
    Check(!p.Read(inaccessible, value), "inaccessible object accepted"); VirtualFree(inaccessible, 0, MEM_RELEASE);
    const unsigned char heapCode[] = {0x48,0x89,0x11,0xc3};
    Check(!AnalyzeSetter(const_cast<unsigned char*>(heapCode)).valid, "non-image code accepted");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        // Optional static inspection of a vendor PE. DONT_RESOLVE_DLL_REFERENCES
        // maps code without invoking its entry point or loading dependencies.
        if (argc >= 4 && std::wstring(argv[1]) == L"--inspect") {
            HMODULE image = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
            Check(image != nullptr, "map vendor image for static analysis");
            const auto rva = std::stoull(argv[3], nullptr, 0);
            const auto argument = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 1;
            const auto bytes = argc > 5 ? static_cast<unsigned>(std::stoul(argv[5])) : 8;
            auto p = AnalyzeSetter(reinterpret_cast<unsigned char*>(image) + rva, argument, bytes);
            std::cout << p.diagnostic << '\n'; FreeLibrary(image); return p.valid ? 0 : 1;
        }
        TestCompiled(); TestInstructions();
        std::cout << "Setter analysis: compiled field paths, Win64 provenance, ambiguity and invalid memory checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
