#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace lan3d::ui {

// All methods and the owner window's message loop run on the same COM STA.
class WebViewHost {
public:
    WebViewHost();
    ~WebViewHost();
    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    bool Open(HWND parent, const std::filesystem::path& dataDirectory, bool transparent,
        std::function<void(const std::string&)> onMessage,
        std::function<void(const std::string&)> onError);
    void Resize();
    void Post(const std::string& json);
    void Execute(const std::wstring& javascript);
    bool Ready() const;
    void Capture(const std::filesystem::path& png, std::function<void(bool)> done);
    void Close();

private:
    struct State;
    std::shared_ptr<State> state_;
};

}
