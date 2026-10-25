#include "update/Updater.h"
#include <fstream>
#include <iostream>
#include <cstdlib>

using namespace lan3d::update;
using Json = nlohmann::json;
static void Require(bool value) { if (!value) throw std::runtime_error("check failed"); }
template<class F> static void Reject(F f) { bool rejected{}; try { f(); } catch (...) { rejected=true; } Require(rejected); }
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--wait") {
            const auto event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
            if (!event) return 2;
            const auto result = WaitForSingleObject(event, 60000); CloseHandle(event);
            return result == WAIT_OBJECT_0 ? 0 : 3;
        }
        if (argc == 3 && (std::wstring(argv[1]) == L"--download" || std::wstring(argv[1]) == L"--download-api-fallback")) {
            std::atomic<bool> cancel{};
            auto release = FetchRelease(cancel);
            const auto actualVersion = release.version;
            // Force only the public route to return 404; the real API asset,
            // size and digest are retained to exercise the verified fallback.
            if (std::wstring(argv[1]) == L"--download-api-fallback") release.version = "99999.99999.99999";
            FetchArchive(release, argv[2], cancel, {});
            release.version = actualVersion;
            std::cout << Json{{"version",release.version},{"size",release.size},{"sha256",Sha256(argv[2])}}.dump(2) << '\n';
            return 0;
        }
        wchar_t* marker{}; size_t markerLength{}; _wdupenv_s(&marker,&markerLength,L"LAN3D_UPDATE_TEST_RESTART_MARKER");
        if (marker) { std::ofstream out(std::filesystem::path(marker),std::ios::binary); out << "restarted"; free(marker); }
        Require(Newer("v0.7.10", "0.7.9")); Require(Newer("1.0.0", "0.99.99"));
        Require(!Newer("0.7.8", "0.7.9")); Require(!Newer("0.7.9", "0.7.9"));
        for (auto v : {"0.7.9-beta", "0.7", "01.2.3", "1.2.3/..", "999999999.2.3"}) Reject([&]{Newer(v,"0.7.9");});
        Require(AllowedUrl(L"https://api.github.com/repos/PureDark/LAN3DShare/releases/latest"));
        Require(AllowedUrl(L"https://release-assets.githubusercontent.com/a?signature=x"));
        Release publicRelease; publicRelease.version="0.8.1";
        Require(ArchiveUrl(publicRelease)==L"https://github.com/PureDark/LAN3DShare/releases/download/v0.8.1/LAN3DShare-0.8.1-win64.zip");
        Require(AllowedUrl(ArchiveUrl(publicRelease)));
        for (auto url : {L"https://github.com/Other/LAN3DShare/releases/download/v0.8.1/LAN3DShare-0.8.1-win64.zip",
            L"https://github.com/PureDark/LAN3DShare/releases/download/v0.8.1/LAN3DShare-0.8.2-win64.zip",
            L"https://github.com/PureDark/LAN3DShare/releases/download/v0.8.1/LAN3DShare-0.8.1-symbols.zip",
            L"https://github.com/PureDark/LAN3DShare/releases/download/v0.8.1/../../other.exe"}) Require(!AllowedUrl(url));
        publicRelease.version="0.8.1/../../other"; Reject([&]{ArchiveUrl(publicRelease);});
        for (auto url : {L"http://api.github.com/repos/PureDark/LAN3DShare/releases/latest", L"https://evil.example/a", L"https://api.github.com.evil.example/a", L"https://api.github.com/repos/Other/Private/releases", L"https://api.github.com/repos/PureDark/LAN3DShare/releases-evil", L"https://user:pass@api.github.com/repos/PureDark/LAN3DShare/releases", L"https://api.github.com:8443/repos/PureDark/LAN3DShare/releases"}) Require(!AllowedUrl(url));
        Json j={{"draft",false},{"prerelease",false},{"tag_name","v0.7.10"},{"assets",Json::array({{{"name","LAN3DShare-0.7.10-win64.zip"},{"state","uploaded"},{"id",123},{"size",1024},{"digest","sha256:"+std::string(64,'a')}}})}};
        Require(ParseRelease(j.dump()).version=="0.7.10");
        auto bad=j; bad["draft"]=true; Reject([&]{ParseRelease(bad.dump());});
        bad=j; bad["prerelease"]=true; Reject([&]{ParseRelease(bad.dump());});
        bad=j; bad["assets"].push_back(bad["assets"][0]); Reject([&]{ParseRelease(bad.dump());});
        bad=j; bad["assets"][0]["digest"]=""; Reject([&]{ParseRelease(bad.dump());});
        bad=j; bad["assets"][0]["size"]=300ull*1024*1024; Reject([&]{ParseRelease(bad.dump());});
        bad=j; bad["assets"][0]["name"]="other.exe"; Reject([&]{ParseRelease(bad.dump());});
        const auto file=std::filesystem::temp_directory_path()/(L"lan3d-update-hash-"+std::to_wstring(GetCurrentProcessId()));
        { std::ofstream out(file,std::ios::binary); out << "abc"; }
        auto hash=Sha256(file); std::filesystem::remove(file);
        Require(hash=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        std::cout << "Updater version, release selection, URL boundaries and SHA256 passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
