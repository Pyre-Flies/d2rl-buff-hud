#include "systems/buff_tracker/timer_metadata.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <iostream>

using namespace BuffPanel::Systems::BuffTracker::Internal;

int main() {
    std::uint32_t skill{}, expiry{};
    // Actual Call to Arms lists observed on build 93847: state and expiry exist,
    // while native skill attribution is zero. skillLevel is intentionally not
    // part of TimerMetadata or ResolveTimer.
    const TimerMetadata orders{2, 32, 15792.0f, 0};
    const TimerMetadata command{2, 51, 15485.0f, 0};
    assert(ResolveTimer(orders, 32, 149, 10743, skill, expiry));
    assert(skill == 149 && expiry == 15792);
    assert(ResolveTimer(command, 51, 155, 10743, skill, expiry));
    assert(skill == 155 && expiry == 15485);
    assert(!ResolveTimer(orders, 32, 0, 10743, skill, expiry));
    assert(!ResolveTimer(orders, 51, 155, 10743, skill, expiry));

    // Custom states/skills follow the same path, with native attribution winning.
    TimerMetadata custom{2, 350, 16000.0f, 0};
    assert(ResolveTimer(custom, 350, 510, 10743, skill, expiry) && skill == 510);
    custom.skill = 511;
    assert(ResolveTimer(custom, 350, 510, 10743, skill, expiry) && skill == 511);
    assert(ResolveTimer(custom, 350, 0, 10743, skill, expiry) && skill == 511);
    custom.flags |= 0x20;
    assert(!ResolveTimer(custom, 350, 510, 10743, skill, expiry));
    custom.flags = 0; // non-temporary StatList (never expires in D2R) must be rejected
    assert(!ResolveTimer(custom, 350, 510, 10743, skill, expiry));
    for (std::uint32_t badFlag : {StatListSuppressedFlag, StatListDisabledFlag, StatListExtendedFlag}) {
        custom.flags = StatListTempFlag | badFlag;
        assert(!ResolveTimer(custom, 350, 510, 10743, skill, expiry));
    }
    custom.flags = 2;
    for (float invalid : {0.0f, 10743.0f, 10742.0f, 16000.5f, 4294967296.0f,
            std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
            static_cast<float>(10743 + MaximumTimerFrames + 100)}) {
        custom.expireFrameFloat = invalid;
        assert(!ResolveTimer(custom, 350, 510, 10743, skill, expiry));
        assert(expiry == 0);
    }

    // Expiry is the native deadline: advancing the clock consumes the timer,
    // while a later native recast deadline renews it.
    assert(ResolveTimer(orders, 32, 149, 15791, skill, expiry) && expiry == 15792);
    assert(!ResolveTimer(orders, 32, 149, 15792, skill, expiry));
    auto renewed = orders;
    renewed.expireFrameFloat = 18000;
    assert(ResolveTimer(renewed, 32, 149, 15792, skill, expiry) && expiry == 18000);
    std::cout << "PASS: CTA fallback, native attribution, expiry and rejection guards; no skillLevel dependency\n";
}
