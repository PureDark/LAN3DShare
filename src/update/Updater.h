#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

namespace lan3d::update {
struct Release {
    std::string version, digest;
    uint64_t assetId{}, size{};
};
bool Newer(const std::string& candidate, const std::string& current);
Release ParseRelease(const std::string& json);
bool AllowedUrl(const std::wstring& url);
std::string Sha256(const std::filesystem::path& file);
Release FetchRelease(const std::atomic<bool>& cancel);
void FetchArchive(const Release& release, const std::filesystem::path& file,
    const std::atomic<bool>& cancel, const std::function<void(unsigned)>& progress);
class Client {
public:
    Client(std::filesystem::path executable, std::filesystem::path dataDirectory);
    ~Client();
    nlohmann::json Model() const;
    bool Busy() const;
    void Check();
    void Download();
    void Install();
    bool TakeExitRequest();
private:
    void Start(std::string stage, std::string message, std::function<void()> operation);
    void State(std::string stage, std::string message);
    std::filesystem::path executable_, data_, stage_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> cancel_{};
    bool busy_{}, exit_{};
    std::string status_{"idle"}, message_{"点击检查 GitHub 最新正式版本"};
    unsigned progress_{};
    Release release_;
};
}
