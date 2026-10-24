#include "AutoReversalLuminous.hpp"

#include <SokuLib.hpp>

namespace OIOverlayAutoRL {
namespace {

constexpr unsigned short kLuminousStrikeAction = 611;
constexpr std::chrono::seconds kWakeupWindow{3};
constexpr std::chrono::seconds kLuminousWindow{1};

bool IsActionInRange(SokuLib::Action action, unsigned short first, unsigned short last)
{
    const unsigned value = static_cast<unsigned short>(action);
    return value >= first && value <= last;
}

} // namespace

bool Detector::IsDownAction(SokuLib::Action action) const
{
    return action == SokuLib::ACTION_KNOCKED_DOWN ||
           action == SokuLib::ACTION_KNOCKED_DOWN_STATIC;
}

bool Detector::IsWakeupAction(SokuLib::Action action) const
{
    return action == SokuLib::ACTION_FORWARD_TECH ||
           action == SokuLib::ACTION_BACKWARD_TECH ||
           action == SokuLib::ACTION_NEUTRAL_TECH;
}

bool Detector::IsHitReaction(SokuLib::Action action) const
{
    if (IsActionInRange(action, 50, 59) || IsActionInRange(action, 62, 64))
        return true;
    if (IsActionInRange(action, 70, 78) || IsActionInRange(action, 88, 89))
        return true;
    return false;
}

bool Detector::IsGuardAction(SokuLib::Action action) const
{
    // Guardstun, air guard and wrong-guard states. HP loss during these states
    // must not be accepted as a successful RL.
    return IsActionInRange(action, 150, 166);
}

bool Detector::IsNonLuminousAttackAction(SokuLib::Action action) const
{
    const unsigned value = static_cast<unsigned short>(action);
    if (IsActionInRange(action, 300, 499))
        return true;
    if (IsActionInRange(action, 500, 599))
        return true;
    if (IsActionInRange(action, 600, 799) && value != kLuminousStrikeAction)
        return true;
    return false;
}

bool Detector::IsNewGameFrame(const SokuLib::BattleManager *battleManager)
{
    if (!battleManager)
        return false;
    const unsigned frame = battleManager->frameCount;
    if (_haveGameFrame && frame == _lastGameFrame)
        return false;
    _lastGameFrame = frame;
    _haveGameFrame = true;
    return true;
}

bool Detector::Update(
    SokuLib::BattleManager *battleManager,
    SokuLib::CharacterManager *self,
    SokuLib::CharacterManager *opponent,
    Clock::time_point now)
{
    if (!battleManager || !self || !opponent)
        return false;

    std::lock_guard<std::mutex> lock(_mutex);
    if (!IsNewGameFrame(battleManager))
        return false;

    const SokuLib::Action selfAction = self->objectBase.action;
    const SokuLib::Action opponentAction = opponent->objectBase.action;
    const short opponentHp = opponent->objectBase.hp;
    const bool selfDown = IsDownAction(selfAction);
    const bool selfWakeup = IsWakeupAction(selfAction);
    const bool luminousStarted = static_cast<unsigned short>(selfAction) == kLuminousStrikeAction;
    const bool hpDecreasedThisFrame = _havePreviousOpponentHp && opponentHp < _previousOpponentHp;
    const bool opponentGuarding = IsGuardAction(opponentAction);

    // Test a pending spell result before handling a new down event. Both players
    // can change state in the same game tick; a simultaneous self-hit must not
    // hide a valid opponent hit reaction from the spell that was just used.
    if (_state == State::LuminousWindow && !_luminousEventConsumed) {
        if (now - _luminousStarted > kLuminousWindow) {
            _state = State::Idle;
        } else {
            const bool enteredHit = IsHitReaction(opponentAction) &&
                                    !IsHitReaction(_previousOpponentAction);
            const bool nonGuardHpDecrease = hpDecreasedThisFrame && !opponentGuarding;
            if (enteredHit || nonGuardHpDecrease) {
                _luminousEventConsumed = true;
                _state = State::Idle;
                _previousSelfAction = selfAction;
                _previousOpponentAction = opponentAction;
                _previousOpponentHp = opponentHp;
                _havePreviousOpponentHp = true;
                return true;
            }
        }
    }

    // A newly observed down state starts a fresh attempt and resets the event
    // flags for the next reversal attempt.
    if (selfDown) {
        if (_state != State::Downed) {
            _state = State::Downed;
            _luminousEventConsumed = false;
        }
        _previousSelfAction = selfAction;
        _previousOpponentAction = opponentAction;
        _previousOpponentHp = opponentHp;
        _havePreviousOpponentHp = true;
        return false;
    }

    if (_state == State::Downed && selfWakeup) {
        _state = State::WakeupWindow;
        _wakeupWindowStarted = now;
        _luminousEventConsumed = false;
    }

    if (_state == State::WakeupWindow) {
        if (now - _wakeupWindowStarted > kWakeupWindow) {
            _state = State::Idle;
        } else if (IsNonLuminousAttackAction(selfAction)) {
            // The first non-Luminous attack action invalidates the reversal
            // opportunity. This is action-based, not keyboard-layout-based.
            _state = State::Idle;
        } else if (luminousStarted &&
                   static_cast<unsigned short>(_previousSelfAction) != kLuminousStrikeAction) {
            _state = State::LuminousWindow;
            _luminousStarted = now;
            _luminousEventConsumed = false;
        }
    }

    _previousSelfAction = selfAction;
    _previousOpponentAction = opponentAction;
    _previousOpponentHp = opponentHp;
    _havePreviousOpponentHp = true;
    return false;
}

void Detector::Reset()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _state = State::Idle;
    _lastGameFrame = 0;
    _haveGameFrame = false;
    _previousSelfAction = SokuLib::ACTION_IDLE;
    _previousOpponentAction = SokuLib::ACTION_IDLE;
    _wakeupWindowStarted = Clock::time_point{};
    _luminousStarted = Clock::time_point{};
    _luminousEventConsumed = false;
    _previousOpponentHp = 0;
    _havePreviousOpponentHp = false;
}

} // namespace OIOverlayAutoRL
