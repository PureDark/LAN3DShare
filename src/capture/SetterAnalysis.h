#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace lan3d::capture::modern {
struct ReadPlanData;
// A description of reads, never executable code. The caller must supply the
// live interface pointer on the thread currently using the weaver.
struct ReadPlan {
    bool valid{};
    std::string diagnostic;
    bool Read(void* interfaceThis, uint64_t& out) const noexcept;
private:
    std::shared_ptr<const ReadPlanData> data_;
    friend ReadPlan AnalyzeSetter(void*, unsigned, unsigned);
};
// Win64 zero-based argument index: 1 = RDX; 4 = [entry RSP + 0x28].
// Only 4- and 8-byte values are supported. Analysis never calls the setter.
ReadPlan AnalyzeSetter(void* code, unsigned argument = 1, unsigned bytes = 8);
}
