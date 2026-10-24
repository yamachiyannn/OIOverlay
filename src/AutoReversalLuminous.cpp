#include "AutoReversalLuminous.hpp"

#include <SokuLib.hpp>

namespace OIOverlayAutoRL {
namespace {

constexpr unsigned short kLuminousStrikeAction = 611;

// The detector deliberately uses the game's Action state rather than card
// inventory/weather. Action 611 is Marisa's ground 光符「ルミネスストライク」
// activation in the SokuLib action table.
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
    // Ground hitstun / crouching hitstun.
    if (IsActionInRange(action, 50, 59) ||
        IsActionInRange(action, 62, 64))
        return true;

    // Air hitstun / knockback / wall-slam / ground-slam reactions.
    if (IsActionInRange(action, 70, 78) ||
        IsActionInRange(action, 88, 89))
        return true;

    return false;
}

bool Detector::IsGuardAction(SokuLib::Action action) const
{
    // Normal high/low blockstun and air guard, plus wrong-block states.
    // These states are explicitly excluded from the HP-decrease fallback so
    // chip damage cannot create an automatic RL.
    return IsActionInRange(action, 150, 166);
}

bool Detector::IsNonLuminousAttackAction(SokuLib::Action action) const
{
    const unsigned value = static_cast<unsigned short>(action);

    // Normal attacks.
    if (IsActionInRange(action, 300, 499))
        return true;

    // Skills.
    if (IsActionInRange(action, 500, 599))
        return true;

    // Spellcard actions. 611 is specifically exempted below.
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

    // BattleManager::frameCount advances with the actual game update. This
    // prevents the worker thread / render rate from causing duplicate
    // processing of one game frame.
    if (!IsNewGameFrame(battleManager))
        return false;

    const SokuLib::Action selfAction = self->objectBase.action;
    const SokuLib::Action opponentAction = opponent->objectBase.action;

    const bool selfDown = IsDownAction(selfAction);
    const bool selfWakeup = IsWakeupAction(selfAction);
    const bool luminousStarted = static_cast<unsigned short>(selfAction) == kLuminousStrikeAction;

    // A fresh knockdown starts a new reversal opportunity.  If the character
    // remains in the static-down state, keep waiting; the 3-second window starts
    // only after the tech/wakeup transition is seen.
    if (selfDown) {
        if (_state != State::Downed) {
            _state = State::Downed;
            _luminousEventConsumed = false;
        }
        _previousSelfAction = selfAction;
        _previousOpponentAction = opponentAction;
        return false;
    }

    if (_state == State::Downed) {
        if (selfWakeup) {
            _state = State::WakeupWindow;
            _wakeupWindowStarted = now;
            _luminousEventConsumed = false;
        }
    }

    if (_state == State::WakeupWindow) {
        if (now - _wakeupWindowStarted > kWakeupWindow) {
            _state = State::Idle;
        } else {
            // Any non-Luminous attack action before Luminous invalidates this
            // reversal candidate.  This is intentionally action-based: the
            // detector does not depend on a particular keyboard layout.
            if (IsNonLuminousAttackAction(selfAction)) {
                _state = State::Idle;
            } else if (luminousStarted &&
                       static_cast<unsigned short>(_previousSelfAction) != kLuminousStrikeAction) {
                // The action transition into 611 is the one-shot "Luminous was
                // actually started" event.
                _state = State::LuminousWindow;
                _luminousStarted = now;
                _luminousBaselineHp = opponent->objectBase.hp;
                _luminousEventConsumed = false;
            }
        }
    }

    if (_state == State::LuminousWindow) {
        if (_luminousEventConsumed) {
            _state = State::Idle;
        } else if (now - _luminousStarted > kLuminousWindow) {
            _state = State::Idle;
        } else {
            const bool enteredHit =
                IsHitReaction(opponentAction) &&
                !IsHitReaction(_previousOpponentAction);

            // The fallback HP condition is deliberately gated by the opponent
            // not being in a guard action. This prevents chip damage from a
            // blocked Luminous from counting as a successful reversal.
            const bool hpDecreased =
                opponent->objectBase.hp < _luminousBaselineHp;
            const bool nonGuardHpDecrease =
                hpDecreased && !IsGuardAction(opponentAction);

            if (enteredHit || nonGuardHpDecrease) {
                _luminousEventConsumed = true;
                _state = State::Idle;
                _previousSelfAction = selfAction;
                _previousOpponentAction = opponentAction;
                return true;
            }
        }
    }

    _previousSelfAction = selfAction;
    _previousOpponentAction = opponentAction;
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
    _luminousBaselineHp = 0;
    _wakeupWindowStarted = Clock::time_point{};
    _luminousStarted = Clock::time_point{};
    _luminousEventConsumed = false;
}

} // namespace OIOverlayAutoRL
