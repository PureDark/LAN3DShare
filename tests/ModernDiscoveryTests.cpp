#include "capture/ModernDiscovery.h"
#include <sr/weaver/dx11weaver.h>
#include <sr/weaver/dx12weaver.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace SR { IDX11Weaver1::~IDX11Weaver1() = default; IDX12Weaver1::~IDX12Weaver1() = default; }
using namespace lan3d::capture::modern;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class Interface> struct Direct : Interface {};
template<class Interface> struct Virtual : virtual Interface {};
template<class Base> class Fixture : public Base {
public:
    void destroy() override {}
    void setWindowHandle(HWND) override {}
    void weave() override {}
    void enableLateLatching(bool) override {}
    bool isLateLatchingEnabled() const override { return false; }
    void setShaderSRGBConversion(bool, bool) override {}
    void setLatencyInFrames(uint64_t) override {}
    void setLatency(uint64_t) override {}
    uint64_t getLatency() const override { return 0; }
    void getPredictedEyePositions(float[3], float[3]) override {}
    void setInputViewTexture(ID3D11ShaderResourceView*, int, int, DXGI_FORMAT) {}
    void setInputViewTexture(ID3D12Resource*, int, int, DXGI_FORMAT) {}
    void setContext(ID3D11DeviceContext*) {}
    void setOutputFormat(DXGI_FORMAT) {}
    void setCommandList(ID3D12GraphicsCommandList*) {}
    void setViewport(D3D12_VIEWPORT) {}
    void setScissorRect(D3D12_RECT) {}
protected:
    void* queryInterface(std::type_index) override { return nullptr; }
private:
    // Ensures this does not accidentally rely on a minimum-layout probe object.
    std::array<uint64_t, 37> storage_{};
};
struct Prefix { virtual ~Prefix() = default; virtual void Other() {} std::array<uint64_t, 3> padding{}; };
template<class Interface> class Shifted final : public Prefix, public Fixture<Direct<Interface>> {};

template<class Interface, class T> void Verify(T& object, const std::vector<ClassInfo>& classes) {
    auto typed = static_cast<Interface*>(&object);
    auto base = static_cast<SR::IWeaverBase1*>(typed);
    auto destroy = static_cast<SR::IDestroyable*>(typed);
    const auto table = *reinterpret_cast<void***>(typed);
    const auto found = std::find_if(classes.begin(), classes.end(), [&](const ClassInfo& c) { return c.typed.vtable == table; });
    Check(found != classes.end(), "fixture concrete interface table was not discovered");
    for (const auto kind : {InterfaceKind::Typed, InterfaceKind::Base, InterfaceKind::Destroy}) {
        void* self = kind == InterfaceKind::Typed ? static_cast<void*>(typed) : kind == InterfaceKind::Base ? static_cast<void*>(base) : static_cast<void*>(destroy);
        ObjectViews views;
        Check(Locate(*found, self, kind, views), "real interface callback could not recover aliases");
        Check(views.typed == typed && views.base == base && views.destroy == destroy, "recovered wrong complete-object aliases");
        Check(!Locate(*found, reinterpret_cast<void*>(1), kind, views), "unreadable self was accepted");
    }
    ObjectViews views;
    Check(!Locate(*found, nullptr, InterfaceKind::Typed, views), "null callback accepted");
    Check(!Locate(*found, base, InterfaceKind::Typed, views), "wrong subobject kind accepted");
    const auto& sub = found->base;
    if (sub.cdOffset) {
        auto displacement = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(base) - sub.cdOffset);
        const auto old = *displacement; *displacement = 16;
        const bool accepted = Locate(*found, base, InterfaceKind::Base, views);
        *displacement = old;
        Check(!accepted, "transient construction vtordisp accepted");
    }
    std::cout << found->className << ": typed=" << found->typed.offset << " base=" << found->base.offset
        << " destroy=" << found->destroy.offset << '\n';
}

