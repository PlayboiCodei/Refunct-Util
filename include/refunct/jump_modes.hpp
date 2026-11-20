#pragma once
// Pure, portable jump-assist selector logic. INI values 0..2 must remain
// unchanged so existing ScrollJumpTargets/HoldJumpTargets stay compatible.
namespace refunct {

enum class JumpTargets : int {
    GroundOnly = 0,
    GroundAndWall = 1,
    WallOnly = 2,
    ClimbOnly = 3,
    GroundAndClimb = 4,
    WallAndClimb = 5,
    GroundWallAndClimb = 6,
};

constexpr int kJumpSelectorModeCount = 8; // Off + seven combinations
constexpr int kJumpGroundBit = 1;
constexpr int kJumpWallBit = 2;
constexpr int kJumpClimbBit = 4;

constexpr int JumpTargetMask(JumpTargets targets) noexcept {
    switch (targets) {
        case JumpTargets::GroundOnly: return kJumpGroundBit;
        case JumpTargets::GroundAndWall: return kJumpGroundBit | kJumpWallBit;
        case JumpTargets::WallOnly: return kJumpWallBit;
        case JumpTargets::ClimbOnly: return kJumpClimbBit;
        case JumpTargets::GroundAndClimb: return kJumpGroundBit | kJumpClimbBit;
        case JumpTargets::WallAndClimb: return kJumpWallBit | kJumpClimbBit;
        case JumpTargets::GroundWallAndClimb:
            return kJumpGroundBit | kJumpWallBit | kJumpClimbBit;
    }
    return kJumpGroundBit; // Defensive fallback for invalid saved values.
}

constexpr bool JumpAllowsGround(JumpTargets targets) noexcept {
    return (JumpTargetMask(targets) & kJumpGroundBit) != 0;
}
constexpr bool JumpAllowsWall(JumpTargets targets) noexcept {
    return (JumpTargetMask(targets) & kJumpWallBit) != 0;
}
constexpr bool JumpAllowsClimb(JumpTargets targets) noexcept {
    return (JumpTargetMask(targets) & kJumpClimbBit) != 0;
}
constexpr JumpTargets JumpTargetsFromStoredValue(int value) noexcept {
    return value >= 0 && value <= 6
        ? static_cast<JumpTargets>(value) : JumpTargets::GroundOnly;
}
constexpr int JumpSelectorMode(bool enabled, JumpTargets targets) noexcept {
    return enabled ? static_cast<int>(targets) + 1 : 0;
}
constexpr int NextJumpSelectorMode(int current, int direction) noexcept {
    const int normalized = current >= 0 && current < kJumpSelectorModeCount ? current : 0;
    return (normalized + (direction < 0 ? kJumpSelectorModeCount - 1 : 1))
        % kJumpSelectorModeCount;
}

} // namespace refunct
