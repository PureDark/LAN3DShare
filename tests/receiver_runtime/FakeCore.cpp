#include <windows.h>
#include "TestState.h"
namespace { ReceiverCoreState state; int contextToken; }
extern "C" __declspec(dllexport) void* ReceiverCreateContext(int mode) {
    if (mode != 5) return nullptr; // NonBlockingClientMode; never connect to hardware.
    ++state.created;
    return &contextToken;
}
extern "C" __declspec(dllexport) void ReceiverInitializeContext(void* context) {
    if (context == &contextToken) ++state.initialized;
}
extern "C" __declspec(dllexport) void ReceiverDeleteContext(void* context) {
    if (context == &contextToken) ++state.destroyed;
}
extern "C" __declspec(dllexport) ReceiverCoreState ReceiverGetCoreState() { return state; }
