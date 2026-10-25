// Drives the default shared WASAPI render endpoint with synthetic zero PCM so
// loopback integration tests do not depend on other applications making sound.
#include "audio/AudioPlayback.h"
#include "common/FrameBus.h"
#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    HANDLE stopEvent{};
    lan3d::audio::Playback playback;
    try {
        std::wstring stopName;
        unsigned seconds{15};
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("Missing silence-source option value");
            const std::wstring value = argv[++i];
            if (option == L"--stop-event") stopName = value;
            else if (option == L"--seconds") {
                size_t used{}; const auto parsed = std::stoul(value, &used);
                if (used != value.size() || parsed < 1 || parsed > 30) throw std::runtime_error("Invalid silence-source duration");
                seconds = unsigned(parsed);
            } else throw std::runtime_error("Unknown silence-source option");
        }
        if (stopName.empty()) throw std::runtime_error("Silence source requires an owned stop event");
        stopEvent = OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str());
        if (!stopEvent) throw std::runtime_error("Cannot open silence-source stop event");
        playback.Start(); playback.Reset(1);
        const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!playback.Stats().ready && std::chrono::steady_clock::now() < readyDeadline && WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!playback.Stats().ready) throw std::runtime_error("Default audio endpoint unavailable: " + playback.Status());
        std::cout << "SILENCE_READY sample_rate=48000 channels=2\n" << std::flush;
        auto next = std::chrono::steady_clock::now();
        const auto deadline = next + std::chrono::seconds(seconds);
        auto pts = lan3d::Timestamp100ns();
        while (next < deadline && WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
            lan3d::audio::Block block;
            block.timestamp100ns = pts; block.samples.resize(480 * lan3d::audio::Channels);
            playback.Push(block, 1); pts = block.End100ns(); playback.Presented(pts, 1);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
        playback.Stop();
        const auto stats = playback.Stats();
        if (!stats.submittedFrames) throw std::runtime_error("WASAPI never consumed synthetic silence");
        std::cout << "SILENCE_STOPPED submitted_frames=" << stats.submittedFrames << " max_padding_frames=" << stats.maxPaddingFrames
            << " samples=zero\n" << std::flush;
        CloseHandle(stopEvent); return 0;
    } catch (const std::exception& error) {
        playback.Stop(); if (stopEvent) CloseHandle(stopEvent);
        std::cerr << "AudioSilenceSource FAILED: " << error.what() << '\n'; return 1;
    }
}
