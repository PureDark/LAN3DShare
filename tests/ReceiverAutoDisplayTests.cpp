#include "receiver/Renderer.h"
#include "receiver_runtime/TestState.h"
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }
lan3d::Frame Pattern(lan3d::ContentKind kind) {
    lan3d::Frame frame{640, 360, 640 * 4, 1, 1, {}};
    frame.kind = kind;
    frame.bgra.resize(size_t(frame.stride) * frame.height);
    for (unsigned y = 0; y < frame.height; ++y) for (unsigned x = 0; x < frame.width; ++x) {
        auto* pixel = frame.bgra.data() + size_t(y) * frame.stride + x * 4;
        pixel[0] = x < 320 ? 37 : 193; pixel[1] = x < 320 ? 139 : 71;
        pixel[2] = x < 320 ? 217 : 43; pixel[3] = 255;
    }
    return frame;
}
class FakeRuntime {
public:
    explicit FakeRuntime(const std::filesystem::path& directory) {
        // Keep the fixture DLLs loaded only to inspect counters after the
        // renderer closes its references. Never load an installed runtime.
        core_ = LoadLibraryExW((directory / L"SimulatedRealityCore.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        directx_ = LoadLibraryExW((directory / L"SimulatedRealityDirectX.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        getCore_ = core_ ? reinterpret_cast<ReceiverCoreState (*)()>(GetProcAddress(core_, "ReceiverGetCoreState")) : nullptr;
        getWeaver_ = directx_ ? reinterpret_cast<ReceiverWeaverState (*)()>(GetProcAddress(directx_, "ReceiverGetWeaverState")) : nullptr;
        if (!getCore_ || !getWeaver_) { Close(); throw std::runtime_error("Expected receiver fake runtime with counter exports"); }
    }
    ~FakeRuntime() { Close(); }
    ReceiverCoreState Core() const { return getCore_(); }
    ReceiverWeaverState Weaver() const { return getWeaver_(); }
private:
    void Close() { if (directx_) FreeLibrary(directx_); if (core_) FreeLibrary(core_); directx_ = core_ = nullptr; }
    HMODULE core_{}, directx_{};
    ReceiverCoreState (*getCore_)(){};
    ReceiverWeaverState (*getWeaver_)(){};
};
void FullscreenLifecycle(HWND window, const std::filesystem::path& directory, const FakeRuntime& fixture, bool legacy) {
    SetEnvironmentVariableW(L"LAN3D_RECEIVER_TEST_LEGACY", legacy ? L"1" : nullptr);
    const auto coreBefore = fixture.Core();
    const auto weaverBefore = fixture.Weaver();
    auto check = [&](unsigned created, unsigned destroyed, unsigned weaves) {
        const auto core = fixture.Core();
        const auto weaver = fixture.Weaver();
        Require(core.created - coreBefore.created == created && core.initialized - coreBefore.initialized == created &&
            core.destroyed - coreBefore.destroyed == destroyed, "Fullscreen content transition context lifecycle mismatch");
        Require(weaver.weaveCount - weaverBefore.weaveCount == weaves, "Fullscreen content transition weave count mismatch");
        Require(weaver.modernAttempts - weaverBefore.modernAttempts == created, "Runtime reopen did not retry the selected ABI");
        if (legacy) {
            Require(weaver.legacyCreated - weaverBefore.legacyCreated == created &&
                weaver.legacyDestroyed - weaverBefore.legacyDestroyed == destroyed &&
                weaver.modernCreated == weaverBefore.modernCreated, "Legacy fullscreen lifecycle mismatch");
        } else {
            Require(weaver.modernCreated - weaverBefore.modernCreated == created &&
                weaver.modernDestroyed - weaverBefore.modernDestroyed == destroyed &&
                weaver.legacyCreated == weaverBefore.legacyCreated, "Modern fullscreen lifecycle mismatch");
        }
    };
    {
        lan3d::receiver::Renderer renderer;
        renderer.Open(window, MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), false, directory, true);
        std::string error;
        renderer.Upload(Pattern(lan3d::ContentKind::Desktop));
        Require(renderer.SetFullscreen(false, error) && renderer.Draw(true), "Windowed desktop failed with available fake runtime");
        Require(renderer.SetFullscreen(true, error) && renderer.Draw(true), "Fullscreen desktop failed with available fake runtime");
        check(0, 0, 0);
        renderer.Upload(Pattern(lan3d::ContentKind::LeiaSbs));
        Require(renderer.SetFullscreen(false, error) && renderer.Draw(true), "Windowed SBS left eye failed with available fake runtime");
        auto halfSbs = Pattern(lan3d::ContentKind::LeiaSbs);
        halfSbs.displayWidth = 640; halfSbs.displayHeight = 360;
        renderer.Upload(halfSbs);
        Require(renderer.Draw(true), "Windowed half-SBS did not restore the original single-eye display aspect");
        check(0, 0, 0);
        Require(renderer.SetFullscreen(true, error) && renderer.Draw() && renderer.Draw(), "Fullscreen SBS did not weave");
        check(1, 0, 2);
        const auto input = fixture.Weaver();
        Require(input.textureWidth == 640 && input.suppliedWidth == (legacy ? 640u : 320u), "Fullscreen SBS width contract mismatch");
        Require(input.height == 360, "Logical display aspect changed the actual weaver texture height");
        Require(renderer.SetFullscreen(false, error), "Leaving fullscreen failed");
        check(1, 1, 2);
        Require(renderer.Draw(true), "Windowed SBS did not recover left-eye rendering");
        check(1, 1, 2);
        Require(renderer.SetFullscreen(true, error) && renderer.Draw(), "Reentering fullscreen did not restore weaving");
        check(2, 1, 3);
        renderer.Upload(Pattern(lan3d::ContentKind::Desktop));
        check(2, 2, 3); // Upload must retire 3D immediately, before the next draw.
        Require(renderer.SetFullscreen(true, error) && renderer.Draw(true), "Fullscreen switch to desktop failed");
        check(2, 2, 3);
        renderer.Upload(Pattern(lan3d::ContentKind::LeiaSbs));
        Require(renderer.SetFullscreen(true, error) && renderer.Draw(), "Fullscreen return to SBS did not restore weaving");
        check(3, 2, 4);
    }
    check(3, 3, 4); // Closing an active fullscreen renderer also cleans up.
    SetEnvironmentVariableW(L"LAN3D_RECEIVER_TEST_LEGACY", nullptr);
}
}
int wmain(int argc, wchar_t** argv) try {
    Require(argc == 2, "Expected the receiver fake runtime directory");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName=L"LAN3DAutoDisplayTest";
    Require(RegisterClassW(&cls) != 0, "Cannot register test window");
    HWND window = CreateWindowW(cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 800, 650, nullptr, nullptr, cls.hInstance, nullptr);
    Require(window != nullptr, "Cannot create hidden test window");
    {
        lan3d::receiver::Renderer renderer;
        // A deliberately nonexistent path makes any accidental runtime load
        // fail; this test must remain completely independent of real hardware.
        renderer.Open(window, MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), false,
            std::filesystem::path(L"Z:\\LAN3D-does-not-exist\\LeiaRuntime"), true);
        std::string error;
        renderer.Upload(Pattern(lan3d::ContentKind::Desktop));
        Require(renderer.SetFullscreen(false, error) && renderer.Draw(true), "Windowed desktop failed");
        Require(renderer.SetFullscreen(true, error) && renderer.Draw(true), "Fullscreen desktop tried to initialize Leia");
        Require(!GetModuleHandleW(L"SimulatedRealityDirectX.dll"), "Desktop loaded Leia runtime");
        renderer.Upload(Pattern(lan3d::ContentKind::LeiaSbs));
        Require(renderer.SetFullscreen(false, error) && renderer.Draw(true), "Windowed SBS left-eye preview failed");
        Require(!GetModuleHandleW(L"SimulatedRealityDirectX.dll"), "Windowed SBS loaded Leia runtime");
        Require(!renderer.SetFullscreen(true, error) && !error.empty(), "Missing fullscreen runtime was not reported");
        Require(renderer.SetFullscreen(false, error) && renderer.Draw(true), "2D recovery after runtime failure failed");
        renderer.Upload(Pattern(lan3d::ContentKind::Desktop));
        Require(renderer.SetFullscreen(true, error) && renderer.Draw(true), "Return to desktop failed");
    }
    const auto directory = std::filesystem::canonical(argv[1]);
    {
        FakeRuntime fixture(directory);
        FullscreenLifecycle(window, directory, fixture, false);
        FullscreenLifecycle(window, directory, fixture, true);
    }
    Require(!GetModuleHandleW(L"SimulatedRealityDirectX.dll") && !GetModuleHandleW(L"SimulatedRealityCore.dll"),
        "Receiver leaked fake runtime module references");
    DestroyWindow(window);
    std::cout << "Receiver auto display PASS: desktop and windowed SBS stay 2D; missing runtime recovers; fake modern/legacy fullscreen weave, exit, Desktop/SBS transitions, and teardown\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "Receiver auto display FAILED: " << e.what() << '\n'; return 1; }
