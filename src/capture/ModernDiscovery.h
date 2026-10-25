#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace lan3d::capture::modern {
enum class InterfaceKind { Typed, Base, Destroy };
struct Subobject {
    void** vtable{};
    uint32_t offset{}, cdOffset{};
};
struct BaseOffset {
    int32_t mdisp{}, pdisp{-1}, vdisp{};
    uint32_t offset{};
    int32_t vbtableTop{};
};
struct ClassInfo {
    HMODULE module{};
    uint32_t imageSize{};
    std::string className;
    bool dx12{};
    Subobject typed, base, destroy;
    // The loaded image's own RTTI/vbtable relationships, not runtime-version offsets.
    std::vector<BaseOffset> bases;
};
struct ObjectViews { void* typed{}; void* base{}; void* destroy{}; };

// Reads metadata only. The caller pins this module before installing any hooks.
// Supply the actual implementation module when an export is forwarded.
std::vector<ClassInfo> Discover(HMODULE module, std::string& diagnostic);

// Called only for an actual intercepted public-interface call. Never enumerates
// allocations or calls methods on an object while determining its interfaces.
bool Locate(const ClassInfo& info, void* self, InterfaceKind kind, ObjectViews& out) noexcept;
}
