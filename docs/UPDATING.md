# 更新与 GitHub 授权 / Updates and GitHub authorization

**首次升级到 0.7.9：**旧版没有更新入口，需结束共享、退出客户端，手动下载并覆盖一次。从此可在 **设置 → 程序更新** 中依次点击 **检查更新 → 下载更新 → 安装并重启**。共享期间不会执行更新；已有设置保留。安装包先做 SHA256 和逐文件校验，再等待客户端退出后替换，失败时尝试恢复原文件。未变化的采集 DLL 会跳过替换；如果未来更新改变了游戏正在使用的采集 DLL，请关闭对应游戏后重试。

**私有仓库首次授权：**在“GitHub 下载授权”里点击“创建访问令牌”，使用有仓库访问权的账号，仅选择 `PureDark/LAN3DShare`，授权 **Contents → Read-only**。将生成的 fine-grained token 粘贴到客户端，点击“保存授权”。令牌保存在当前 Windows 用户的凭据管理器，不写进配置、日志或发布包。每台电脑配置一次；令牌到期或被撤销时重新授权。程序目录需要当前用户可写。

![程序更新与下载授权](screenshots/updater.png)


## English

**First upgrade to 0.7.9:** older clients do not have an updater, so end sharing, exit the app and manually replace the files once. Future updates use **Settings → Program update → Check → Download → Install and restart**. Updates are blocked while sharing. Settings are preserved. The archive and individual files are verified before replacement; the helper waits for the client to exit and attempts rollback on failure. Unchanged capture DLLs are skipped. If a future release changes a DLL currently loaded by a game, close that game and retry.

**One-time private-repository authorization:** open **GitHub download authorization → Create access token**, sign in with an account that can access this repository, select only `PureDark/LAN3DShare`, and grant **Contents → Read-only**. Paste the fine-grained token and select **Save authorization**. It is stored in the current user's Windows Credential Manager, never in settings, logs or release packages. Configure each computer once; replace expired or revoked tokens. The application directory must be writable by the current user.

