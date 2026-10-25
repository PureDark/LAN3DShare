#include "WebViewHost.h"
#include <objbase.h>
#include <WebView2.h>
#include <shlwapi.h>
#include <wrl.h>
#include <cstdio>
#include <cwchar>
#include <utility>

namespace lan3d::ui {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr size_t kMaximumMessageCharacters = 64 * 1024;
constexpr int kHtmlResource = 201;

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!count) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), result.data(), count);
    return result;
}

std::string Utf8(const wchar_t* wide, int length) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide, length, nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length,
        result.data(), count, nullptr, nullptr);
    return result;
}
}

struct WebViewHost::State {
    HWND parent{};
    bool closed{};
    bool failed{};
    bool ready{};
    bool navigationStarted{};
    UINT64 navigationId{};
    std::function<void(const std::string&)> onMessage;
    std::function<void(const std::string&)> onError;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;

    bool Active() const { return !closed && parent && IsWindow(parent); }

    void Fail(HRESULT hr, const char* operation) {
        if (!Active() || failed) return;
        failed = true;
        ready = false;
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        const auto callback = onError;
        if (callback) callback(std::string("界面加载失败（") + operation + "，" + code +
            "）。请确认已安装 Microsoft Edge WebView2 Runtime。安装地址："
            "https://go.microsoft.com/fwlink/p/?LinkId=2124703");
    }

    void Resize() const {
        if (!Active() || !controller) return;
        RECT bounds{};
        GetClientRect(parent, &bounds);
        controller->put_Bounds(bounds);
        controller->put_IsVisible(IsWindowVisible(parent) && !IsIconic(GetAncestor(parent, GA_ROOT)));
        controller->NotifyParentWindowPositionChanged();
    }
};

WebViewHost::WebViewHost() = default;
WebViewHost::~WebViewHost() { Close(); }

