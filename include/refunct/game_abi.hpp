#pragma once
// Private ABI for this specific Win32 Refunct executable. Do not expose to plug-in consumers.
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refunct {
// RVAs relative to Refunct-Win32-Shipping.exe (historical Refunct TAS release).
constexpr uintptr_t kCharacterTickRva = 0x000F0870;
// TAS hooks AMyCharacter::ForcedUnCrouch as the native new-game boundary.
// Its on_new_game callback sets LevelState.level=29 to pre-raise the map.
constexpr uintptr_t kNewGameEventRva  = 0x000EF1B0;
constexpr uintptr_t kHudDrawRva       = 0x000F6710;
constexpr uintptr_t kHudDrawTextRva   = 0x00C4B600;
constexpr uintptr_t kHudDrawRectRva   = 0x00C4B570;
constexpr uintptr_t kHudTextSizeRva   = 0x00C54E00;
constexpr uintptr_t kViewportSizeRva  = 0x00DBBA80;
constexpr uintptr_t kSetCollisionRva  = 0x00A4D540;
constexpr uintptr_t kSlateKeyDownRva  = 0x0032C750;
constexpr uintptr_t kSlateKeyUpRva    = 0x0032C8F0;
constexpr uintptr_t kGlobalWorldRva   = 0x01FBF9EC;
constexpr uintptr_t kProcessEventRva  = 0x00269C80;
constexpr uintptr_t kFNameAppendRva   = 0x001C34F0;
constexpr uintptr_t kMemoryFreeRva    = 0x001410D0;
constexpr uintptr_t kAccurateTimeRva  = 0x00BFC960;
// Refunct TAS native callbacks. Unlike raw position writes, these trigger
// the game's actual button activation logic.
constexpr uintptr_t kAddBasedCharacterRva    = 0x000F96F0;
constexpr uintptr_t kRemoveBasedCharacterRva = 0x000FAA60;
constexpr uintptr_t kGlobalObjectArrayRva    = 0x01F046D0;

struct Vec3 { float x, y, z; };
struct Rotator { float pitch, yaw, roll; };
struct LinearColor { float r, g, b, a; };
struct FStringView { wchar_t* data; int32_t count, capacity; };
// Original FName uses a 32-bit-aligned 64-bit value on Windows.
struct FNameView { uint32_t index, number; };

struct SceneComponent {
    std::byte unused[0x140];
    Vec3 location;
};
struct MovementComponent {
    std::byte unused0[0xB4];
    Vec3 velocity;
    std::byte unused1[0x13C - 0xC0];
    uint8_t movement_mode;
    std::byte unused2[0x170 - 0x13D];
    float max_fly_speed;
    std::byte unused3[0x20C - 0x174];
    Vec3 acceleration;
};
struct PlayerController {
    std::byte unused[0x2D0];
    Rotator control_rotation;
};
struct MyCharacter {
    std::byte unused0[0x11C];
    SceneComponent* root_component;
    std::byte unused1[0x1B4];
    PlayerController* controller;
    std::byte unused2[0x24];
    MovementComponent* movement;
};
struct LevelState {
    int32_t level, platforms, cubes, buttons, resets;
    int32_t start_seconds;
    float start_partial_seconds;
    int32_t end_seconds;
    float end_partial_seconds;
};
struct UObjectHeader {
    void* vtable;
    int32_t flags, index;
    void* class_ptr;
    FNameView name;
    void* outer;
};
struct UFieldHeader {
    UObjectHeader object;
    void* next;
};
struct UStructHeader {
    UFieldHeader field;
    void* super_struct;
    void* children;
};
static_assert(sizeof(void*) == 4, "Refunct Utility must target Win32 (x86).");
static_assert(offsetof(MyCharacter, root_component) == 0x11C);
static_assert(offsetof(MyCharacter, controller) == 0x2D4);
static_assert(offsetof(MyCharacter, movement) == 0x2FC);
static_assert(offsetof(SceneComponent, location) == 0x140);
static_assert(offsetof(MovementComponent, movement_mode) == 0x13C);
static_assert(offsetof(MovementComponent, max_fly_speed) == 0x170);
static_assert(offsetof(MovementComponent, acceleration) == 0x20C);
static_assert(offsetof(UObjectHeader, class_ptr) == 0x0C);
static_assert(offsetof(UObjectHeader, name) == 0x10);
static_assert(offsetof(UFieldHeader, next) == 0x1C);
static_assert(offsetof(UStructHeader, children) == 0x24);

using CharacterTickFn = void(__thiscall*)(MyCharacter*, float);
using NewGameEventFn = void(__thiscall*)(MyCharacter*);
using HudDrawFn = void(__thiscall*)(void*);
using HudDrawTextFn = void(__thiscall*)(void*, const FStringView*, LinearColor,
                                      float, float, void*, float, bool);
using HudDrawRectFn = void(__thiscall*)(void*, LinearColor, float, float, float, float);
using HudTextSizeFn = void(__thiscall*)(void*, const FStringView*, float*, float*, void*, float);
using ViewportSizeFn = void(__thiscall*)(PlayerController*, int32_t*, int32_t*);
using SetCollisionFn = void(__thiscall*)(MyCharacter*, uint32_t);
using SlateKeyFn = void(__thiscall*)(void*, int32_t, uint32_t, bool);
using FNameAppendFn = void(__thiscall*)(FNameView*, FStringView*);
using MemoryFreeFn = void(__cdecl*)(void*);
using ProcessEventFn = void(__thiscall*)(void*, void*, void*);
using GetAccurateTimeFn = void(__cdecl*)(const void*, int32_t*, float*);
using BasedCharacterFn = void(__thiscall*)(void*, MyCharacter*);

// Windows (x86) layout. Validate every potentially stale pointer before use.
struct GlobalObjectItem {
    UObjectHeader* object;
    int32_t flags;
    int32_t cluster_index;
    int32_t serial_number;
};
struct GlobalObjectArray {
    int32_t first_gc_index;
    int32_t last_non_gc_index;
    int32_t max_objects_not_considered_by_gc;
    uint8_t open_for_disregard_for_gc;
    uint8_t padding[3];
    GlobalObjectItem* objects;
    int32_t max_elements;
    int32_t num_elements;
};
struct UObjectPointerArray {
    void** data;
    int32_t count;
    int32_t capacity;
};
static_assert(sizeof(GlobalObjectItem) == 16);
static_assert(offsetof(GlobalObjectArray, objects) == 0x10);
static_assert(offsetof(GlobalObjectArray, num_elements) == 0x18);
static_assert(sizeof(UObjectPointerArray) == 12);

} // namespace refunct
