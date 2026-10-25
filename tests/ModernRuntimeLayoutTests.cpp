#include "capture/ModernDiscovery.h"
#include "capture/SetterAnalysis.h"
#include <iostream>
#include <stdexcept>

// Inspect the installed image only: no DllMain, factory, context, display, or
// game is invoked. Locations come from that image's metadata, never an RVA list.
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) throw std::runtime_error("supply at least one runtime DLL");
        for (int i = 1; i < argc; ++i) {
            const auto module = LoadLibraryExW(argv[i], nullptr, DONT_RESOLVE_DLL_REFERENCES);
            if (!module) throw std::runtime_error("runtime image mapping failed");
            std::string diagnostic;
            const auto classes = lan3d::capture::modern::Discover(module, diagnostic);
            std::wcout << argv[i] << L'\n';
            std::cout << diagnostic << '\n';
            if (classes.empty()) throw std::runtime_error("no provable modern weaver interface layout");
            for (const auto& info : classes) {
                // Public v1 headers: input is the first typed method; context
                // is DX11's second and command list is DX12's third method.
                const auto input = lan3d::capture::modern::AnalyzeSetter(info.typed.vtable[0]);
                const auto execution = lan3d::capture::modern::AnalyzeSetter(info.typed.vtable[info.dx12 ? 2 : 1]);
                std::cout << info.className << " input=" << input.diagnostic << " execution=" << execution.diagnostic << '\n';
                if (!input.valid || !execution.valid) throw std::runtime_error("existing input/context plan unsupported");
                if (info.dx12) {
                    const auto format = lan3d::capture::modern::AnalyzeSetter(info.typed.vtable[0], 4, 4);
                    std::cout << "format=" << format.diagnostic << '\n';
                    if (!format.valid) throw std::runtime_error("existing input format plan unsupported");
                }
            }
            FreeLibrary(module);
        }
        std::cout << "Runtime metadata and setter plans PASS (static inspection only)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