bool WebViewHost::Open(HWND parent, const std::filesystem::path& dataDirectory, bool transparent,
    std::function<void(const std::string&)> onMessage,
    std::function<void(const std::string&)> onError) {
    Close();
    auto state = std::make_shared<State>();
    state_ = state;
    state->parent = parent;
    state->onMessage = std::move(onMessage);
    state->onError = std::move(onError);
    if (!state->Active()) return false;
    std::error_code directoryError;
    std::filesystem::create_directories(dataDirectory, directoryError);
    if (directoryError) {
        state->Fail(HRESULT_FROM_WIN32(directoryError.value()), "缓存目录");
        return false;
    }
    const std::weak_ptr<State> weak = state;
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(nullptr, dataDirectory.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [weak, transparent](HRESULT hr, ICoreWebView2Environment* environment) -> HRESULT {
            auto state = weak.lock();
            if (!state || !state->Active()) return S_OK;
            if (FAILED(hr) || !environment) { state->Fail(FAILED(hr) ? hr : E_POINTER, "浏览器环境"); return S_OK; }
            state->environment = environment;
            const HRESULT create = environment->CreateCoreWebView2Controller(state->parent,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [weak, transparent](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                    auto state = weak.lock();
                    if (!state || !state->Active()) {
                        if (controller) controller->Close();
                        return S_OK;
                    }
                    if (FAILED(hr) || !controller) { state->Fail(FAILED(hr) ? hr : E_POINTER, "浏览器窗口"); return S_OK; }
                    state->controller = controller;
                    hr = controller->get_CoreWebView2(&state->view);
                    if (FAILED(hr) || !state->view) { state->Fail(FAILED(hr) ? hr : E_POINTER, "浏览器接口"); return S_OK; }
                    state->Resize();
                    controller->put_ZoomFactor(1.0);
                    ComPtr<ICoreWebView2Controller2> color;
                    hr = state->controller.As(&color);
                    if (SUCCEEDED(hr)) {
                        hr = color->put_DefaultBackgroundColor(transparent ? COREWEBVIEW2_COLOR{0, 0, 0, 0}
                            : COREWEBVIEW2_COLOR{255, 248, 249, 251});
                    }
                    if (FAILED(hr)) { state->Fail(hr, "窗口背景"); return S_OK; }
                    ComPtr<ICoreWebView2Settings> settings;
                    hr = state->view->get_Settings(&settings);
                    if (FAILED(hr)) { state->Fail(hr, "浏览器设置"); return S_OK; }
                    settings->put_AreDevToolsEnabled(FALSE);
                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                    settings->put_AreHostObjectsAllowed(FALSE);
                    settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings3> settings3;
                    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings5> settings5;
                    if (SUCCEEDED(settings.As(&settings5))) settings5->put_IsPinchZoomEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings6> settings6;
                    if (SUCCEEDED(settings.As(&settings6))) settings6->put_IsSwipeNavigationEnabled(FALSE);

                    // Handlers retain only a weak reference: neither pending creation nor events
                    // can access the host after Close(), including when the owner closes in a callback.
                    EventRegistrationToken token{};
                    hr = state->view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [weak](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            auto state = weak.lock();
                            if (!state || !state->Active() || state->failed || !state->navigationStarted) return S_OK;
                            LPWSTR source{}, current{}, message{};
                            args->get_Source(&source);
                            sender->get_Source(&current);
                            const bool trusted = source && current && wcscmp(source, L"about:blank") == 0
                                && wcscmp(current, L"about:blank") == 0;
                            std::string json;
                            if (trusted && SUCCEEDED(args->get_WebMessageAsJson(&message)) && message) {
                                const auto length = wcsnlen_s(message, kMaximumMessageCharacters + 1);
                                if (length && length <= kMaximumMessageCharacters) json = Utf8(message, static_cast<int>(length));
                            }
                            CoTaskMemFree(source);
                            CoTaskMemFree(current);
                            CoTaskMemFree(message);
                            const auto callback = state->onMessage;
                            if (!json.empty() && callback) callback(json);
                            return S_OK;
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "界面消息"); return S_OK; }
                    hr = state->view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            auto state = weak.lock();
                            LPWSTR uri{};
                            args->get_Uri(&uri);
                            BOOL redirected{};
                            args->get_IsRedirected(&redirected);
                            const bool initial = state && state->Active() && !state->navigationStarted && !redirected
                                && uri && (wcsncmp(uri, L"data:text/html", 14) == 0 || wcscmp(uri, L"about:blank") == 0);
                            args->put_Cancel(!initial);
                            if (initial) { state->navigationStarted = true; args->get_NavigationId(&state->navigationId); }
                            CoTaskMemFree(uri);
                            return S_OK;
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "导航限制"); return S_OK; }
                    hr = state->view->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            return args->put_Cancel(TRUE);
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "子窗口导航限制"); return S_OK; }
                    hr = state->view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                            return args->put_Handled(TRUE);
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "新窗口限制"); return S_OK; }
                    ComPtr<ICoreWebView2_4> view4;
                    hr = state->view.As(&view4);
                    if (SUCCEEDED(hr)) hr = view4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                            args->put_Handled(TRUE);
                            return args->put_Cancel(TRUE);
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "下载限制（请更新 WebView2 Runtime）"); return S_OK; }
                    hr = state->view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                            return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "浏览器权限"); return S_OK; }
                    hr = state->view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            auto state = weak.lock();
                            if (!state || !state->Active() || state->failed) return S_OK;
                            UINT64 id{};
                            args->get_NavigationId(&id);
                            if (!state->navigationStarted || id != state->navigationId) return S_OK;
                            BOOL success{};
                            args->get_IsSuccess(&success);
                            if (success) state->ready = true;
                            else state->Fail(E_FAIL, "页面导航");
                            return S_OK;
                        }).Get(), &token);
                    if (FAILED(hr)) { state->Fail(hr, "页面加载状态"); return S_OK; }

                    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(kHtmlResource), RT_RCDATA);
                    const DWORD bytes = resource ? SizeofResource(nullptr, resource) : 0;
                    const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
                    const auto data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
                    if (!data || !bytes || bytes >= 2 * 1024 * 1024) { state->Fail(E_FAIL, "内嵌界面资源"); return S_OK; }
                    const auto html = Wide(std::string(data, bytes));
                    if (html.empty()) { state->Fail(E_FAIL, "界面字符编码"); return S_OK; }
                    if (transparent) {
                        // The receiver must start in its transparent viewer mode, before
                        // the first paint, rather than briefly showing the sharing window.
                        hr = state->view->AddScriptToExecuteOnDocumentCreated(L"window.__LAN3D_VIEWER__ = true;",
                            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                            [weak, html](HRESULT hr, LPCWSTR) -> HRESULT {
                                auto state = weak.lock();
                                if (!state || !state->Active() || state->failed) return S_OK;
                                if (FAILED(hr)) { state->Fail(hr, "接收窗口初始化"); return S_OK; }
                                const HRESULT navigation = state->view->NavigateToString(html.c_str());
                                if (FAILED(navigation)) state->Fail(navigation, "打开界面");
                                return S_OK;
                            }).Get());
                        if (FAILED(hr)) state->Fail(hr, "接收窗口初始化");
                    } else {
                        hr = state->view->NavigateToString(html.c_str());
                        if (FAILED(hr)) state->Fail(hr, "打开界面");
                    }
                    return S_OK;
                }).Get());
            if (FAILED(create)) state->Fail(create, "创建浏览器窗口");
            return S_OK;
        }).Get());
    if (FAILED(result)) state->Fail(result, "创建浏览器环境");
    return SUCCEEDED(result);
}

