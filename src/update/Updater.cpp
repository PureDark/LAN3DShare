#include "Updater.h"
#include "Version.h"
#include <winhttp.h>
#include <wincred.h>
#include <bcrypt.h>
#include <objbase.h>
#include <array>
#include <algorithm>
#include <utility>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <vector>

namespace lan3d::update {
namespace {
using Json = nlohmann::json;
constexpr wchar_t CredentialName[] = L"LAN3DShare/GitHubRelease/PureDark/LAN3DShare";
std::string PathText(const std::filesystem::path& p) { const auto s=p.u8string(); return std::string(s.begin(),s.end()); }
constexpr uint64_t MaxArchive = 256ull * 1024 * 1024;
std::wstring Wide(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0); if (!n && !s.empty()) throw std::runtime_error("无效的文本编码"); std::wstring r(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n); return r; }
struct Internet {
    HINTERNET value{};
    explicit Internet(HINTERNET v) : value(v) { if (!v) throw std::runtime_error("无法连接 GitHub，请检查网络或代理设置"); }
    ~Internet() { WinHttpCloseHandle(value); }
};
struct Handle { HANDLE value{}; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
std::array<unsigned, 3> Version(std::string s) {
    if (s.starts_with('v')) s.erase(0, 1);
    std::smatch m;
    if (!std::regex_match(s, m, std::regex(R"((0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4}))"))) throw std::runtime_error("不支持的版本号");
    return {unsigned(std::stoul(m[1])), unsigned(std::stoul(m[2])), unsigned(std::stoul(m[3]))};
}
std::string Token() {
    PCREDENTIALW c{};
    if (!CredReadW(CredentialName, CRED_TYPE_GENERIC, 0, &c)) return {};
    std::string token(reinterpret_cast<char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c); return token;
}
std::wstring Quote(const std::wstring& value) {
    std::wstring out = L"\""; unsigned slashes{};
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        out.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; out += ch;
    }
    out.append(slashes * 2, L'\\'); return out + L'"';
}
std::string Get(const std::wstring& initial, const std::string& token, const std::atomic<bool>& cancel,
    uint64_t limit, const std::filesystem::path& output, const std::function<void(unsigned)>& progress = {}) {
    Internet session(WinHttpOpen(L"LAN3DShare/" L"0.7.9", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
    auto url = initial;
    for (unsigned redirect = 0; redirect < 5; ++redirect) {
        if (cancel) throw std::runtime_error("已取消更新操作");
        bool authorize{}; if (!AllowedUrl(url, authorize)) throw std::runtime_error("更新下载地址不属于 GitHub");
        URL_COMPONENTS parts{sizeof(parts)}; parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw std::runtime_error("更新下载地址无效");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        auto resource = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Internet connection(WinHttpConnect(session.value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        Internet request(WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) throw std::runtime_error("无法设置安全下载策略");
        std::wstring headers = output.empty() ? L"Accept: application/vnd.github+json\r\n" : L"Accept: application/octet-stream\r\n";
        headers += L"X-GitHub-Api-Version: 2022-11-28\r\n";
        // Never forward a private repository token to a signed asset/CDN URL.
        if (authorize && !token.empty()) headers += L"Authorization: Bearer " + Wide(token) + L"\r\n";
        if (!WinHttpSendRequest(request.value, headers.c_str(), DWORD(headers.size()), nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr))
            throw std::runtime_error("GitHub 请求失败，请检查网络后重试");
        DWORD status{}, bytes = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes, nullptr)) throw std::runtime_error("无法读取 GitHub 响应");
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            bytes = 0; WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, nullptr, nullptr, &bytes, nullptr);
            if (!bytes || bytes > 32768) throw std::runtime_error("GitHub 重定向地址无效");
            std::vector<wchar_t> location(bytes / sizeof(wchar_t) + 1);
            if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, nullptr, location.data(), &bytes, nullptr)) throw std::runtime_error("无法读取下载地址");
            url = location.data(); continue;
        }
        if (status == 401 || status == 404) throw std::runtime_error("无法访问私有 Release。请填写可读取 PureDark/LAN3DShare 的 GitHub 令牌，再检查更新");
        if (status == 403 || status == 429) throw std::runtime_error("GitHub 拒绝请求或请求过于频繁，请检查令牌权限或稍后重试");
        if (status != 200) throw std::runtime_error("GitHub 暂时无法提供更新，请稍后重试");
        std::ofstream file;
        if (!output.empty()) { file.open(output, std::ios::binary | std::ios::trunc); if (!file) throw std::runtime_error("无法写入更新缓存"); }
        std::string body; std::array<char, 65536> buffer{}; uint64_t total{};
        const auto started = GetTickCount64();
        for (;;) {
            if (cancel || GetTickCount64() - started > 180000) throw std::runtime_error("下载已取消或超时，请重新下载");
            DWORD read{};
            if (!WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), &read)) throw std::runtime_error("下载中断，请重新下载");
            if (!read) break;
            total += read; if (total > limit) throw std::runtime_error("更新文件超过允许大小");
            if (output.empty()) body.append(buffer.data(), read);
            else { file.write(buffer.data(), read); if (!file) throw std::runtime_error("更新缓存写入失败，请检查剩余空间"); }
            if (progress) progress(unsigned(std::min<uint64_t>(99, total * 100 / limit)));
        }
        if (!output.empty()) { file.flush(); if (!file) throw std::runtime_error("更新缓存写入失败"); }
        return body;
    }
    throw std::runtime_error("更新下载重定向次数过多");
}
}
bool Newer(const std::string& candidate, const std::string& current) { return Version(candidate) > Version(current); }
bool AllowedUrl(const std::wstring& url, bool& authorize) {
    authorize = false;
    URL_COMPONENTS p{sizeof(p)}; p.dwHostNameLength = p.dwUrlPathLength = p.dwUserNameLength = p.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &p) || p.nScheme != INTERNET_SCHEME_HTTPS || p.nPort != 443 || p.dwUserNameLength || p.dwPasswordLength) return false;
    const std::wstring host(p.lpszHostName, p.dwHostNameLength), path(p.lpszUrlPath, p.dwUrlPathLength);
    if (host == L"api.github.com") { authorize = path == L"/repos/PureDark/LAN3DShare/releases" || path.starts_with(L"/repos/PureDark/LAN3DShare/releases/"); return authorize; }
    return host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com";
}
Release ParseRelease(const std::string& json) {
    auto j = Json::parse(json);
    if (j.value("draft", true) || j.value("prerelease", true)) throw std::runtime_error("未找到正式发布版本");
    auto tag = j.at("tag_name").get<std::string>(); Version(tag);
    Release r; r.version = tag.starts_with('v') ? tag.substr(1) : tag;
    unsigned found{};
    for (const auto& asset : j.at("assets")) if (asset.value("name", "") == "LAN3DShare-" + r.version + "-win64.zip") {
        ++found;
        if (asset.value("state", "") != "uploaded") throw std::runtime_error("更新包尚未上传完成");
        r.assetId = asset.at("id").get<uint64_t>(); r.size = asset.at("size").get<uint64_t>();
        r.digest = asset.at("digest").get<std::string>();
        if (!r.assetId || !r.size || r.size > MaxArchive || !std::regex_match(r.digest, std::regex("sha256:[0-9a-f]{64}"))) throw std::runtime_error("更新包缺少有效的大小或 SHA256 校验信息");
    }
    if (found != 1) throw std::runtime_error("此版本没有唯一的 Windows x64 更新包");
    return r;
}
std::string Sha256(const std::filesystem::path& path) {
    struct Algorithm { BCRYPT_ALG_HANDLE h{}; ~Algorithm(){if(h) BCryptCloseAlgorithmProvider(h,0);} } algorithm;
    struct Hash { BCRYPT_HASH_HANDLE h{}; ~Hash(){if(h) BCryptDestroyHash(h);} } hash;
    if (BCryptOpenAlgorithmProvider(&algorithm.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || BCryptCreateHash(algorithm.h, &hash.h, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("无法初始化文件校验");
    std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("无法读取更新文件");
    std::array<char,65536> buffer{};
    while (in) { in.read(buffer.data(), buffer.size()); if (in.gcount() && BCryptHashData(hash.h, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()),0)<0) throw std::runtime_error("文件校验失败"); }
    if (!in.eof()) throw std::runtime_error("更新文件读取失败");
    std::array<unsigned char,32> digest{}; if (BCryptFinishHash(hash.h,digest.data(),ULONG(digest.size()),0)<0) throw std::runtime_error("文件校验失败");
    constexpr char hex[]="0123456789abcdef"; std::string out; for (auto b:digest) {out+=hex[b>>4];out+=hex[b&15];} return out;
}
Release FetchRelease(const std::string& token, const std::atomic<bool>& cancel) {
    return ParseRelease(Get(L"https://api.github.com/repos/PureDark/LAN3DShare/releases/latest", token, cancel, 2*1024*1024, {}));
}
void FetchArchive(const Release& r, const std::string& token, const std::filesystem::path& path, const std::atomic<bool>& cancel, const std::function<void(unsigned)>& progress) {
    Get(L"https://api.github.com/repos/PureDark/LAN3DShare/releases/assets/"+std::to_wstring(r.assetId),token,cancel,r.size,path,progress);
    if (std::filesystem::file_size(path)!=r.size || "sha256:"+Sha256(path)!=r.digest) throw std::runtime_error("下载文件校验失败，请重新下载");
}
Client::Client(std::filesystem::path executable, std::filesystem::path data) : executable_(std::move(executable)), data_(std::move(data)), authorized_(!Token().empty()) {
    std::ifstream in(data_/L"update-result.json");
    try { if(in) { Json result;in>>result; message_=result.value("message",message_); status_=result.value("ok",false)?"idle":"error"; } } catch(...) {}
}
Client::~Client(){cancel_=true;if(worker_.joinable())worker_.join();}
bool Client::Busy() const {std::lock_guard lock(mutex_);return busy_;}
Json Client::Model() const {std::lock_guard lock(mutex_);return {{"current",LAN3D_VERSION},{"latest",release_.version},{"status",status_},{"message",message_},{"progress",progress_},{"busy",busy_},{"authorized",authorized_},{"hasUpdate",!release_.version.empty() && Newer(release_.version,LAN3D_VERSION)}};}
void Client::State(std::string status,std::string message){std::lock_guard lock(mutex_);status_=std::move(status);message_=std::move(message);}
void Client::Start(std::string status,std::string message,std::function<void()> action){
    {std::lock_guard lock(mutex_);if(busy_)return;}
    if(worker_.joinable())worker_.join();
    {std::lock_guard lock(mutex_);busy_=true;status_=std::move(status);message_=std::move(message);progress_=0;}
    try {
        worker_=std::thread([this,action=std::move(action)]{try{action();}catch(const std::exception& e){State("error",e.what());}catch(...){State("error","更新失败，请重试");}std::lock_guard lock(mutex_);busy_=false;});
    } catch (...) {
        std::lock_guard lock(mutex_);busy_=false;status_="error";message_="无法启动更新任务，请重试";
    }
}
void Client::Check(){Start("checking","正在检查 GitHub 最新版本…",[this]{
    auto r=FetchRelease(Token(),cancel_);const bool newer=Newer(r.version,LAN3D_VERSION);
    {std::lock_guard lock(mutex_);release_=std::move(r);stage_.clear();}
    State(newer?"available":"current",newer?"发现新版本，可下载后安装":"当前已是最新版本");
});}
void Client::Download(){
    Release r;{std::lock_guard lock(mutex_);if(busy_||release_.version.empty()||!Newer(release_.version,LAN3D_VERSION))return;r=release_;}
    Start("downloading","正在下载更新…",[this,r]{
        GUID g{};if(FAILED(CoCreateGuid(&g)))throw std::runtime_error("无法创建更新缓存");wchar_t name[40]{};StringFromGUID2(g,name,40);
        auto stage=data_/L"updates"/name;std::filesystem::create_directories(stage);
        {std::lock_guard lock(mutex_);stage_=stage;}
        try {
            FetchArchive(r,Token(),stage/L"package.zip",cancel_,[this](unsigned p){std::lock_guard lock(mutex_);progress_=p;});
        } catch (...) {
            std::error_code ignored;std::filesystem::remove(stage/L"package.zip",ignored);throw;
        }
        {std::lock_guard lock(mutex_);progress_=100;}
        State("ready","下载并校验完成，可以安装并重启");
    });
}
void Client::SaveToken(const std::string& token){
    if(Busy())return;
    if(token.size()<20||token.size()>256||!std::all_of(token.begin(),token.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';})) {State("error","请填写有效的 GitHub 访问令牌");return;}
    CREDENTIALW c{};c.Type=CRED_TYPE_GENERIC;c.TargetName=const_cast<wchar_t*>(CredentialName);c.CredentialBlobSize=DWORD(token.size());c.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<char*>(token.data()));c.Persist=CRED_PERSIST_LOCAL_MACHINE;c.UserName=const_cast<wchar_t*>(L"GitHub");
    if(!CredWriteW(&c,0)){State("error","无法保存到 Windows 凭据管理器");return;}
    {std::lock_guard lock(mutex_);authorized_=true;}State("idle","GitHub 凭据已保存，可以检查更新");
}
void Client::ForgetToken(){if(Busy())return;if(!CredDeleteW(CredentialName,CRED_TYPE_GENERIC,0)&&GetLastError()!=ERROR_NOT_FOUND){State("error","无法清除 GitHub 凭据");return;}{std::lock_guard lock(mutex_);authorized_=false;}State("idle","GitHub 凭据已清除");}
bool Client::TakeExitRequest(){std::lock_guard lock(mutex_);return std::exchange(exit_,false);}
void Client::Install(){
    Release r;std::filesystem::path stage;
    {std::lock_guard lock(mutex_);if(busy_||status_!="ready")return;r=release_;stage=stage_;}
    Start("installing","正在准备安装，客户端将自动重启…",[this,r,stage]{
        if("sha256:"+Sha256(stage/L"package.zip")!=r.digest)throw std::runtime_error("更新包校验失败，请重新下载");
        const auto module=GetModuleHandleW(nullptr);const auto resource=FindResourceW(module,MAKEINTRESOURCEW(202),RT_RCDATA);
        const auto loaded=resource?LoadResource(module,resource):nullptr;const auto bytes=resource?SizeofResource(module,resource):0;
        if(!loaded||!bytes)throw std::runtime_error("更新组件缺失，请重新下载完整安装包");
        {std::ofstream out(stage/L"install.ps1",std::ios::binary);out.write(static_cast<const char*>(LockResource(loaded)),bytes);if(!out)throw std::runtime_error("无法准备更新组件");}
        GUID g{};if(FAILED(CoCreateGuid(&g)))throw std::runtime_error("无法创建更新握手");wchar_t id[40]{};StringFromGUID2(g,id,40);const auto eventName=std::wstring(L"Local\\LAN3DShare.Update.")+id;
        Handle ready{CreateEventW(nullptr,TRUE,FALSE,eventName.c_str())};if(!ready.value)throw std::runtime_error("无法创建更新握手");
        FILETIME creation{},exit{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))throw std::runtime_error("无法确认客户端进程");
        const auto start=(uint64_t(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
        Json request{{"installDirectory",PathText(executable_.parent_path())},{"archive",PathText(stage/L"package.zip")},{"sha256",r.digest.substr(7)},{"version",r.version},{"parentPid",GetCurrentProcessId()},{"parentStart",start},{"readyEvent",std::string(eventName.begin(),eventName.end())},{"resultFile",PathText(data_/L"update-result.json")}};
        {std::ofstream out(stage/L"request.json",std::ios::binary);out<<request.dump(2);if(!out)throw std::runtime_error("无法写入更新请求");}
        wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);const auto ps=std::filesystem::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
        auto cmd=Quote(ps.wstring())+L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+Quote((stage/L"install.ps1").wstring())+L" -RequestFile "+Quote((stage/L"request.json").wstring());
        STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
        if(!CreateProcessW(ps.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,stage.c_str(),&si,&pi))throw std::runtime_error("无法启动更新组件");
        Handle process{pi.hProcess},thread{pi.hThread};HANDLE handles[]{ready.value,process.value};
        if(WaitForMultipleObjects(2,handles,FALSE,15000)!=WAIT_OBJECT_0){
            if(WaitForSingleObject(process.value,0)==WAIT_TIMEOUT)TerminateProcess(process.value,1);
            std::string message="无法准备安装。请检查程序目录写入权限后重试";
            try {std::ifstream in(data_/L"update-result.json");Json result;in>>result;if(!result.value("ok",true)&&result.value("version","")==r.version)message=result.value("message",message);}catch(...){}
            throw std::runtime_error(message);
        }
        {std::lock_guard lock(mutex_);exit_=true;}
    });
}
}
