#include "App.h"
#include "app/Workers.h"
#include <shellapi.h>
#include <iostream>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    int result{};
    try {
        if (argc > 1 && std::wstring(argv[1]) == L"--internal-send") result = lan3d::RunSender(argc - 1, argv + 1);
        else if (argc > 1 && std::wstring(argv[1]) == L"--internal-receive") result = lan3d::RunReceiver(argc - 1, argv + 1);
        else result = lan3d::ui::RunApplication(instance, argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    LocalFree(argv);
    return result;
}
