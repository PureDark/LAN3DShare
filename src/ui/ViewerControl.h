#pragma once
#include <windows.h>

namespace lan3d::ui {
// Both endpoints validate the supplied HWND against their own worker/parent PID.
// No received peer text is used as a window command or executable script.
inline constexpr wchar_t ViewerWindowClass[] = L"LAN3DReceiverWindow";
inline UINT ViewerFullscreenMessage() {
    static const UINT message = RegisterWindowMessageW(L"LAN3DShare.Viewer.ShowFullscreen.v1");
    return message;
}
inline UINT ViewerSettingsMessage() {
    static const UINT message = RegisterWindowMessageW(L"LAN3DShare.Viewer.Settings.v1");
    return message;
}
inline UINT ViewerVolumeMessage() {
    static const UINT message = RegisterWindowMessageW(L"LAN3DShare.Viewer.Volume.v1");
    return message;
}
inline UINT ViewerVolumeChangedMessage() {
    static const UINT message = RegisterWindowMessageW(L"LAN3DShare.Viewer.VolumeChanged.v1");
    return message;
}
}
