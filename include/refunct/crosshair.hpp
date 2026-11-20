#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace refunct {
// Native suppression is scoped to the original HUD draw. Style IDs 0–3 remain
// compatible with existing settings and profiles; 4 hides the reticle; 5 is a custom diamond.
struct CrosshairSettings {
    int style = 0; // 0 = game default, 1 = dot, 2 = cross, 3 = cross + dot, 4 = off, 5 = diamond
    int size = 8; // arm length (or dot diameter), pixels
    int thickness = 2;
    int gap = 4;
    int opacity = 100;
    int rgb = 0xFFFFFF;
    bool operator==(const CrosshairSettings&) const = default;
};
inline CrosshairSettings g_crosshair{};
inline constexpr int kCrosshairDiamond = 5;
// Editing the default style selects an editable replacement. Keep existing
// style IDs and saved profiles intact; restoring defaults still uses style 0.
inline void EnableCrosshairEditing(CrosshairSettings& s) {
    if (s.style == 0) s.style = kCrosshairDiamond;
}
enum class CrosshairStatus { Pending, NativeFlagUnavailable, RenderUnavailable,
                             GameReticleOff, NativeFlagCleared };
inline CrosshairStatus g_crosshair_status = CrosshairStatus::Pending;
inline const wchar_t* CrosshairStatusLabel() {
    switch (g_crosshair_status) {
        case CrosshairStatus::NativeFlagUnavailable: return L" [native flag unavailable]";
        case CrosshairStatus::RenderUnavailable: return L" [renderer unavailable]";
        case CrosshairStatus::GameReticleOff: return L" [game reticle off]";
        case CrosshairStatus::NativeFlagCleared: return L" [native flag cleared]";
        default: return L" [waiting for HUD]";
    }
}
inline constexpr bool ValidCrosshair(const CrosshairSettings& s) {
    return s.style >= 0 && s.style <= 5 && s.size >= 1 && s.size <= 100 &&
           s.thickness >= 1 && s.thickness <= 20 && s.gap >= 0 && s.gap <= 50 &&
           s.opacity >= 0 && s.opacity <= 100 && s.rgb >= 0 && s.rgb <= 0xFFFFFF;
}
// UBoolProperty's descriptor is resolved from live metadata, not a guessed
// actor offset. Preserve packed neighbours and support UE's native C++ bool.
struct ReticleBits {
    int offset = -1;
    uint8_t mask = 0;
    bool native_bool = false;
    explicit operator bool() const { return offset >= 0 && mask != 0; }
};
inline constexpr ReticleBits DecodeReticleBits(int offset, const uint8_t* d) {
    if (offset < 0x1c || offset > 0x1ff8 || !d) return {};
    const uint8_t size=d[0], byte=d[1], mask=d[2], field=d[3];
    if ((size != 1 && size != 2 && size != 4 && size != 8) || byte >= size ||
        !mask || (mask & (mask - 1)) || (field != mask && field != 0xff)) return {};
    if (field == 0xff && (size != 1 || byte != 0 || mask != 1)) return {};
    return {offset + byte, mask, field == 0xff};
}
inline constexpr bool ValidReticleValue(ReticleBits bits, uint8_t value) {
    return bool(bits.mask) && (!bits.native_bool || value <= 1);
}
// Called only with a live, writable reflected field. Inactive/unsupported
// requests pass through unchanged and never draw a second crosshair.
// Reacquire validates identity after the original callback before restoration.
template<class Draw, class Reacquire>
bool DrawWithNativeReticleSuppressed(uint8_t* address, ReticleBits bits,
                                    Draw&& draw, Reacquire&& reacquire) {
    if (!address || !bits || !ValidReticleValue(bits, *address)) { draw(); return false; }
    const uint8_t mask = bits.native_bool ? uint8_t{0xff} : bits.mask;
    const uint8_t saved = *address & mask;
    if (!saved) { draw(); return false; } // Respect the game's own visibility setting.
    *address &= static_cast<uint8_t>(~mask);
    draw();
    if (auto* live = reacquire(); live == address) {
        *live = static_cast<uint8_t>((*live & ~mask) | saved);
        return true;
    }
    return false;
}
// Shared geometry, testable without Unreal or Windows. No heap allocation.
template<class Rect>
void DrawCrosshairGeometry(const CrosshairSettings& s, float x, float y, Rect&& rect) {
    if (!ValidCrosshair(s) || s.style == 0 || s.style == 4 || s.opacity == 0) return;
    const float len = static_cast<float>(s.size);
    const float t = static_cast<float>(s.thickness);
    const float gap = static_cast<float>(s.gap);
    if (s.style == kCrosshairDiamond) {
        // Pixel-aligned diamond ring. Each row has at most two non-overlapping
        // rectangles; size is the overall diameter and thickness is the border.
        // Bounded by 2*100 calls even at the maximum supported size.
        const int diameter = s.size;
        const float left = x - len * 0.5f, top = y - len * 0.5f;
        for (int row = 0; row < diameter; ++row) {
            const int inset = std::abs(2 * row + 1 - diameter) / 2;
            const int width = diameter - 2 * inset;
            const int inner = (row < s.thickness || row >= diameter - s.thickness)
                ? 0 : std::max(0, width - 2 * s.thickness);
            const float l = left + inset;
            if (inner == 0) rect(l, top + row, float(width), 1.0f);
            else {
                rect(l, top + row, t, 1.0f);
                rect(l + width - t, top + row, t, 1.0f);
            }
        }
        return;
    }
    if (s.style == 2 || s.style == 3) {
        // Non-overlapping arms even at gap=0, preventing doubled alpha.
        const float offset = std::max(gap, t * 0.5f);
        rect(x - offset - len, y - t * 0.5f, len, t);
        rect(x + offset, y - t * 0.5f, len, t);
        rect(x - t * 0.5f, y - offset - len, t, len);
        rect(x - t * 0.5f, y + offset, t, len);
    }
    if (s.style == 1 || s.style == 3) {
        const float d = s.style == 1 ? len : t;
        rect(x - d * 0.5f, y - d * 0.5f, d, d);
    }
}
} // namespace refunct
