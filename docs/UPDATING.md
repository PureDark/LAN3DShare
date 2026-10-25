# 程序更新 / App updates

在 **设置 → 程序更新** 中点击 **检查更新 → 下载更新 → 安装并重启**。程序直接访问 GitHub 的公开 Release，无需登录或填写令牌；访问失败会显示错误，可稍后重试。

没有更新入口的旧版需先手动升级一次。共享期间不会执行更新；已有设置保留。下载和解压后均会校验文件，安装失败时尝试恢复原版本。程序目录需要当前用户可写。

未变化的采集 DLL 会跳过替换；如果未来更新改变了游戏正在使用的采集 DLL，请关闭对应游戏后重试。

![程序更新](screenshots/updater.png)

从 0.8.2 起，下载优先使用公开 Release 直链，失败后尝试 API 下载。旧版若能检查更新但下载返回 403，可从 [GitHub Release](https://github.com/PureDark/LAN3DShare/releases/latest) 手动下载完整包升级一次。

## English

Version 0.8.2 prefers public Release downloads and retries through the asset API if needed. If an older client gets HTTP 403 while downloading, manually install the complete package from [GitHub Releases](https://github.com/PureDark/LAN3DShare/releases/latest) once.

Use **Settings → Program update → Check → Download → Install and restart**. The app accesses public GitHub Releases directly, without sign-in or tokens. Failed requests show an error and can be retried later.

Clients without an update button need one manual upgrade. Updates are blocked while sharing. Settings are preserved, files are verified, and installation failures attempt to restore the previous version. The app directory must be writable.

Unchanged capture DLLs are skipped. If a future release changes a DLL loaded by a game, close that game and retry.
