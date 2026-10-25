#include "NativeGpuHooks.h"
#include <cstring>
#include <iterator>
#include <limits>

namespace lan3d {
namespace {
bool CopyImage(HMODULE module, size_t offset, void* destination, size_t size) noexcept {
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (!base || offset > std::numeric_limits<uintptr_t>::max() - base ||
        size > std::numeric_limits<uintptr_t>::max() - base - offset) return false;
    auto cursor = base + offset;
    const auto end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) ||
            memory.AllocationBase != module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        const auto next = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    __try { std::memcpy(destination, reinterpret_cast<void*>(base + offset), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(HMODULE module, size_t offset, T& value) noexcept {
    return CopyImage(module, offset, &value, sizeof(value));
}
bool Range(uint32_t imageSize, uint32_t offset, size_t bytes) noexcept {
    return offset < imageSize && bytes <= imageSize - offset;
}
struct CodeViewRecord { uint32_t signature; GUID guid; uint32_t age; };
static_assert(sizeof(CodeViewRecord) == 24);
}

bool ReadNativeGpuModule(HMODULE module, NativeGpuModule& info) {
    info = {};
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!Read(module, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024 ||
        !Read(module, static_cast<size_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage < 4096 || nt.OptionalHeader.SizeOfImage > 1024u * 1024u * 1024u ||
        !Range(nt.OptionalHeader.SizeOfImage, static_cast<uint32_t>(dos.e_lfanew), sizeof(nt))) return false;

    NativeGpuModule result{};
    wchar_t loadedPath[std::size(result.path)]{};
    const auto length = GetModuleFileNameW(module, loadedPath, static_cast<DWORD>(std::size(loadedPath)));
    if (!length || length >= std::size(loadedPath)) return false;
    const auto fullLength = GetFullPathNameW(loadedPath, static_cast<DWORD>(std::size(result.path)), result.path, nullptr);
    if (!fullLength || fullLength >= std::size(result.path)) return false;
    result.timestamp = nt.FileHeader.TimeDateStamp;
    result.imageSize = nt.OptionalHeader.SizeOfImage;

    // Read only the mapped PE's metadata. In particular, never load a module or
    // invoke an export while checking an address obtained in another process.
    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
        const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        if (directory.Size) {
            if (!directory.VirtualAddress || directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) ||
                directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) > 256 ||
                !Range(result.imageSize, directory.VirtualAddress, directory.Size)) return false;
            bool hasCodeView = false;
            for (size_t offset = 0; offset < directory.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
                IMAGE_DEBUG_DIRECTORY entry{};
                if (!Read(module, directory.VirtualAddress + offset, entry)) return false;
                if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
                CodeViewRecord record{};
                if (!entry.AddressOfRawData || entry.SizeOfData < sizeof(record) ||
                    !Range(result.imageSize, entry.AddressOfRawData, entry.SizeOfData) ||
                    !Read(module, entry.AddressOfRawData, record)) return false;
                if (record.signature != 0x53445352) continue; // RSDS
                if (hasCodeView && (!IsEqualGUID(result.pdbGuid, record.guid) || result.pdbAge != record.age)) return false;
                result.pdbGuid = record.guid;
                result.pdbAge = record.age;
                hasCodeView = true;
            }
        }
    }
    info = result;
    return true;
}
}
