#pragma once

#include <SokuLib.hpp>

#include <chrono>
#include <mutex>

namespace SokuLib {
    struct BattleManager;
    struct CharacterManager;
}

namespace OIOverlayAutoRL {

class Detector {
public:
    using Clock = std::chrono::steady_clock;

    // Returns true exactly once for each successful reversal-Luminous event.
    // This function is intended to be called once per completed BattleManager
    // process/update, after the original game update has run.
    bool Update(SokuLib::BattleManager *battleManager,
                SokuLib::CharacterManager *self,
                SokuLib::CharacterManager *opponent,
                Clock::time_point now);

    void Reset();

private:
    // Update runs from BattleManager while Reset is called by the overlay thread.
    mutable std::mutex _mutex;

    enum class State {
        Idle,
        Downed,
        WakeupWindow,
        LuminousWindow
    };

    State _state = State::Idle;
    unsigned _lastGameFrame = 0;
    bool _haveGameFrame = false;
    SokuLib::Action _previousSelfAction = SokuLib::ACTION_IDLE;
    SokuLib::Action _previousOpponentAction = SokuLib::ACTION_IDLE;
    Clock::time_point _wakeupWindowStarted{};
    Clock::time_point _luminousStarted{};
    bool _luminousEventConsumed = false;
    short _previousOpponentHp = 0;
    bool _havePreviousOpponentHp = false;

    bool IsDownAction(SokuLib::Action action) const;
    bool IsWakeupAction(SokuLib::Action action) const;
    bool IsHitReaction(SokuLib::Action action) const;
    bool IsGuardAction(SokuLib::Action action) const;
    bool IsNonLuminousAttackAction(SokuLib::Action action) const;
    bool IsNewGameFrame(const SokuLib::BattleManager *battleManager);
};

} // namespace OIOverlayAutoRL