void WebViewHost::Resize() {
    const auto state = state_;
    if (state) state->Resize();
}

void WebViewHost::Post(const std::string& json) {
    const auto state = state_;
    if (!state || !state->Active() || state->failed || !state->view || json.size() > 4 * 1024 * 1024) return;
    const auto wide = Wide(json);
    if (!wide.empty()) state->view->PostWebMessageAsJson(wide.c_str());
}

void WebViewHost::Execute(const std::wstring& javascript) {
    const auto state = state_;
    if (state && state->Active() && !state->failed && state->view)
        state->view->ExecuteScript(javascript.c_str(), nullptr);
}

bool WebViewHost::Ready() const { return state_ && state_->Active() && !state_->failed && state_->ready; }

void WebViewHost::Capture(const std::filesystem::path& png, std::function<void(bool)> done) {
    const auto state = state_;
    if (!state || !Ready()) { if (done) done(false); return; }
    ComPtr<IStream> stream;
    const HRESULT open = SHCreateStreamOnFileEx(png.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
    if (FAILED(open)) { if (done) done(false); return; }
    // Keep the stream alive until WebView2 has finished writing. Suppress owner callbacks
    // after Close, since a caller's callback may refer to its destroyed window/controller.
    const std::weak_ptr<State> weak = state;
    const auto completion = std::make_shared<std::function<void(bool)>>(std::move(done));
    const HRESULT capture = state->view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
        [weak, stream, completion](HRESULT hr) mutable -> HRESULT {
            if (SUCCEEDED(hr)) hr = stream->Commit(STGC_DEFAULT);
            stream.Reset();
            auto state = weak.lock();
            if (state && state->Active() && *completion) {
                const auto callback = std::move(*completion);
                callback(SUCCEEDED(hr));
            }
            return S_OK;
        }).Get());
    if (FAILED(capture) && state->Active() && *completion) {
        const auto callback = std::move(*completion);
        callback(false);
    }
}

void WebViewHost::Close() {
    const auto state = std::move(state_);
    if (!state) return;
    state->closed = true;
    state->ready = false;
    state->parent = nullptr;
    state->onMessage = {};
    state->onError = {};
    if (state->controller) state->controller->Close();
    state->view.Reset();
    state->controller.Reset();
    state->environment.Reset();
}

}
