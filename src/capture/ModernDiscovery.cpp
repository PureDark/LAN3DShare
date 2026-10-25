#include "ModernDiscovery.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace lan3d::capture::modern {
namespace {
constexpr uint32_t MaxObjectOffset = 1024 * 1024;
constexpr std::string_view Dx11Name = ".?AVIDX11Weaver1@SR@@";
constexpr std::string_view Dx12Name = ".?AVIDX12Weaver1@SR@@";
constexpr std::string_view BaseName = ".?AVIWeaverBase1@SR@@";
constexpr std::string_view DestroyName = ".?AVIDestroyable@SR@@";
struct Locator { uint32_t signature, offset, cdOffset, type, hierarchy, self; };
struct Hierarchy { uint32_t signature, attributes, count, bases; };
struct Descriptor { uint32_t type, count; int32_t mdisp, pdisp, vdisp; uint32_t attributes, hierarchy; };
struct Section { uint32_t begin, size, flags; };

bool Readable(const void* address, size_t bytes) noexcept {
    auto current = reinterpret_cast<uintptr_t>(address);
    if (!current || bytes > std::numeric_limits<uintptr_t>::max() - current) return false;
    const auto end = current + bytes;
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        const auto next = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (next <= current) return false;
        current = next;
    }
    return true;
}
bool Copy(const void* source, void* target, size_t bytes) noexcept {
    if (!Readable(source, bytes)) return false;
    __try { std::memcpy(target, source, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(const void* source, T& value) noexcept { return Copy(source, &value, sizeof(T)); }

struct Image {
    const uint8_t* data{};
    uint32_t size{};
    std::vector<Section> sections;
    bool Open(HMODULE module) {
        data = reinterpret_cast<const uint8_t*>(module);
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
        if (!Read(data, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1024 * 1024 ||
            !Read(data + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96 ||
            nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return false;
        size = nt.OptionalHeader.SizeOfImage;
        if (size < 4096 || size > 512 * 1024 * 1024) return false;
        const size_t first = size_t(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        if (first > size || size_t(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > size - first) return false;
        for (uint32_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER s{};
            if (!Read(data + first + i * sizeof(s), s)) return false;
            const auto length = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
            if (s.VirtualAddress > size || length > size - s.VirtualAddress) return false;
            sections.push_back({s.VirtualAddress, length, s.Characteristics});
        }
        return true;
    }
    bool Range(uint32_t offset, size_t bytes, bool readonly = false) const {
        if (offset >= size || bytes > size - offset) return false;
        for (const auto& s : sections) if (offset >= s.begin && offset - s.begin < s.size && bytes <= s.size - (offset - s.begin)) {
            if (!(s.flags & IMAGE_SCN_MEM_READ)) return false;
            if (readonly && (s.flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE))) return false;
            return Readable(data + offset, bytes);
        }
        return false;
    }
    template<class T> bool Get(uint32_t offset, T& value, bool readonly = true) const {
        return Range(offset, sizeof(T), readonly) && Read(data + offset, value);
    }
    bool Executable(uintptr_t address) const {
        const auto start = reinterpret_cast<uintptr_t>(data);
        if (address < start || address - start >= size) return false;
        const auto offset = static_cast<uint32_t>(address - start);
        for (const auto& s : sections) if (offset >= s.begin && offset - s.begin < s.size)
            return (s.flags & IMAGE_SCN_MEM_EXECUTE) != 0;
        return false;
    }
    std::string Type(uint32_t offset) const {
        if (!Range(offset, 17)) return {};
        std::string name;
        for (uint32_t i = 16; i < 512 && offset <= UINT32_MAX - i; ++i) {
            char c{};
            if (!Get(offset + i, c, false)) return {};
            if (!c) return name.starts_with(".?A") ? name : std::string{};
            if (c < 32 || c > 126) return {};
            name += c;
        }
        return {};
    }
    template<class F> void Scan(uint32_t alignment, size_t length, F&& f) const {
        for (const auto& s : sections) {
            if (!(s.flags & IMAGE_SCN_MEM_READ) || (s.flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) || s.size < length) continue;
            // Loaded modules can contain discarded data. Never scan an unreadable section.
            if (!Readable(data + s.begin, s.size)) continue;
            for (uint32_t rva = (s.begin + alignment - 1) & ~(alignment - 1); rva - s.begin <= s.size - length; rva += alignment) f(rva);
        }
    }
};
struct Base { Descriptor descriptor; std::string name; };
struct Group { std::string name; std::vector<Base> bases; std::map<uint32_t, std::vector<std::pair<uint32_t, Locator>>> locators; };

bool ReadBases(const Image& image, const Locator& locator, std::vector<Base>& bases) {
    Hierarchy hierarchy{};
    if (!image.Get(locator.hierarchy, hierarchy) || hierarchy.signature != 0 || hierarchy.count == 0 || hierarchy.count > 128 ||
        !image.Range(hierarchy.bases, size_t(hierarchy.count) * 4, true)) return false;
    for (uint32_t i = 0; i < hierarchy.count; ++i) {
        uint32_t rva{}; Descriptor d{};
        if (!image.Get(hierarchy.bases + i * 4, rva) || !image.Get(rva, d) ||
            d.count > hierarchy.count - i - 1 || d.mdisp < 0 || d.mdisp > static_cast<int32_t>(MaxObjectOffset) || d.pdisp < -1 ||
            d.pdisp > static_cast<int32_t>(MaxObjectOffset) || d.vdisp < 0 || d.vdisp > 512 || d.vdisp % 4 || (d.pdisp >= 0 && !d.vdisp)) return false;
        auto name = image.Type(d.type);
        if (name.empty()) return false;
        bases.push_back({d, std::move(name)});
    }
    return !bases.empty() && bases.front().descriptor.type == locator.type;
}
bool Vtable(const Image& image, uint32_t locatorRva, uint32_t methods, void**& result) {
    const auto locatorAddress = reinterpret_cast<uintptr_t>(image.data + locatorRva);
    std::set<uint32_t> tables;
    image.Scan(8, sizeof(uintptr_t) * (size_t(methods) + 1), [&](uint32_t rva) {
        uintptr_t candidate{}; std::memcpy(&candidate, image.data + rva, sizeof(candidate));
        if (candidate != locatorAddress) return;
        for (uint32_t i = 0; i < methods; ++i) {
            uintptr_t method{}; std::memcpy(&method, image.data + rva + (i + 1) * sizeof(uintptr_t), sizeof(method));
            if (!image.Executable(method)) return;
        }
        tables.insert(rva + 8);
    });
    if (tables.size() != 1) return false;
    result = reinterpret_cast<void**>(const_cast<uint8_t*>(image.data + *tables.begin()));
    return true;
}

bool Resolve(const Image& image, const Group& group, std::vector<uint32_t>& offsets, std::map<int32_t, int32_t>& tops) {
    offsets.resize(group.bases.size());
    std::map<int32_t, std::vector<size_t>> virtualGroups;
    for (size_t i = 0; i < group.bases.size(); ++i) {
        const auto& d = group.bases[i].descriptor;
        if (d.pdisp < 0) offsets[i] = static_cast<uint32_t>(d.mdisp);
        else virtualGroups[d.pdisp].push_back(i);
    }
    for (const auto& [pdisp, indices] : virtualGroups) {
        // A secondary nonvirtual base owns its own vbptr. Its vbtable[0]
        // points back to that base, not necessarily to the complete object.
        int32_t ancestorOffset{};
        for (size_t i = 0; i < indices.front(); ++i) {
            const auto& d = group.bases[i].descriptor;
            if (d.pdisp < 0 && i + d.count >= indices.back()) ancestorOffset = d.mdisp;
        }
        const int32_t top = ancestorOffset - pdisp;
        if (top > 0) return false;
        tops[pdisp] = top;
        int32_t last{};
        for (const auto i : indices) last = (std::max)(last, group.bases[i].descriptor.vdisp);
        std::set<std::vector<uint32_t>> mappings;
        image.Scan(4, size_t(last) + 4, [&](uint32_t rva) {
            int32_t first{}; std::memcpy(&first, image.data + rva, sizeof(first));
            if (first != top) return;
            std::vector<uint32_t> candidate;
            std::map<int32_t, uint32_t> entries;
            std::set<uint32_t> uniqueOffsets;
            for (const auto i : indices) {
                const auto& d = group.bases[i].descriptor;
                int32_t displacement{}; std::memcpy(&displacement, image.data + rva + d.vdisp, sizeof(displacement));
                const auto offset = int64_t(d.mdisp) + pdisp + displacement;
                if (offset < 0 || offset > MaxObjectOffset || !group.locators.contains(static_cast<uint32_t>(offset))) return;
                const auto [entry, inserted] = entries.emplace(d.vdisp, static_cast<uint32_t>(offset));
                if ((!inserted && entry->second != offset) || (inserted && !uniqueOffsets.insert(static_cast<uint32_t>(offset)).second)) return;
                candidate.push_back(static_cast<uint32_t>(offset));
            }
            mappings.insert(std::move(candidate));
        });
        // Several duplicate compiler tables are fine; distinct mappings are not.
        if (mappings.size() != 1) return false;
        const auto& mapping = *mappings.begin();
        for (size_t i = 0; i < indices.size(); ++i) offsets[indices[i]] = mapping[i];
    }
    return true;
}
bool Choose(const Image& image, const Group& group, const std::vector<uint32_t>& offsets,
            std::string_view name, uint32_t methods, Subobject& out) {
    std::set<uint32_t> matches;
    for (size_t i = 0; i < group.bases.size(); ++i) if (group.bases[i].name == name) {
        // Reject ambiguous/private occurrences of the public interface.
        if (group.bases[i].descriptor.attributes & 0x0f) return false;
        matches.insert(offsets[i]);
    }
    if (matches.size() != 1) return false;
    const auto offset = *matches.begin();
    const auto it = group.locators.find(offset);
    if (it == group.locators.end()) return false;
    std::vector<Subobject> candidates;
    for (const auto& [rva, locator] : it->second) {
        void** table{};
        if (Vtable(image, rva, methods, table)) candidates.push_back({table, offset, locator.cdOffset});
    }
    if (candidates.size() != 1) return false;
    out = candidates.front(); return true;
}
}

std::vector<ClassInfo> Discover(HMODULE module, std::string& diagnostic) {
    std::vector<ClassInfo> result;
    Image image;
    if (!image.Open(module)) { diagnostic = "invalid/unreadable x64 PE image"; return result; }
    std::map<std::pair<uint32_t, uint32_t>, Group> groups;
    image.Scan(4, sizeof(Locator), [&](uint32_t rva) {
        Locator locator{}; std::memcpy(&locator, image.data + rva, sizeof(locator));
        if (locator.signature != 1 || locator.self != rva || locator.offset > MaxObjectOffset || locator.cdOffset > 64) return;
        auto name = image.Type(locator.type);
        if (name.empty()) return;
        std::vector<Base> bases;
        if (!ReadBases(image, locator, bases)) return;
        bool dx11{}, dx12{};
        for (const auto& b : bases) { dx11 |= b.name == Dx11Name; dx12 |= b.name == Dx12Name; }
        if (dx11 == dx12) return;
        auto [it, inserted] = groups.try_emplace({locator.type, locator.hierarchy});
        if (inserted) { it->second.name = std::move(name); it->second.bases = std::move(bases); }
        it->second.locators[locator.offset].emplace_back(rva, locator);
    });
    size_t rejected{};
    for (const auto& [identity, group] : groups) {
        (void)identity;
        ClassInfo info; info.module = module; info.imageSize = image.size; info.className = group.name;
        info.dx12 = std::any_of(group.bases.begin(), group.bases.end(), [](const Base& b) { return b.name == Dx12Name; });
        std::vector<uint32_t> offsets;
        std::map<int32_t, int32_t> tops;
        if (!Resolve(image, group, offsets, tops) || !Choose(image, group, offsets, info.dx12 ? Dx12Name : Dx11Name, info.dx12 ? 5 : 2, info.typed) ||
            !Choose(image, group, offsets, BaseName, 2, info.base) || !Choose(image, group, offsets, DestroyName, 1, info.destroy)) { ++rejected; continue; }
        for (size_t i = 0; i < group.bases.size(); ++i) {
            const auto& d = group.bases[i].descriptor;
            info.bases.push_back({d.mdisp, d.pdisp, d.vdisp, offsets[i], d.pdisp < 0 ? 0 : tops.at(d.pdisp)});
        }
        result.push_back(std::move(info));
    }
    std::ostringstream message;
    message << "modern RTTI: " << result.size() << " concrete interface layout(s), " << rejected << " unsupported/ambiguous layout(s)";
    diagnostic = message.str(); return result;
}

bool Locate(const ClassInfo& info, void* self, InterfaceKind kind, ObjectViews& out) noexcept {
    out = {};
    const auto& selected = kind == InterfaceKind::Typed ? info.typed : kind == InterfaceKind::Base ? info.base : info.destroy;
    void** table{};
    if (!self || !Read(self, table) || table != selected.vtable) return false;
    const auto address = reinterpret_cast<uintptr_t>(self);
    int32_t constructionDisplacement{};
    if (selected.cdOffset && (address < selected.cdOffset || !Read(reinterpret_cast<void*>(address - selected.cdOffset), constructionDisplacement))) return false;
    const int64_t displacement = int64_t(selected.offset) + constructionDisplacement;
    if (displacement < 0 || static_cast<uint64_t>(displacement) > address) return false;
    const auto complete = address - static_cast<uintptr_t>(displacement);
    // A construction/destruction adjustment is real MSVC metadata, but its
    // transient layout is not a fully initialized weaver suitable for capture.
    if (constructionDisplacement != 0) return false;
    const auto image = reinterpret_cast<uintptr_t>(info.module);
    for (const auto& b : info.bases) {
        int64_t offset = b.mdisp;
        if (b.pdisp >= 0) {
            uintptr_t vbtable{}; int32_t entry{}, top{};
            if (complete > UINTPTR_MAX - static_cast<uint32_t>(b.pdisp) || !Read(reinterpret_cast<void*>(complete + b.pdisp), vbtable) ||
                vbtable < image || vbtable - image >= info.imageSize || static_cast<uint64_t>(b.vdisp) + 4 > info.imageSize - (vbtable - image) ||
                !Read(reinterpret_cast<void*>(vbtable), top) || top != b.vbtableTop ||
                !Read(reinterpret_cast<void*>(vbtable + b.vdisp), entry)) return false;
            offset += int64_t(b.pdisp) + entry;
        }
        if (offset != b.offset) return false;
    }
    auto check = [&](const Subobject& part, void*& view) {
        if (complete > UINTPTR_MAX - part.offset) return false;
        view = reinterpret_cast<void*>(complete + part.offset);
        void** actual{};
        if (!Read(view, actual) || actual != part.vtable) return false;
        int32_t adjust{};
        return !part.cdOffset || (reinterpret_cast<uintptr_t>(view) >= part.cdOffset &&
            Read(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(view) - part.cdOffset), adjust) && adjust == 0);
    };
    ObjectViews views;
    if (!check(info.typed, views.typed) || !check(info.base, views.base) || !check(info.destroy, views.destroy)) return false;
    out = views; return true;
}
}
