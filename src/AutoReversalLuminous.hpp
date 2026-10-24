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

    // A stable diagnostic snapshot of the most recent detector frame/event.
    // The game/render threads consume copies; callers never inspect private
    // detector state without the mutex.
    struct DebugSnapshot {
        bool hasFrame = false;
        unsigned gameFrame = 0;
        unsigned short selfAction = 0;
        unsigned short opponentAction = 0;
        int selfHp = 0;
        int opponentHp = 0;
        int baselineOpponentHp = 0;
        bool downDetected = false;
        bool wakeupDetected = false;
        bool wakeupWindowActive = false;
        bool wakeupWindowExpired = false;
        bool cancelledByOtherAttack = false;
        bool luminousDetected = false;
        bool luminousWindowActive = false;
        bool luminousWindowExpired = false;
        bool successByHitReaction = false;
        bool successByHpDecrease = false;
        bool guardChipIgnored = false;
        bool hpDecreasedThisFrame = false;
        bool opponentGuarding = false;
        unsigned stateCode = 0; // 0=Idle, 1=Downed, 2=WakeupWindow, 3=LuminousWindow
    };

    // Returns true exactly once for each successful reversal-Luminous event.
    // This function is intended to be called once per completed BattleManager
    // process/update, after the original game update has run.
    bool Update(SokuLib::BattleManager *battleManager,
                SokuLib::CharacterManager *self,
                SokuLib::CharacterManager *opponent,
                Clock::time_point now);

    void Reset();
    DebugSnapshot GetDebugSnapshot() const;

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
    short _luminousBaselineHp = 0;
    Clock::time_point _wakeupWindowStarted{};
    Clock::time_point _luminousStarted{};
    bool _luminousEventConsumed = false;
    short _previousOpponentHp = 0;
    bool _havePreviousOpponentHp = false;
    DebugSnapshot _debug;

    bool IsDownAction(SokuLib::Action action) const;
    bool IsWakeupAction(SokuLib::Action action) const;
    bool IsHitReaction(SokuLib::Action action) const;
    bool IsGuardAction(SokuLib::Action action) const;
    bool IsNonLuminousAttackAction(SokuLib::Action action) const;
    bool IsNewGameFrame(const SokuLib::BattleManager *battleManager);
};

} // namespace OIOverlayAutoRL
