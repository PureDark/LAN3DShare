#pragma once
#include <cstdint>
struct ReceiverCoreState { unsigned created{}, initialized{}, destroyed{}; };
struct ReceiverWeaverState {
    unsigned modernAttempts{}, modernCreated{}, modernDestroyed{};
    unsigned legacyCreated{}, legacyDestroyed{}, inputCount{}, weaveCount{};
    unsigned suppliedWidth{}, textureWidth{}, height{}, latencyFrames{};
    bool srgbRead{}, srgbWrite{};
};