// Minimal independent PE/RTTI image permits malformed and ambiguous metadata
// checks without changing a loaded DLL or calling any code in the image.
class MetadataImage {
public:
    uint8_t* bytes = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x5000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    MetadataImage() {
        Check(bytes != nullptr, "synthetic image allocation");
        IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80; Put(0, dos);
        IMAGE_NT_HEADERS64 nt{}; nt.Signature = IMAGE_NT_SIGNATURE; nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 2; nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt.OptionalHeader.SizeOfImage = 0x5000; Put(0x80, nt);
        IMAGE_SECTION_HEADER text{}; text.VirtualAddress = 0x1000; text.Misc.VirtualSize = 0x1000;
        text.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        IMAGE_SECTION_HEADER data{}; data.VirtualAddress = 0x2000; data.Misc.VirtualSize = 0x2000; data.Characteristics = IMAGE_SCN_MEM_READ;
        Put(0x80 + sizeof(nt), text); Put(0x80 + sizeof(nt) + sizeof(text), data);
        const char* names[] = {".?AVSyntheticWeaver@@", ".?AVIDX12Weaver1@SR@@", ".?AVIDestroyable@SR@@", ".?AVIQueryInterface@SR@@", ".?AVIWeaverBase1@SR@@"};
        for (uint32_t i = 0; i < 5; ++i) std::memcpy(bytes + 0x2010 + i * 0x80, names[i], std::strlen(names[i]) + 1);
        const std::array<uint32_t, 4> hierarchy{0, 3, 5, 0x2500}; Put(0x2400, hierarchy);
        for (uint32_t i = 0; i < 5; ++i) {
            Put(0x2500 + i * 4, uint32_t(0x2600 + i * 0x20));
            // Concrete and typed interface are nonvirtual; remaining bases use vbptr+8.
            const std::array<int32_t, 7> b{int32_t(0x2000 + i * 0x80), i == 0 ? 4 : i == 1 ? 3 : 0,
                0, i < 2 ? -1 : 8, i < 2 ? 0 : int32_t((i - 1) * 4), i < 2 ? 0x40 : 0x50, 0x2400};
            Put(0x2600 + i * 0x20, b);
        }
        const uint32_t offsets[] = {0, 80, 96, 112};
        for (uint32_t i = 0; i < 4; ++i) {
            const std::array<uint32_t, 6> col{1, offsets[i], i ? 4u : 0u, 0x2000, 0x2400, 0x2800 + i * 0x20};
            Put(0x2800 + i * 0x20, col);
            Put(0x2a00 + i * 0x80, reinterpret_cast<uintptr_t>(bytes + 0x2800 + i * 0x20));
            for (uint32_t n = 1; n <= 6; ++n) Put(0x2a00 + i * 0x80 + n * 8, reinterpret_cast<uintptr_t>(bytes + 0x1000));
        }
        const std::array<int32_t, 4> vb{-8, 72, 88, 104}; Put(0x3000, vb);
    }
    ~MetadataImage() { if (bytes) VirtualFree(bytes, 0, MEM_RELEASE); }
    template<class T> void Put(uint32_t rva, const T& value) { std::memcpy(bytes + rva, &value, sizeof(value)); }
    std::vector<ClassInfo> Scan() { std::string message; return Discover(reinterpret_cast<HMODULE>(bytes), message); }
};

int wmain(int argc, wchar_t** argv) {
    try {
        Fixture<Direct<SR::IDX11Weaver1>> direct11;
        Fixture<Direct<SR::IDX12Weaver1>> direct12;
        Fixture<Virtual<SR::IDX11Weaver1>> virtual11;
        Fixture<Virtual<SR::IDX12Weaver1>> virtual12;
        Shifted<SR::IDX11Weaver1> shifted11;
        Shifted<SR::IDX12Weaver1> shifted12;
        std::string diagnostic;
        const auto classes = Discover(GetModuleHandleW(nullptr), diagnostic);
        std::cout << diagnostic << '\n';
        Verify<SR::IDX11Weaver1>(direct11, classes); Verify<SR::IDX12Weaver1>(direct12, classes);
        Verify<SR::IDX11Weaver1>(virtual11, classes); Verify<SR::IDX12Weaver1>(virtual12, classes);
        Verify<SR::IDX11Weaver1>(shifted11, classes); Verify<SR::IDX12Weaver1>(shifted12, classes);
        MetadataImage metadata;
        Check(metadata.Scan().size() == 1, "valid independent metadata was rejected");
        const std::array<int32_t, 4> duplicate{-8, 72, 88, 104}; metadata.Put(0x3100, duplicate);
        Check(metadata.Scan().size() == 1, "duplicate identical vbtable mappings rejected");
        const std::array<int32_t, 4> ambiguous{-8, 88, 72, 104}; metadata.Put(0x3100, ambiguous);
        Check(metadata.Scan().empty(), "ambiguous vbtable mapping accepted");
        metadata.Put(0x3100, duplicate); metadata.Put(0x2408, uint32_t(0xffffffff));
        Check(metadata.Scan().empty(), "invalid RTTI base count accepted");
        Check(Discover(reinterpret_cast<HMODULE>(1), diagnostic).empty(), "unreadable image accepted");
        for (int i = 1; i < argc; ++i) {
            // Map/relocate only: do not execute vendor entrypoints, resolve its
            // dependencies, create a context, or connect to a display/runtime.
            HMODULE image = LoadLibraryExW(argv[i], nullptr, DONT_RESOLVE_DLL_REFERENCES);
            Check(image != nullptr, "could not map supplied runtime image");
            const auto layouts = Discover(image, diagnostic);
            std::wcout << argv[i] << L"\n";
            std::cout << diagnostic << '\n';
            FreeLibrary(image);
            Check(!layouts.empty(), "supplied runtime has no supported interface layouts");
        }
        std::cout << "Modern discovery: metadata, aliases, virtual inheritance, malformed/ambiguous input passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
