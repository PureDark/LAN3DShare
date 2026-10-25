#pragma once
#include "audio/AudioTelemetry.h"
#include <windows.h>
#include "nlohmann/json.hpp"

namespace lan3d::ui {
inline nlohmann::json AudioMeterModel(const audio::Telemetry& data) {
    auto level = [](audio::Level value) { return nlohmann::json{{"peak", value.peak}, {"rms", value.rms}, {"active", value.active != 0}}; };
    return {{"observedAt", GetTickCount64()}, {"receiver", data.receiver != 0}, {"enabled", data.enabled != 0}, {"ready", data.ready != 0},
        {"suspended", data.suspended != 0}, {"volume", data.volume}, {"clockCorrected", data.clockCorrected != 0},
        {"input", level(data.input)}, {"output", level(data.output)}, {"detail", data.detail},
        {"inputFrames", data.inputFrames}, {"outputFrames", data.outputFrames}, {"droppedFrames", data.droppedFrames}};
}
}
