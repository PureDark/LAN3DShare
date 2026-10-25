#pragma once
#include <MinHook.h>
#include <array>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>

namespace lan3d::capture {
inline std::recursive_mutex hookMutex;
inline std::map<void*, const void*> hookOwners;
// Each installed target gets its own typed thunk and original function. This
// also supports multiple adapters/runtimes whose COM implementation addresses
// differ. Never call the original through a possibly re-hooked vtable entry.
template<class Tag, class Signature> struct Hooks;
template<class Tag, class R, class... A> struct Hooks<Tag, R(A...)> {
    using Function = R(*)(A...);
    struct Entry { void* target{}; Function original{}; };
    static inline std::array<Entry, 64> entries{};
    static inline int owner;
    template<size_t I> static R Thunk(A... args) { return Tag::Call(entries[I].original, entries[I].target, args...); }
    template<size_t... I> static auto Thunks(std::index_sequence<I...>) { return std::array<Function, sizeof...(I)>{&Thunk<I>...}; }
    static bool Install(void* target) {
        if (!target) return false;
        std::lock_guard lock(hookMutex);
        if (const auto it = hookOwners.find(target); it != hookOwners.end()) return it->second == &owner;
        static const auto thunks = Thunks(std::make_index_sequence<64>{});
        for (size_t i = 0; i < entries.size(); ++i) if (!entries[i].target) {
            auto& entry = entries[i]; entry.target = target;
            if (MH_CreateHook(target, reinterpret_cast<void*>(thunks[i]), reinterpret_cast<void**>(&entry.original)) != MH_OK) { entry = {}; return false; }
            hookOwners[target] = &owner;
            if (MH_EnableHook(target) != MH_OK) { hookOwners.erase(target); MH_RemoveHook(target); entry = {}; return false; }
            return true;
        }
        return false;
    }
};
template<class Tag> bool Hook(void* address) { return Hooks<Tag, typename Tag::Signature>::Install(address); }
inline void* Method(void* object, unsigned index) { return (*reinterpret_cast<void***>(object))[index]; }
}
