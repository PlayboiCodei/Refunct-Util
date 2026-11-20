#include "refunct/crosshair.hpp"
#include "refunct/internal_api.hpp"
#include "refunct/performance.hpp"

namespace refunct {
namespace {
float HudTextWidth(void* hud, const std::wstring& value, float scale) {
    const float fallback = static_cast<float>(value.size()) * 9.5f * scale;
    if (!g_text_size || value.empty() || value.size() >= 200) return fallback;
    struct Width { void* hud = nullptr; std::wstring text; float scale = 0; float width = 0; };
    static thread_local std::array<Width, 256> cache{};
    static thread_local ULONGLONG epoch = 0;
    const auto now = GetTickCount64();
    if (!epoch || now - epoch >= 2000) {
        for (auto& entry : cache) entry.hud = nullptr;
        epoch = now;
    }
    size_t hash = reinterpret_cast<uintptr_t>(hud) >> 4;
    for (wchar_t c : value) hash = hash * 16777619u ^ static_cast<size_t>(c);
    uint32_t bits = 0;
    std::memcpy(&bits, &scale, sizeof(bits));
    auto& entry = cache[(hash ^ bits) % cache.size()];
    if (entry.hud == hud && entry.scale == scale && entry.text == value) return entry.width;
    FStringView src{const_cast<wchar_t*>(value.c_str()),
                    static_cast<int32_t>(value.size() + 1),
                    static_cast<int32_t>(value.size() + 1)};
    float w = 0.0f, h = 0.0f;
    g_text_size(hud, &src, &w, &h, nullptr, scale);
    if (!std::isfinite(w) || w <= 0.0f || w >= 10000.0f) return fallback;
    entry = {hud, value, scale, w};
    return w;
}
} // namespace
void DrawText(void* hud, const std::wstring& text, float x, float y, float scale,
              LinearColor color) {
    if (!g_draw_text || text.size() > 32767) return;
    FStringView value{const_cast<wchar_t*>(text.c_str()),
                      static_cast<int32_t>(text.size() + 1),
                      static_cast<int32_t>(text.size() + 1)};
    g_draw_text(hud, &value, color, x, y, nullptr, scale, false);
}

void DrawCenteredText(void* hud, const std::wstring& value, float center_x,
                      float y, float scale, LinearColor color) {
    if (value.empty()) return;
    const float width = HudTextWidth(hud, value, scale);
    const float x = center_x - width * 0.5f;
    // Light text shadow; avoids an intrusive opaque popup panel.
    DrawText(hud, value, x + 1.0f, y + 2.0f, scale, {0.02f, 0.03f, 0.05f, color.a * 0.7f});
    DrawText(hud, value, x, y, scale, color);
}

namespace {
// A manually entered PB can contain only RunTime. It is a legitimate
// cumulative comparison at the finish, but never a substitute for an
// intermediate missing button timestamp or a per-button segment.
double ActivePBRecordedTimeAtButton(int button, RecordTimeView view) {
    const double recorded = ProfileRecordedTimeAtButton(
        g_pb.button_seconds, g_pb.best_segment_seconds, button,
        ProfileCompareMode::PersonalBest, view);
    if (std::isfinite(recorded) && recorded >= 0.0 && recorded <= 86400.0)
        return recorded;
    if (view == RecordTimeView::Total && button == kRefunctButtonCount &&
        std::isfinite(g_pb.total_seconds) &&
        g_pb.total_seconds >= 0.0 && g_pb.total_seconds <= 86400.0)
        return g_pb.total_seconds;
    return -1.0;
}

LinearColor DeltaColor(DeltaDirection direction, float alpha) {
    switch (direction) {
        case DeltaDirection::Ahead: return {0.47f, 0.95f, 0.64f, alpha}; // green
        case DeltaDirection::Behind: return {1.0f, 0.27f, 0.25f, alpha}; // red
        case DeltaDirection::Even:
        case DeltaDirection::Invalid: return {0.60f, 0.64f, 0.69f, alpha}; // neutral grey
    }
    return {0.60f, 0.64f, 0.69f, alpha};
}

// The supplied cooked BP_Settings and BP_MyGameInstance assets import
// /Script/Refunct.MyHUD.ShowReticle as a BoolProperty; BP_MyHUD assigns
// /Game/Materials/UI/M_Player_Crosshair to ReticleMaterial. Resolve that
// exact property on the live HUD rather than guessing a native actor offset.
struct ReticleObjectId {
    void* object = nullptr;
    void* klass = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    bool operator==(const ReticleObjectId&) const = default;
};
ReticleObjectId CrosshairIdentity(void* object) {
    if (!g_game_module || !IsReadable(object, sizeof(UObjectHeader))) return {};
    const auto* header = static_cast<const UObjectHeader*>(object);
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements <= 0 || objects->num_elements > 500000 ||
        objects->max_elements < objects->num_elements || header->index < 0 ||
        header->index >= objects->num_elements || (static_cast<uint32_t>(header->flags) & 0x30u)) return {};
    const auto* slot = objects->objects + header->index;
    if (!IsReadable(slot, sizeof(*slot)) || slot->object != object ||
        (static_cast<uint32_t>(slot->flags) & 0x30000000u)) return {};
    return {object, header->class_ptr, header->index, slot->serial_number};
}
std::wstring ReticleName(void* object) {
    return IsReadable(object, sizeof(UObjectHeader)) ?
        UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
ReticleBits ResolveNativeReticle(void* klass, std::wstring& reason) {
    // Require the native declaring class; do not accidentally edit a Blueprint
    // property or the separate persisted MyGameUserSettings.bShowReticle.
    void* owner = klass;
    for (int d=0; d<16 && IsReadable(owner, sizeof(UStructHeader)); ++d) {
        if (ReticleName(owner) == L"MyHUD") break;
        owner = static_cast<const UStructHeader*>(owner)->super_struct;
    }
    if (ReticleName(owner) != L"MyHUD") { reason = L"HUD class is " + ReticleName(klass) + L"; MyHUD not found"; return {}; }
    const auto* native_class = static_cast<const UObjectHeader*>(owner);
    if (ReticleName(native_class->outer) != L"/Script/Refunct") { reason = L"MyHUD outer is " + ReticleName(native_class->outer); return {}; }
    void* property = FindCachedClassField(owner, L"ShowReticle", 4096);
    if (!IsReadable(property, sizeof(UFieldHeader))) { reason = L"ShowReticle property not found"; return {}; }
    const auto* header = static_cast<const UObjectHeader*>(property);
    if (header->outer != owner || ReticleName(header->class_ptr) != L"BoolProperty") { reason = L"ShowReticle property owner/type mismatch"; return {}; }
    const int offset = FindClassPropertyOffset(owner, L"ShowReticle");
    ReticleBits selected{};
    int count = 0;
    bool conflict = false;
    reason = L"ShowReticle offset=" + std::to_wstring(offset);
    // Same candidate descriptor layouts already used by the water/world code.
    // Accept only candidates that agree on the target byte and mask; conflicts fail open.
    for (const size_t layout : {0x50u,0x54u,0x58u,0x5cu,0x60u,0x64u}) {
        if (!IsReadable(property, layout+4)) continue;
        const auto* bytes = static_cast<const uint8_t*>(property)+layout;
        reason += L"; +" + std::to_wstring(layout) + L"=[" +
            std::to_wstring(bytes[0]) + L"," + std::to_wstring(bytes[1]) + L"," +
            std::to_wstring(bytes[2]) + L"," + std::to_wstring(bytes[3]) + L"]";
        const auto bits = DecodeReticleBits(offset, bytes);
        if (!bits) continue;
        if (count && (selected.offset != bits.offset || selected.mask != bits.mask ||
                      selected.native_bool != bits.native_bool)) conflict = true;
        selected = bits;
        ++count;
    }
    return count && !conflict ? selected : ReticleBits{};
}
struct ReticleTarget { ReticleObjectId id{}; ReticleBits bits{}; };
ReticleTarget NativeReticleTarget(void* hud) {
    // A single cache, invalidated by object/class/index/serial changes. Failed
    // metadata discovery is retried at most once per second, not every frame.
    static thread_local ReticleTarget cached{};
    static thread_local ULONGLONG next_retry=0;
    static thread_local int last_report=-1;
    const auto id = CrosshairIdentity(hud);
    if (!id.object) {
        if (last_report != 2) {
            last_report=2;
            Log(L"Crosshair: HUD object identity not validated in registry");
        }
        return {};
    }
    if (cached.id != id) { cached={id,{}}; next_retry=0; last_report=-1; }
    const auto now = GetTickCount64();
    std::wstring reason;
    if (!cached.bits && now >= next_retry) {
        cached.bits = ResolveNativeReticle(id.klass, reason);
        next_retry = now + 1000;
    }
    const int status = cached.bits ? 1 : 0;
    if (status != last_report) {
        last_report = status;
        if (cached.bits) {
            const auto message = L"Crosshair: native MyHUD.ShowReticle resolved; byte offset=" +
                std::to_wstring(cached.bits.offset) + L" mask=" + std::to_wstring(cached.bits.mask);
            Log(message.c_str());
        } else {
            Log((L"Crosshair: native flag unresolved: " + reason).c_str());
            Notify(L"Crosshair unavailable: native reticle not resolved (see log)");
        }
    }
    return cached;
}
uint8_t* NativeReticleAddress(const ReticleTarget& target) {
    if (!target.bits || !target.id.object || CrosshairIdentity(target.id.object) != target.id) return nullptr;
    auto* byte = static_cast<uint8_t*>(target.id.object) + target.bits.offset;
    return IsReadable(byte,1) && IsWritable(byte,1) && ValidReticleValue(target.bits,*byte) ? byte : nullptr;
}

} // namespace

void __fastcall HudDrawHook(void* hud, void* /*edx*/) {
    if (!hud || !g_game_data_valid) {
        if (g_original_hud) g_original_hud(hud);
        return;
    }
    const CrosshairSettings crosshair = g_crosshair;
    const bool custom_crosshair = ValidCrosshair(crosshair) && crosshair.style != 0;
    if (!custom_crosshair) g_crosshair_status = CrosshairStatus::Pending;
    if (!custom_crosshair && g_original_hud) g_original_hud(hud);
    // Lay out independent persistent HUD elements in their selected placements.
    // Split popups, PB notices and the keyboard menu keep their own locations.
    int32_t viewport_width = 1280, viewport_height = 720;
    bool crosshair_viewport_valid = false;
    const auto read_viewport = [&] {
        if (g_viewport_size && g_player && IsReadable(g_player, sizeof(MyCharacter)) &&
            IsReadable(g_player->controller, sizeof(PlayerController))) {
            int32_t w = 0, h = 0;
            g_viewport_size(g_player->controller, &w, &h);
            if (w >= 320 && w <= 16384 && h >= 240 && h <= 16384) {
                viewport_width = w;
                viewport_height = h;
                crosshair_viewport_valid = true;
            }
        }
    };
    // Preserve the normal HUD callback/timing path when using game default.
    if (custom_crosshair) read_viewport();
    bool draw_replacement = false;
    if (custom_crosshair && g_original_hud) {
        const bool hiding = crosshair.style == 4 || crosshair.opacity == 0;
        const auto target = (hiding || (crosshair_viewport_valid && g_draw_rect)) ?
            NativeReticleTarget(hud) : ReticleTarget{};
        auto* native_byte = NativeReticleAddress(target);
        if (!hiding && (!crosshair_viewport_valid || !g_draw_rect))
            g_crosshair_status = CrosshairStatus::RenderUnavailable;
        else if (!native_byte) g_crosshair_status = CrosshairStatus::NativeFlagUnavailable;
        else if (!(*native_byte & target.bits.mask)) g_crosshair_status = CrosshairStatus::GameReticleOff;
        else g_crosshair_status = CrosshairStatus::Pending;
        // Only draw the new crosshair after the native flag was actually
        // cleared for this invocation, and restored on the very same HUD.
        const bool was_visible = DrawWithNativeReticleSuppressed(native_byte, target.bits,
            [&] { g_original_hud(hud); }, [&] { return NativeReticleAddress(target); });
        if (was_visible) g_crosshair_status = CrosshairStatus::NativeFlagCleared;
        draw_replacement = was_visible && !hiding;
    }
    PerformanceScope perf_hud(PerfArea::Hud);
    if (!custom_crosshair) read_viewport();
    // HUD callback frequency approximates rendered FPS. Measure using the
    // monotonic Windows wall clock, not Refunct's time-dilated run timer.
    static ULONGLONG fps_window_start = 0;
    static uint32_t fps_frames = 0;
    static int displayed_fps = -1;
    const ULONGLONG fps_now = GetTickCount64();
    if (!g_show_fps) {
        fps_window_start = 0;
        fps_frames = 0;
        displayed_fps = -1;
    } else if (fps_window_start == 0 || fps_now < fps_window_start) {
        fps_window_start = fps_now;
        fps_frames = 0;
        displayed_fps = -1;
    } else {
        ++fps_frames;
        const ULONGLONG elapsed = fps_now - fps_window_start;
        if (elapsed >= 750) {
            const double measured = 1000.0 * static_cast<double>(fps_frames) /
                                    static_cast<double>(elapsed);
            displayed_fps = static_cast<int>(std::clamp(measured, 0.0, 9999.0) + 0.5);
            fps_window_start = fps_now;
            fps_frames = 0;
        }
    }
    const float center_x = static_cast<float>(viewport_width) * 0.5f;
    // Render the ghost from the current native run time, behind menu/HUD text.
    DrawPBGhost(hud, viewport_width, viewport_height);
    // The original reticle was suppressed during this frame; replacement only.
    if (draw_replacement && crosshair_viewport_valid && g_draw_rect) {
        constexpr float unit = 1.0f / 255.0f;
        const auto rgb = static_cast<uint32_t>(crosshair.rgb);
        const LinearColor colour{float((rgb >> 16) & 255) * unit,
            float((rgb >> 8) & 255) * unit, float(rgb & 255) * unit,
            float(crosshair.opacity) / 100.0f};
        DrawCrosshairGeometry(crosshair, center_x, viewport_height * 0.5f,
            [&](float x, float y, float w, float h) { g_draw_rect(hud, colour, x, y, w, h); });
    }

    struct HudLine {
        std::wstring text;
        float scale;
        float advance;
        LinearColor color;
        bool module_header = false;
        bool module_detail = false;
        bool warning_background = false;
        int key_row = -1; // -1 = ordinary text, 0..2 = interactive keyboard row.
    };
    // Retain vector capacity between rendered frames, while rebuilding all live text.
    static thread_local std::array<std::vector<HudLine>, kHudPlacementCount> corners{};
    for (auto& corner : corners) corner.clear();
    const auto styled_color = [&](size_t index, LinearColor original) -> LinearColor {
        const auto mode = g_hud_colour_modes[index];
        if (mode == 0 || mode >= kHudColourModeCount) return original;
        uint32_t rgb = 0xFFFFFF;
        switch (mode) {
            case 1: rgb = 0xFFFFFF; break; // White
            case 2: rgb = 0x55DDFF; break; // Cyan
            case 3: rgb = 0x65EF88; break; // Green
            case 4: rgb = 0xFFE166; break; // Yellow
            case 5: rgb = 0xFF5050; break; // Red
            case 6: rgb = 0xFF9944; break; // Orange
            case 7: rgb = 0xFF70CC; break; // Pink
            case 8: rgb = 0x6699FF; break; // Blue
            case 9: rgb = 0xBB88FF; break; // Purple
            case 10: rgb = g_hud_custom_colours[index] & 0xFFFFFFu; break;
            default: return original;
        }
        constexpr float unit = 1.0f / 255.0f;
        return {static_cast<float>((rgb >> 16) & 0xFF) * unit,
                static_cast<float>((rgb >> 8) & 0xFF) * unit,
                static_cast<float>(rgb & 0xFF) * unit,
                original.a}; // Retain the original opacity.
    };
    const auto add_line = [&](HudElement element, std::wstring text, float scale,
                              float advance, LinearColor color,
                              bool module_header = false, bool module_detail = false,
                              bool warning_background = false, int key_row = -1) {
        const size_t index = static_cast<size_t>(element);
        if (index >= kHudElementCount || text.empty()) return;
        // A header is only presentation. Never alter a value or record here.
        if (!g_hud_show_headers[index]) {
            if (module_header) return; // List details remain visible.
            if (!module_detail) {
                if (element == HudElement::PracticeStatus) {
                    const std::wstring prefix = L"PRACTICE  ";
                    if (text.rfind(prefix, 0) == 0) text.erase(0, prefix.size());
                } else if (element != HudElement::Coordinates &&
                           element != HudElement::Timer &&
                           element != HudElement::RecordStatus) {
                    // The profile rows contain a dynamic context in parentheses;
                    // their label ends at the "): " immediately before the value.
                    size_t delimiter = text.find(L"): ");
                    if (delimiter != std::wstring::npos)
                        text.erase(0, delimiter + 3);
                    else {
                        delimiter = text.find(L": ");
                        if (delimiter != std::wstring::npos)
                            text.erase(0, delimiter + 2);
                    }
                }
            }
        } else if (element == HudElement::Timer) {
            text = L"Run timer: " + text;
        } else if (element == HudElement::Coordinates) {
            text = L"Position: " + text;
        } else if (element == HudElement::RecordStatus) {
            text = L"Record status: " + text;
        }
        const size_t corner = static_cast<size_t>(g_hud_corners[index]);
        if (corner >= corners.size()) return;
        const int saved_size = g_hud_size_percent[index];
        const float size_factor = static_cast<float>(
            saved_size >= kHudMinSizePercent && saved_size <= kHudMaxSizePercent
                ? saved_size : 100) / 100.0f;
        corners[corner].push_back({std::move(text), scale * size_factor,
                                   advance * size_factor, styled_color(index, color),
                                   module_header, module_detail, warning_background, key_row});
    };

    if (g_show_timer)
        add_line(HudElement::Timer, TimerText(), 1.15f, 29.0f,
                 {0.43f, 0.87f, 1.0f, 1.0f});
    const auto two_digits = [](unsigned number) -> std::wstring {
        return (number < 10 ? L"0" : L"") + std::to_wstring(number);
    };
    if (g_show_game_time_hud) {
        const int minute = GetLiveGameTimeMinutes();
        const std::wstring display = minute >= 0 && minute < 1440
            ? two_digits(static_cast<unsigned>(minute / 60)) + L":" +
              two_digits(static_cast<unsigned>(minute % 60))
            : L"N/A";
        add_line(HudElement::GameTime, L"Game clock: " + display,
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_local_time_hud) {
        // Query Windows once per display update instead of once per HUD frame.
        static ULONGLONG next_local_update = 0;
        static std::wstring cached_local_time;
        if (cached_local_time.empty() || fps_now >= next_local_update) {
            SYSTEMTIME local_time{};
            GetLocalTime(&local_time);
            cached_local_time = two_digits(local_time.wHour) + L":" +
                two_digits(local_time.wMinute) + L":" +
                two_digits(local_time.wSecond);
            next_local_update = fps_now + 250;
        }
        add_line(HudElement::LocalTime, L"PC time: " + cached_local_time,
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_pb_run_hud) {
        const std::wstring value = std::isfinite(g_pb.total_seconds) &&
            g_pb.total_seconds >= 0.0 && g_pb.total_seconds <= 86400.0
            ? FormatSplitTime(g_pb.total_seconds) : L"N/A";
        add_line(HudElement::PBRun, L"Full-run PB: " + value, 0.98f, 28.0f,
                 {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_sum_of_best_hud) {
        const SumOfBest sob = CalculateSumOfBest();
        const std::wstring value = sob.recorded > 0 &&
            std::isfinite(sob.seconds) && sob.seconds <= 86400.0
            ? FormatSplitTime(sob.seconds) + L" (" +
              std::to_wstring(sob.recorded) + L"/" +
              std::to_wstring(kRefunctButtonCount) + L")"
            : L"N/A";
        add_line(HudElement::SumOfBest, L"Sum of Best: " + value, 0.98f, 28.0f,
                 {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_last_button_split_hud) {
        const std::wstring value = g_button_splits.empty() ||
            !std::isfinite(g_button_splits.back().run_seconds) ||
            g_button_splits.back().run_seconds < 0.0 ||
            g_button_splits.back().run_seconds > 86400.0
            ? L"N/A"
            : ButtonLabel(g_button_splits.back().button_number) + L"  " +
              FormatSplitTime(g_button_splits.back().run_seconds);
        add_line(HudElement::LastButtonSplit, L"Run time at last button: " + value,
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_compare_hud) {
        // Match the Best/PB HUDs: show the selected comparison profile's
        // stored time, not a signed difference against the current run.
        const int button = g_button_splits.empty()
            ? 0 : g_button_splits.back().button_number;
        const bool has_button = button >= 1 && button <= kRefunctButtonCount;
        const std::wstring button_label = has_button ? ButtonLabel(button) : L"--";
        const std::wstring description = g_compare_profile.empty()
            ? L"No profile"
            : g_compare_profile + (g_compare_mode == ProfileCompareMode::PersonalBest
                ? L" PB" : L" Best");
        std::wstring value = L"N/A";
        if (has_button && g_compare_reference_loaded && !g_compare_profile.empty() &&
            _wcsicmp(g_compare_profile.c_str(), g_active_profile.c_str()) != 0) {
            const double reference = g_compare_hud_view == RecordTimeView::Total
                ? ComparisonReferenceSeconds(button)
                : ProfileRecordedTimeAtButton(
                    g_compare_pb.button_seconds, g_compare_pb.best_segment_seconds,
                    button, g_compare_mode, RecordTimeView::Segment);
            if (reference >= 0.0) value = FormatSplitTime(reference);
        }
        add_line(HudElement::CompareTime,
                 std::wstring(g_compare_hud_view == RecordTimeView::Total
                     ? L"Other profile total (" : L"Other profile button (") +
                 button_label + L", " + description + L"): " + value,
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    // Show stored times for the last completed button in the ACTIVE profile.
    // Each HUD independently selects a cumulative or per-segment record.
    // No run split yet means the reference button is unknown (never guess 1).
    const int last_split_button = g_button_splits.empty()
        ? 0 : g_button_splits.back().button_number;
    const bool has_split_button = last_split_button >= 1 &&
                                  last_split_button <= kRefunctButtonCount;
    const std::wstring split_label = has_split_button
        ? ButtonLabel(last_split_button) : L"--";
    if (g_show_best_segment_time_hud) {
        std::wstring value = L"N/A";
        if (has_split_button) {
            const double best = ProfileRecordedTimeAtButton(
                g_pb.button_seconds, g_pb.best_segment_seconds, last_split_button,
                ProfileCompareMode::BestSegments, g_best_segment_time_view);
            if (best >= 0.0) value = FormatSplitTime(best);
        }
        add_line(HudElement::BestSegmentTime,
                 (g_best_segment_time_view == RecordTimeView::Total ? L"Best sum to button (" : L"Fastest button (") +
                 split_label + L"): " + value,
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_pb_split_time_hud) {
        std::wstring value = L"N/A";
        if (has_split_button) {
            const double best = ActivePBRecordedTimeAtButton(
                last_split_button, g_pb_split_time_view);
            if (best >= 0.0) value = FormatSplitTime(best);
        }
        add_line(HudElement::PBSplitTime,
                 (g_pb_split_time_view == RecordTimeView::Total ? L"PB total to button (" : L"PB button time (") +
                 split_label + L"): " + value,
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_button_number) {
        // Read the native button count cached by UpdateButtonSplits(), not the
        // split-history vector (which can skip events during practice/reloads).
        const bool valid = g_button_state_initialized &&
            g_last_button_count >= 0 && g_last_button_count <= kRefunctButtonCount;
        const std::wstring value = valid
            ? std::to_wstring(g_last_button_count) : L"--";
        add_line(HudElement::ButtonNumber,
                 L"Current button: " + value + L"/" +
                 std::to_wstring(kRefunctButtonCount),
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_key_display) {
        // A persistent, read-only key visualiser. No game input, movement,
        // split records or cheat state is modified by this feature.
        add_line(HudElement::KeyDisplay, L"Pressed keys", 0.92f, 26.0f,
                 {0.63f, 0.91f, 1.0f, 1.0f}, true);
        for (int row = 0; row < 3; ++row) {
            // Reserve true keycap dimensions in the normal HUD layout, so
            // all positions (including Center) and font scaling stay intact.
            add_line(HudElement::KeyDisplay, L"[WASD]", 1.0f, 32.0f,
                     {0.44f, 0.90f, 1.0f, 1.0f}, false, false, false, row);
        }
    }
    if (g_show_coordinates) {
        wchar_t position[160]{};
        swprintf_s(position, L"X %.1f   Y %.1f   Z %.1f",
                   g_position.x, g_position.y, g_position.z);
        add_line(HudElement::Coordinates, position, 0.98f, 28.0f,
                 {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (g_show_speed) {
        wchar_t speed_text[80]{};
        swprintf_s(speed_text, L"Speed: %.0f u/s",
                   static_cast<double>(g_speed_units_per_second));
        add_line(HudElement::Speed, speed_text, 0.98f, 28.0f,
                 {0.65f, 0.95f, 0.73f, 1.0f});
    }
    if (g_progress_counters.valid && g_cube_display_mode != ProgressDisplayMode::Off) {
        const std::wstring denominator = g_cube_display_mode == ProgressDisplayMode::Loaded
            ? (g_progress_counters.loaded_valid
                ? std::to_wstring(g_progress_counters.loaded_cubes) : L"?")
            : std::to_wstring(kTotalCubes);
        add_line(HudElement::Cubes,
                 L"Cubes collected: " + std::to_wstring(g_progress_counters.cubes) +
                 L"/" + denominator, 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_progress_counters.valid && g_tile_display_mode != ProgressDisplayMode::Off) {
        const std::wstring denominator = g_tile_display_mode == ProgressDisplayMode::Loaded
            ? (g_progress_counters.loaded_valid
                ? std::to_wstring(g_progress_counters.loaded_tiles) : L"?")
            : std::to_wstring(kTotalColourableTiles);
        add_line(HudElement::Tiles,
                 L"Tiles coloured: " + std::to_wstring(g_progress_counters.tiles) +
                 L"/" + denominator, 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_completion_percent && g_progress_counters.valid) {
        // The native LevelState counters already track every coloured tile and
        // collected cube. Use fixed whole-map totals: no actor enumeration,
        // guessed offsets, or hooks in the HUD drawing path.
        const int completed = g_progress_counters.tiles + g_progress_counters.cubes;
        constexpr int total = kTotalColourableTiles + kTotalCubes;
        const int percent = std::clamp((completed * 100 + total / 2) / total, 0, 100);
        add_line(HudElement::CompletionPercent,
                 L"Completion: " + std::to_wstring(percent) + L"%",
                 0.98f, 28.0f, {0.91f, 0.94f, 1.0f, 1.0f});
    }
    if (g_show_fps) {
        const std::wstring value = displayed_fps >= 0
            ? std::to_wstring(displayed_fps) : L"...";
        add_line(HudElement::FPS, L"FPS: " + value, 0.98f, 28.0f,
                 {0.65f, 0.95f, 0.73f, 1.0f});
    }
    if (g_show_enabled_modules) {
        std::vector<std::wstring> modules;
        modules.reserve(24);
        if (g_flying || g_restore_flying_pending) modules.emplace_back(L"Flying");
        if (g_noclip || g_restore_noclip_pending) modules.emplace_back(L"Noclip");
        if (g_walk_speed_percent != 100)
            modules.emplace_back(L"Movement speed: " + std::to_wstring(g_walk_speed_percent) + L"%");
        if (g_swim_speed_percent != 100)
            modules.emplace_back(L"Swim speed: " + std::to_wstring(g_swim_speed_percent) + L"%");
        if (g_slide_speed_percent != 100)
            modules.emplace_back(L"Slide speed: " + std::to_wstring(g_slide_speed_percent) + L"%");
        if (g_slide_distance_percent != 100)
            modules.emplace_back(L"Slide distance: " + std::to_wstring(g_slide_distance_percent) + L"%");
        if (g_jump_height_percent != 100)
            modules.emplace_back(L"Jump height: " + std::to_wstring(g_jump_height_percent) + L"%");
        if (g_acceleration_percent != 100)
            modules.emplace_back(L"Acceleration: " + std::to_wstring(g_acceleration_percent) + L"%");
        if (g_game_speed_percent != 100)
            modules.emplace_back(L"Game speed: " + std::to_wstring(g_game_speed_percent) + L"%");
        if (g_preserve_momentum) modules.emplace_back(L"Preserve momentum");
        if (g_scroll_to_jump.load(std::memory_order_relaxed))
            modules.emplace_back(std::wstring(L"Scroll jump: ") + JumpAssistModeLabel(true));
        if (g_hold_space_to_jump)
            modules.emplace_back(std::wstring(L"Hold Space jump: ") + JumpAssistModeLabel(false));
        if (g_air_jump_enabled) modules.emplace_back(L"Air jump");
        if (g_show_timer) modules.emplace_back(L"Run timer");
        if (g_show_splits) modules.emplace_back(L"Split popups");
        if (g_show_pb_run_hud) modules.emplace_back(L"Full-run PB time");
        if (g_show_sum_of_best_hud) modules.emplace_back(L"Sum of best segments");
        if (g_show_last_button_split_hud) modules.emplace_back(L"Run time at last button");
        if (g_show_compare_hud) modules.emplace_back(L"Other profile time");
        if (g_show_best_segment_time_hud) modules.emplace_back(L"Fastest button record");
        if (g_show_pb_split_time_hud) modules.emplace_back(L"PB time at last button");
        if (g_show_compare_popup) modules.emplace_back(L"Other profile split comparison");
        if (g_show_coordinates) modules.emplace_back(L"Player position");
        if (g_show_speed) modules.emplace_back(L"Movement speed");
        if (g_show_completion_percent) modules.emplace_back(L"Level completion");
        if (g_cube_display_mode != ProgressDisplayMode::Off) modules.emplace_back(L"Cube counter");
        if (g_tile_display_mode != ProgressDisplayMode::Off) modules.emplace_back(L"Tile counter");
        if (g_show_fps) modules.emplace_back(L"FPS counter");
        if (g_show_game_time_hud) modules.emplace_back(L"Game day/night clock");
        if (g_show_local_time_hud) modules.emplace_back(L"PC local clock");
        if (g_show_key_display) modules.emplace_back(L"Key press display");
        if (g_save_personal_bests) modules.emplace_back(L"PB recording");
        if (g_save_fastest_segments) modules.emplace_back(L"Best-segment recording");
        if (g_cheat_record_mode != CheatRecordMode::Off) modules.emplace_back(L"Cheat recording permitted");
        if (g_keep_movement_on_game_launch) modules.emplace_back(L"Movement persists on launch");
        if (g_keep_movement_on_map_restart) modules.emplace_back(L"Movement persists on restart");
        if (g_progress_restart_active) modules.emplace_back(L"Progress restoration");
        if (g_preloaded_restart_active) modules.emplace_back(L"Preloaded map");
        if (g_practice_active) modules.emplace_back(L"Practice mode");
        if (g_buttons_sync_with_sections) modules.emplace_back(L"Buttons sync with sections");
        if (IsPlatformCustomColourEnabled()) modules.emplace_back(L"Platform colours");
        if (IsPlatformSpeedModified()) modules.emplace_back(L"Platform speed: " + std::to_wstring(GetPlatformSpeedPercent()) + L"%");
        if (IsJumpPadCustomColourEnabled()) modules.emplace_back(L"Jump pad colours");
        if (IsWaterCustomColourEnabled()) modules.emplace_back(L"Water colours");
        if (IsUnderwaterCustomColourEnabled()) modules.emplace_back(L"Underwater colours");
        if (IsJumpPadHeightModified()) modules.emplace_back(L"Jump pad launch height: " + std::to_wstring(GetJumpPadHeightPercent()) + L"%");

        add_line(HudElement::EnabledModules,
                 L"Enabled features (" + std::to_wstring(modules.size()) + L")",
                 0.96f, 24.0f, {0.55f, 0.95f, 1.0f, 1.0f}, true);
        if (modules.empty()) {
            add_line(HudElement::EnabledModules, L"None", 0.90f, 18.0f,
                     {0.68f, 0.73f, 0.79f, 1.0f}, false, true);
        } else {
            for (const auto& name : modules)
                add_line(HudElement::EnabledModules, name, 0.88f, 18.0f,
                         {0.81f, 0.93f, 0.87f, 1.0f}, false, true);
        }
    }
    if (g_show_active_cheats) {
        // All cheat entries are owned by records.cpp, which also gates PBs.
        // This prevents newer world/movement features from being omitted.
        const auto cheats = ActiveCheatLabels();

        add_line(HudElement::ActiveCheats,
                 L"Cheats Active (" + std::to_wstring(cheats.size()) + L")",
                 0.96f, 24.0f, {1.0f, 0.71f, 0.42f, 1.0f}, true);
        if (cheats.empty()) {
            add_line(HudElement::ActiveCheats, L"None", 0.90f, 18.0f,
                     {0.68f, 0.73f, 0.79f, 1.0f}, false, true);
        } else {
            for (const auto& name : cheats)
                add_line(HudElement::ActiveCheats, name, 0.88f, 18.0f,
                         {1.0f, 0.86f, 0.74f, 1.0f}, false, true);
        }
    }
    if (g_show_active_customisations) {
        const auto customisations = ActiveCustomisationLabels();
        add_line(HudElement::ActiveCustomisations,
                 L"Customisations Active (" + std::to_wstring(customisations.size()) + L")",
                 0.96f, 24.0f, {0.55f, 0.95f, 1.0f, 1.0f}, true);
        if (customisations.empty()) {
            add_line(HudElement::ActiveCustomisations, L"None", 0.90f, 18.0f,
                     {0.68f, 0.73f, 0.79f, 1.0f}, false, true);
        } else {
            for (const auto& name : customisations)
                add_line(HudElement::ActiveCustomisations, name, 0.88f, 18.0f,
                         {0.80f, 0.95f, 1.0f, 1.0f}, false, true);
        }
    }
    if (g_show_practice_status_hud &&
        g_practice_active && g_practice_phase != PracticePhase::Idle) {
        add_line(HudElement::PracticeStatus, L"PRACTICE  " + PracticeStatus(),
                 0.90f, 26.0f, g_practice_phase == PracticePhase::Failed
                     ? LinearColor{1.0f, 0.31f, 0.29f, 1.0f}
                     : LinearColor{0.98f, 0.80f, 0.45f, 1.0f});
    }
    // Checkpoint replays count as cheats: their PB permission is governed by
    // the same Save With Cheats mode as other assisted movement.
    const bool checkpoint_assisted = g_practice_active &&
        g_practice_origin != PracticeOrigin::ProgressRestart;
    if (g_show_record_status_hud &&
        (CheatsCurrentlyActive() || g_recording_tainted ||
         g_preloaded_restart_active || g_progress_restart_active ||
         checkpoint_assisted)) {
        const bool active = CheatsCurrentlyActive();
        std::wstring indicator;
        if (active) {
            indicator = L"CHEATS ACTIVE";
        } else if (g_progress_restart_active) {
            indicator = g_practice_phase == PracticePhase::Ready
                ? L"PROGRESS LOADED - RECORDS OFF"
                : (g_practice_phase == PracticePhase::Failed
                    ? L"PROGRESS FAILED - RECORDS OFF"
                    : L"RESTORING PROGRESS - RECORDS OFF");
        } else if (g_preloaded_restart_active) {
            indicator = g_preloaded_restart_failed
                ? L"PRELOAD FAILED - RECORDS OFF"
                : (g_preloaded_restart_confirmed
                    ? L"PRELOADED MAP - RECORDS OFF"
                    : L"PRELOADING - RECORDS OFF");
        } else if (checkpoint_assisted) {
            const bool checkpoint_pb_enabled =
                g_practice_phase == PracticePhase::Ready &&
                g_save_personal_bests && g_pb_recording_enabled_for_run &&
                g_pb_cheats_allowed_for_run && CheatRecordsPB(g_cheat_record_mode) &&
                RecordsAllowedForPB();
            const std::wstring suffix = checkpoint_pb_enabled ? L" - PB ON" : L" - PB OFF";
            if (g_practice_phase == PracticePhase::Failed) {
                indicator = L"CHECKPOINT FAILED - PB OFF";
            } else if (g_practice_phase != PracticePhase::Ready) {
                indicator = L"LOADING CHECKPOINT - PB OFF";
            } else if (g_practice_origin == PracticeOrigin::SavedState) {
                indicator = L"SAVED STATE" + suffix;
            } else if (g_practice_origin == PracticeOrigin::LastButton) {
                indicator = L"LAST BUTTON" + suffix;
            } else {
                indicator = L"SECTION PRACTICE" + suffix;
            }
        } else {
            // Restore Position and other non-checkpoint cheats use the same
            // per-run recording permissions; don't report records disabled
            // when PB saving was explicitly permitted.
            const bool pb_on = g_save_personal_bests &&
                g_pb_recording_enabled_for_run && RecordsAllowedForPB() &&
                (g_pb_cheats_allowed_for_run || !g_recording_tainted);
            indicator = pb_on ? L"ASSISTED RUN - PB ON" : L"ASSISTED RUN - PB OFF";
        }
        add_line(HudElement::RecordStatus, indicator, 0.82f, 29.0f,
                 active ? LinearColor{1.0f, 0.66f, 0.34f, 1.0f}
                        : LinearColor{0.96f, 0.80f, 0.43f, 1.0f},
                 false, false, true);
    }

    constexpr float kTopMargin = 16.0f;
    constexpr float kBottomMargin = 30.0f;
    constexpr float kMenuMargin = 16.0f;
    constexpr float kMenuGap = 12.0f;
    constexpr float kMenuRowHeight = 27.0f;
    constexpr float kMenuChromeHeight = 100.0f;
    constexpr float kMenuHelpMinHeight = 72.0f;
    constexpr float kMenuHelpLineHeight = 18.0f;
    constexpr size_t kMenuHelpMaxLines = 8; // Detailed help, bounded on small viewports.
    const float max_height = std::max(18.0f,
        static_cast<float>(viewport_height) - kTopMargin - kBottomMargin);

    // Resolve menu placement before rendering persistent HUD elements. In the
    // same corner, reserve space for the menu rather than drawing over it.
    // Short viewports use a scrollable (keyboard-selected) menu window.
    const bool menu_visible = g_menu_open.load(std::memory_order_relaxed);
    const auto menu_items = menu_visible ? Entries() : std::vector<std::wstring>{};
    const auto text_width = [&](const std::wstring& value, float scale) {
        return HudTextWidth(hud, value, scale);
    };

    // Wrap once before allocating menu space. Multi-sentence help needs a
    // taller panel, but simple one-line options should not waste screen area.
    const float menu_width = std::min(510.0f,
        std::max(280.0f, static_cast<float>(viewport_width) - 32.0f));
    const float menu_help_text_width = std::max(40.0f, menu_width - 42.0f);
    constexpr float kHelpTextScale = 0.82f;
    // Cache wrapped help. HUD rendering can run hundreds of times per second;
    // recomputing Unreal font measurements on each frame would cause stutter.
    static std::wstring cached_help_description;
    static float cached_help_width = -1.0f;
    static std::vector<std::wstring> cached_help_lines;
    if (menu_visible && g_show_menu_descriptions && g_selection >= 0 &&
        static_cast<size_t>(g_selection) < menu_items.size()) {
        const std::wstring description = MenuSelectionDescription(
            g_page, g_selection, menu_items);
        if (description != cached_help_description ||
            menu_help_text_width != cached_help_width) {
            cached_help_description = description;
            cached_help_width = menu_help_text_width;
            cached_help_lines.clear();
            size_t cursor = 0;
            while (cursor < description.size()) {
                while (cursor < description.size() && description[cursor] == L' ') ++cursor;
                if (cursor == description.size()) break;
                const size_t end = description.find(L' ', cursor);
                const std::wstring word = description.substr(cursor, end == std::wstring::npos
                    ? std::wstring::npos : end - cursor);
                if (cached_help_lines.empty()) cached_help_lines.emplace_back();
                std::wstring& current = cached_help_lines.back();
                const std::wstring candidate = current.empty() ? word : current + L" " + word;
                if (!current.empty() && text_width(candidate, kHelpTextScale) > menu_help_text_width) {
                    if (cached_help_lines.size() == kMenuHelpMaxLines) {
                        while (!current.empty() &&
                               text_width(current + L"...", kHelpTextScale) > menu_help_text_width)
                            current.pop_back();
                        current += L"...";
                        break;
                    }
                    cached_help_lines.push_back(word);
                } else {
                    current = candidate;
                }
                if (end == std::wstring::npos) break;
                cursor = end + 1;
            }
        }
    } else {
        cached_help_description.clear();
        cached_help_width = -1.0f;
        cached_help_lines.clear();
    }
    const auto& menu_help_lines = cached_help_lines;
    const float wanted_help_height = menu_help_lines.empty() ? 0.0f :
        std::max(kMenuHelpMinHeight,
                 34.0f + kMenuHelpLineHeight * static_cast<float>(menu_help_lines.size()));
    const float natural_menu_height = kMenuChromeHeight +
        kMenuRowHeight * static_cast<float>(menu_items.size()) + wanted_help_height;
    const size_t menu_corner = static_cast<size_t>(g_menu_position) - 1;
    const float reserved_menu_height = menu_visible
        ? std::min(natural_menu_height,
                   std::max(172.0f, static_cast<float>(viewport_height) * 0.45f))
        : 0.0f;
    std::array<float, kHudPlacementCount> hud_first_y{};
    std::array<float, kHudPlacementCount> hud_after_y{};
    for (size_t corner_index = 0; corner_index < corners.size(); ++corner_index) {
        const auto& lines = corners[corner_index];
        if (lines.empty()) continue;
        // Reserve room for fixed items first, then fit as many module rows as
        // possible. An overflow marker tells users when small screens truncate.
        float fixed_height = 0.0f;
        // Reuse row-height scratch space across draw calls (no per-frame allocation).
        static std::array<std::vector<float>, kHudPlacementCount> detail_height_cache{};
        auto& detail_heights = detail_height_cache[corner_index];
        detail_heights.clear();
        detail_heights.reserve(lines.size());
        for (const auto& line : lines) {
            if (line.module_detail) detail_heights.push_back(line.advance);
            else fixed_height += line.advance;
        }
        const size_t module_count = detail_heights.size();
        const float marker_height = 18.0f;
        const float corner_max_height = (menu_visible && corner_index == menu_corner)
            ? std::max(0.0f, std::min(max_height,
                static_cast<float>(viewport_height) - kTopMargin -
                kMenuMargin - reserved_menu_height - kMenuGap))
            : max_height;
        const float detail_space = std::max(0.0f, corner_max_height - fixed_height);
        float used_detail_height = 0.0f;
        size_t shown_modules = 0;
        // Element sizes vary; count real row heights rather than assuming 18 px.
        for (float height : detail_heights) {
            if (used_detail_height + height > detail_space) break;
            used_detail_height += height;
            ++shown_modules;
        }
        // Make room for the overflow indicator when the stack is truncated.
        while (shown_modules > 0 && shown_modules < module_count &&
               used_detail_height + marker_height > detail_space) {
            used_detail_height -= detail_heights[--shown_modules];
        }
        const size_t omitted_modules = module_count - shown_modules;
        const float total_height = fixed_height + used_detail_height +
            (omitted_modules > 0 ? marker_height : 0.0f);
        const bool at_center = corner_index == static_cast<size_t>(HudCorner::Center);
        const bool at_right = corner_index == static_cast<size_t>(HudCorner::TopRight) ||
                              corner_index == static_cast<size_t>(HudCorner::BottomRight);
        const bool at_bottom = corner_index == static_cast<size_t>(HudCorner::BottomRight) ||
                               corner_index == static_cast<size_t>(HudCorner::BottomLeft);
        // When menu and HUD share a corner, cap the *actual rendered* HUD
        // height too: fixed rows can exceed the desired reservation.
        const float render_height = std::min(total_height, corner_max_height);
        // Center is a single vertically-centered stack, not a fifth corner.
        // Keep the entire stack inside the same viewport margins as corner HUDs.
        float y = at_center
            ? std::clamp((static_cast<float>(viewport_height) - render_height) * 0.5f,
                         kTopMargin,
                         std::max(kTopMargin, static_cast<float>(viewport_height) -
                                                     kBottomMargin - render_height))
            : (at_bottom
                ? std::max(kTopMargin, static_cast<float>(viewport_height) -
                                              kBottomMargin - render_height)
                : kTopMargin);
        const float stack_start = y;
        const float stack_end = y + render_height;
        const auto render_line = [&](const HudLine& line, float row_y) {
            const float width = line.key_row >= 0
                ? 194.0f * line.scale : text_width(line.text, line.scale);
            const float x = at_center
                ? std::clamp(center_x - width * 0.5f, 8.0f,
                             std::max(8.0f, static_cast<float>(viewport_width) - width - 8.0f))
                : (at_right
                    ? std::max(8.0f, static_cast<float>(viewport_width) - 18.0f - width)
                    : 18.0f);
            if (line.warning_background && g_draw_rect) {
                const float bx = std::max(2.0f, x - 9.0f);
                const float bw = std::max(0.0f, std::min(
                    static_cast<float>(viewport_width) - bx - 4.0f, width + 17.0f));
                if (bw > 0.0f)
                    g_draw_rect(hud, {0.06f, 0.07f, 0.09f, 0.72f},
                                bx, row_y - 4.0f, bw, std::max(25.0f, line.advance));
            }
            if (line.key_row >= 0) {
                // Rectangular keycaps use the existing UE HUD rectangles/text.
                // Sampling once per row is inexpensive; all caps share the
                // foreground-only check so Alt+Tab cannot display other apps.
                DWORD foreground_pid = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
                const bool focused = foreground_pid == GetCurrentProcessId();
                const auto held = [&](int virtual_key) {
                    return focused && (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
                };
                const auto cap = [&](const wchar_t* caption, bool down,
                                     float offset_x, float cap_width) {
                    const float k = line.scale;
                    const float cap_x = x + offset_x * k;
                    const float cap_y = row_y + 1.0f * k;
                    const float cap_h = 26.0f * k;
                    // Original colour/preset/custom HEX applies to pressed
                    // caps; idle caps remain dark and visibly readable.
                    const LinearColor fill = down
                        ? LinearColor{line.color.r, line.color.g, line.color.b, 0.92f}
                        : LinearColor{0.11f, 0.14f, 0.19f, 0.78f};
                    const LinearColor label = down
                        ? LinearColor{0.03f, 0.06f, 0.08f, 1.0f}
                        : LinearColor{0.80f, 0.84f, 0.90f, 0.95f};
                    if (g_draw_rect) {
                        g_draw_rect(hud, {0.36f, 0.45f, 0.56f, 0.92f},
                                    cap_x, cap_y, cap_width * k, cap_h);
                        g_draw_rect(hud, fill, cap_x + 1.0f * k, cap_y + 1.0f * k,
                                    (cap_width - 2.0f) * k, cap_h - 2.0f * k);
                    }
                    const std::wstring caption_text(caption);
                    const float caption_scale = 0.79f * k;
                    const float caption_width = text_width(caption_text, caption_scale);
                    DrawText(hud, caption_text,
                             cap_x + (cap_width * k - caption_width) * 0.5f,
                             cap_y + 3.0f * k, caption_scale,
                             g_draw_rect ? label : (down ? line.color : label));
                };
                if (line.key_row == 0) {
                    cap(L"W", held('W'), 78.0f, 38.0f);
                } else if (line.key_row == 1) {
                    cap(L"A", held('A'), 36.0f, 38.0f);
                    cap(L"S", held('S'), 78.0f, 38.0f);
                    cap(L"D", held('D'), 120.0f, 38.0f);
                } else if (line.key_row == 2) {
                    cap(L"SHIFT", held(VK_SHIFT) || held(VK_LSHIFT) || held(VK_RSHIFT),
                        4.0f, 90.0f);
                    cap(L"SPACE", held(VK_SPACE), 100.0f, 90.0f);
                }
                return;
            }
            DrawText(hud, line.text, x, row_y, line.scale, line.color);
        };
        size_t rendered_details = 0;
        bool marker_drawn = false;
        for (const auto& line : lines) {
            if (line.module_detail && rendered_details >= shown_modules) {
                if (omitted_modules && !marker_drawn && y + marker_height <= stack_end) {
                    HudLine overflow{L"+" + std::to_wstring(omitted_modules) + L" more",
                                     0.88f, marker_height,
                                     {0.83f, 0.83f, 0.83f, 1.0f}};
                    render_line(overflow, y);
                    y += marker_height;
                    marker_drawn = true;
                }
                continue;
            }
            if (y + line.advance > stack_end ||
                y + line.advance > static_cast<float>(viewport_height) - 4.0f)
                break;
            render_line(line, y);
            y += line.advance;
            if (line.module_detail) ++rendered_details;
        }
        hud_first_y[corner_index] = stack_start;
        hud_after_y[corner_index] = y;
    }
    // Keep each enabled component in one compact, vertically centered stack.
    // The whole stack sits slightly below the screen center (58% height),
    // regardless of how many rows the user has enabled.
    const float popup_center_y = static_cast<float>(viewport_height) * 0.58f;
    // Show button split popups is the master switch for all popup rows,
    // including section comparisons configured in Popup Layout.
    if (g_show_splits && !g_button_splits.empty() &&
        GetTickCount64() < g_split_popup_expires) {
        const auto& split = g_button_splits.back();
        const ULONGLONG popup_now = GetTickCount64();
        const auto until = g_split_popup_expires > popup_now
            ? g_split_popup_expires - popup_now : 0;
        const float fade = std::clamp(static_cast<float>(until) / 450.0f, 0.0f, 1.0f);

        // Split popups follow every button, but section comparisons must
        // survive intermediate buttons (7.1, 10.1, etc.) in the next section.
        // The section's original four-second expiry prevents stale deltas.
        const auto completed = std::find_if(g_button_splits.rbegin(),
            g_button_splits.rend(), [&](const ButtonSplit& item) {
                return item.section_popup_expires > popup_now &&
                    (SectionEndingAtButton(item.button_number).section != 0 ||
                     item.section_is_partial_button);
            });
        const bool section_completed = completed != g_button_splits.rend();
        const bool partial_button = section_completed && completed->section_is_partial_button;
        const SectionBoundary section = section_completed
            ? (partial_button ? SectionContainingButton(completed->button_number)
                              : SectionEndingAtButton(completed->button_number))
            : SectionBoundary{};
        const float section_fade = section_completed
            ? std::clamp(static_cast<float>(completed->section_popup_expires - popup_now) /
                             450.0f, 0.0f, 1.0f)
            : 0.0f;
        // An enabled comparison always owns a row, even if the current
        // button has no completed section to compare yet.
        const bool show_pb_gain_loss =
            g_pb_gain_loss_display != SectionCompareDisplayMode::Off;
        const bool show_section_pb =
            g_section_vs_pb_display != SectionCompareDisplayMode::Off;
        const bool show_section_file =
            g_section_vs_file_display != SectionCompareDisplayMode::Off;
        // All comparison inputs are captured at the completed button press.
        // A later practice load or simultaneous button activation cannot turn
        // a measured section into N/A during its four-second popup.
        const SectionBoundary comparison_boundary = section_completed &&
            completed->section_comparison_boundary.section != 0
            ? completed->section_comparison_boundary : section;
        const bool practice_remainder = section_completed && !partial_button &&
            comparison_boundary.previous_end_button > section.previous_end_button;
        const double current_section_seconds = section_completed &&
            (show_section_pb || show_section_file)
            ? completed->section_actual_seconds : -1.0;

        // PB difference uses cumulative button times. An enabled row is
        // present even in practice or when no PB split has been recorded.
        const bool has_pb_row = g_show_splits && g_pb_row_mode != SplitRowMode::Off;
        bool has_pb_delta = false;
        double pb_delta = 0.0;
        if (has_pb_row && !g_practice_active &&
            split.button_number >= 1 &&
            split.button_number <= kRefunctButtonCount) {
            // Use the PB recorded when this button was captured. The final
            // button can save a new PB before this popup is drawn.
            const double best = split.cumulative_pb_reference_seconds;
            if (std::isfinite(best) && best >= 0.0 &&
                std::isfinite(split.run_seconds) && split.run_seconds >= 0.0 &&
                split.run_seconds <= 86400.0) {
                has_pb_delta = true;
                pb_delta = DisplayedTimestampDeltaSeconds(split.run_seconds, best);
            }
        }

        const bool has_fastest_row = g_show_splits && g_show_split_fastest_delta;
        const bool has_compare_row = g_show_splits && g_show_compare_popup;
        const float height =
            (g_show_splits && g_show_split_button ? 26.0f : 0.0f) +
            (g_show_splits && g_show_split_total_time ? 45.0f : 0.0f) +
            (g_show_splits && g_segment_row_mode != SplitRowMode::Off ? 30.0f : 0.0f) +
            (has_pb_row ? 30.0f : 0.0f) +
            (show_pb_gain_loss ? 30.0f : 0.0f) +
            (has_fastest_row ? 30.0f : 0.0f) +
            (has_compare_row ? 30.0f : 0.0f) +
            (show_section_pb ? 30.0f : 0.0f) +
            (show_section_file ? 30.0f : 0.0f);
        float y = popup_center_y - height * 0.5f;
        if (g_show_splits && g_show_split_button) {
            DrawCenteredText(hud, L"BUTTON " + ButtonLabel(split.button_number),
                             center_x, y, 0.87f, {0.62f, 0.88f, 1.0f, fade});
            y += 26.0f;
        }
        if (g_show_splits && g_show_split_total_time) {
            const bool valid_total = std::isfinite(split.run_seconds) &&
                split.run_seconds >= 0.0 && split.run_seconds <= 86400.0;
            DrawCenteredText(hud, valid_total ? FormatSplitTime(split.run_seconds) : L"N/A",
                             center_x, y, 1.52f, valid_total
                                 ? LinearColor{0.98f, 0.99f, 1.0f, fade}
                                 : DeltaColor(DeltaDirection::Invalid, fade));
            y += 45.0f;
        }
        if (g_show_splits && g_segment_row_mode != SplitRowMode::Off) {
            const bool valid_segment = std::isfinite(split.segment_seconds) &&
                split.segment_seconds >= 0.0 && split.segment_seconds <= 86400.0;
            const std::wstring segment_text =
                (g_segment_row_mode == SplitRowMode::HeaderAndTime ? L"Time since last button  " : L"") +
                (valid_segment ? FormatSplitTime(split.segment_seconds) : L"N/A");
            DrawCenteredText(hud, segment_text, center_x, y, 0.95f,
                             valid_segment ? LinearColor{0.86f, 0.91f, 0.97f, fade}
                                           : DeltaColor(DeltaDirection::Invalid, fade));
            y += 30.0f;
        }
        if (has_pb_row) {
            const std::wstring prefix =
                g_pb_row_mode == SplitRowMode::HeaderAndTime ? L"Total vs PB  " : L"";
            if (has_pb_delta) {
                const auto display = FormatSplitDelta(pb_delta, true);
                DrawCenteredText(hud, prefix + display.text, center_x, y, 0.96f,
                                 DeltaColor(display.direction, fade));
            } else {
                DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.96f,
                                 DeltaColor(DeltaDirection::Invalid, fade));
            }
            y += 30.0f;
        }
        if (show_pb_gain_loss) {
            const std::wstring prefix = SectionCompareHasHeader(g_pb_gain_loss_display)
                ? L"Time gained/lost here  " : L"";
            if (std::isfinite(split.pb_gain_loss_seconds)) {
                const bool with_time = SectionCompareHasTime(g_pb_gain_loss_display);
                const auto display = FormatSplitDelta(split.pb_gain_loss_seconds, with_time);
                const std::wstring difference = !with_time &&
                    display.direction == DeltaDirection::Even ? L"=" : display.text;
                DrawCenteredText(hud, prefix + difference, center_x, y, 0.95f,
                                 DeltaColor(display.direction, fade));
            } else {
                DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.95f,
                                 DeltaColor(DeltaDirection::Invalid, fade));
            }
            y += 30.0f;
        }
        if (has_fastest_row) {
            const bool using_pb_segment_fallback =
                (!std::isfinite(split.previous_recorded_gold_seconds) ||
                 split.previous_recorded_gold_seconds < 0.0) &&
                std::isfinite(split.comparison_best_seconds) &&
                split.comparison_best_seconds >= 0.0;
            const std::wstring label = g_show_fastest_delta_label
                ? (using_pb_segment_fallback
                    ? L"Button vs PB segment  "
                    : L"Button vs fastest  ") : L"";
            if (std::isfinite(split.comparison_best_seconds) &&
                split.comparison_best_seconds >= 0.0 &&
                split.comparison_best_seconds <= 86400.0 &&
                std::isfinite(split.segment_seconds) &&
                split.segment_seconds >= 0.0 && split.segment_seconds <= 86400.0) {
                const double diff = split.segment_seconds - split.comparison_best_seconds;
                const auto display = FormatSplitDelta(diff, g_show_fastest_delta_time);
                DrawCenteredText(hud, label + display.text, center_x, y, 0.94f,
                                 DeltaColor(display.direction, fade));
            } else {
                // No earlier best exists, even if this press just set a new
                // one. A difference cannot be calculated without a reference.
                DrawCenteredText(hud, label + L"N/A", center_x, y, 0.94f,
                                 DeltaColor(DeltaDirection::Invalid, fade));
            }
            y += 30.0f;
        }
        if (has_compare_row) {
            const double reference = !g_practice_active
                ? ComparisonReferenceSeconds(split.button_number) : -1.0;
            const std::wstring label = g_compare_popup_show_header
                ? L"Vs " + (g_compare_profile.empty() ? L"profile" : g_compare_profile) +
                    (g_compare_mode == ProfileCompareMode::PersonalBest
                        ? L" PB  " : L" Best  ")
                : L"";
            if (std::isfinite(reference) && reference >= 0.0 &&
                std::isfinite(split.run_seconds) && split.run_seconds >= 0.0) {
                const auto display = FormatSplitDelta(split.run_seconds - reference,
                                                     g_compare_popup_show_time);
                // A rounded tie has no +/- sign; show = in sign-only mode.
                const std::wstring signed_value = !g_compare_popup_show_time &&
                    display.direction == DeltaDirection::Even ? L"=" : display.text;
                DrawCenteredText(hud, label + signed_value, center_x, y, 0.94f,
                                 DeltaColor(display.direction, fade));
            } else {
                // All unavailable comparisons use the same placeholder,
                // including practice and sign-only display modes.
                DrawCenteredText(hud, label + L"N/A", center_x, y, 0.94f,
                                 DeltaColor(DeltaDirection::Invalid, fade));
            }
            y += 30.0f;
        }
        // Completed sections compare their full duration; intermediate buttons
        // (18.1 etc.) compare their own measured PB button-segment duration.
        // External-file comparisons always use that file's PB splits first.
        const auto draw_section_delta = [&](SectionCompareDisplayMode mode,
                                            const std::wstring& heading,
                                            const PersonalBest& reference) {
            if (mode == SectionCompareDisplayMode::Off) return;
            const double pb_seconds = partial_button
                ? PBButtonSegmentDuration(reference.button_seconds, completed->button_number)
                : SectionDurationFromRecordedPB(reference.section_seconds,
                    reference.button_seconds, comparison_boundary);
            const double gold_seconds = partial_button
                ? reference.best_segment_seconds[static_cast<size_t>(completed->button_number - 1)]
                : SectionDurationFromSegments(reference.best_segment_seconds, comparison_boundary);
            // The comparison selector chooses exactly one reference. A PB
            // comparison must not silently switch to a sum of independent
            // fastest segments: those may come from different runs.
            const bool prefer_best = g_compare_mode == ProfileCompareMode::BestSegments;
            const double reference_seconds = prefer_best ? gold_seconds : pb_seconds;
            const std::wstring prefix = SectionCompareHasHeader(mode)
                ? (partial_button
                    ? L"Button " + ButtonLabel(completed->button_number) + L" " + heading + L"  "
                    : heading + L" (" + std::to_wstring(section.section) + L")  ")
                : L"";
            if (std::isfinite(current_section_seconds) && current_section_seconds >= 0.0 &&
                std::isfinite(reference_seconds) && reference_seconds >= 0.0) {
                const auto display = FormatSplitDelta(current_section_seconds - reference_seconds,
                                                     SectionCompareHasTime(mode));
                const std::wstring difference = !SectionCompareHasTime(mode) &&
                    display.direction == DeltaDirection::Even ? L"=" : display.text;
                // In Best Segments mode, label the independent-gold reference.
                // In PB mode, a missing PB is N/A, never a gold substitution.
                const std::wstring suffix = prefer_best
                    ? (partial_button ? L" (vs best)" : L" (vs sum of best)") : L"";
                DrawCenteredText(hud, prefix + difference + suffix, center_x, y, 0.95f,
                                 DeltaColor(display.direction, section_fade));
            } else {
                DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.95f,
                                 DeltaColor(DeltaDirection::Invalid, section_fade));
            }
            y += 30.0f;
        };
        if (show_section_pb && !section_completed) {
            // No completed section exists for this popup. Reserve its row
            // rather than letting the stack jump when a boundary is reached.
            const std::wstring prefix = SectionCompareHasHeader(g_section_vs_pb_display)
                ? L"Section vs PB  " : L"";
            DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.95f,
                             DeltaColor(DeltaDirection::Invalid, fade));
            y += 30.0f;
        }
        if (show_section_pb && section_completed) {
            // Use the reference snapshotted at this button event, before any
            // new PB could replace the historical reference. Intermediate
            // buttons and completed sections each keep their own snapshot.
            const double old_pb_section = completed->section_pb_reference_seconds;
            const std::wstring prefix = SectionCompareHasHeader(g_section_vs_pb_display)
                ? (partial_button
                    ? L"Button " + ButtonLabel(completed->button_number) + L" vs PB  "
                    : (practice_remainder ? L"Section remainder vs PB (" : L"Section vs PB (") +
                        std::to_wstring(section.section) + L")  ")
                : L"";
            // Section vs PB uses only the PB snapshot captured at the button.
            // A sum of independently recorded golds is not a PB section.
            const double reference_seconds = old_pb_section;
            if (std::isfinite(current_section_seconds) && current_section_seconds >= 0.0 &&
                std::isfinite(reference_seconds) && reference_seconds >= 0.0) {
                const bool with_time = SectionCompareHasTime(g_section_vs_pb_display);
                const auto display = FormatSplitDelta(current_section_seconds - reference_seconds,
                                                     with_time);
                const std::wstring difference = !with_time &&
                    display.direction == DeltaDirection::Even ? L"=" : display.text;
                DrawCenteredText(hud, prefix + difference, center_x, y, 0.95f,
                                 DeltaColor(display.direction, section_fade));
            } else {
                DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.95f,
                                 DeltaColor(DeltaDirection::Invalid, section_fade));
            }
            y += 30.0f;
        }
        if (show_section_file && !section_completed) {
            const std::wstring prefix = SectionCompareHasHeader(g_section_vs_file_display)
                ? L"Section vs profile  " : L"";
            DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.95f,
                             DeltaColor(DeltaDirection::Invalid, fade));
            y += 30.0f;
        }
        if (show_section_file && section_completed) {
            // A missing/identical comparison profile must never be treated
            // as a valid zero-time reference.
            const bool file_available = g_compare_reference_loaded &&
                !g_compare_profile.empty() &&
                _wcsicmp(g_compare_profile.c_str(), g_active_profile.c_str()) != 0;
            if (file_available) {
                draw_section_delta(g_section_vs_file_display,
                    (practice_remainder ? L"Section remainder vs " : L"Section vs ") +
                        g_compare_profile, g_compare_pb);
            } else {
                const std::wstring prefix = SectionCompareHasHeader(g_section_vs_file_display)
                    ? (partial_button
                        ? L"Button " + ButtonLabel(completed->button_number) + L" vs file  "
                        : L"Section vs file (" + std::to_wstring(section.section) + L")  ")
                    : L"";
                DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.95f,
                                 DeltaColor(DeltaDirection::Invalid, section_fade));
                y += 30.0f;
            }
        }
    }
    // A distinct cheat alert survives normal feature Notify messages, and
    // appears whether the F5 menu is open or closed.
    if (!g_cheat_popup_text.empty() && GetTickCount64() < g_cheat_popup_expires)
        DrawCenteredText(hud, g_cheat_popup_text, center_x,
                         static_cast<float>(viewport_height) * 0.22f, 0.94f,
                         {1.0f, 0.65f, 0.36f, 1.0f});
    // PB completion notice is independent of the individual split rows.
    if (GetTickCount64() < g_pb_popup_expires)
        DrawCenteredText(hud, g_pb_popup_text, center_x,
                         static_cast<float>(viewport_height) * 0.76f,
                         1.05f, {0.52f, 1.0f, 0.66f, 1.0f});
    if (!menu_visible) {
        // A small on-screen confirmation matters for hotkeys used without the
        // F5 menu (save, restore, load last button, and failure messages).
        if (!g_notice.empty() && GetTickCount64() < g_notice_expires)
            DrawCenteredText(hud, g_notice, center_x,
                             static_cast<float>(viewport_height) * 0.16f,
                             0.88f, {0.70f, 0.96f, 0.83f, 1.0f});
        return;
    }
    // Stack the F5 menu after other HUD elements in its selected corner.
    // Top corners: below the HUD stack. Bottom corners: above the stack,
    // since both would otherwise compete for the same bottom edge.
    const auto& items = menu_items;
    const bool at_menu_bottom = g_menu_position == MenuPosition::BottomRight ||
                                g_menu_position == MenuPosition::BottomLeft;
    float menu_min_y = kMenuMargin;
    float menu_max_y = static_cast<float>(viewport_height) - kMenuMargin;
    if (menu_corner < corners.size() && hud_after_y[menu_corner] > hud_first_y[menu_corner]) {
        if (at_menu_bottom)
            menu_max_y = std::min(menu_max_y, hud_first_y[menu_corner] - kMenuGap);
        else
            menu_min_y = std::max(menu_min_y, hud_after_y[menu_corner] + kMenuGap);
    }
    const float available_menu_height = std::max(0.0f, menu_max_y - menu_min_y);
    // Leave at least one selectable row visible on short viewports.
    // If there is not enough room, suppress help rather than clipping the menu.
    const bool show_menu_help = g_show_menu_descriptions && !menu_help_lines.empty() &&
        available_menu_height >= kMenuChromeHeight + kMenuRowHeight + kMenuHelpMinHeight;
    // Prefer the whole description; use a shorter excerpt if HUD placement
    // leaves limited room. Never allow the panel to overlap selectable rows.
    const float help_height = show_menu_help
        ? std::min(wanted_help_height, available_menu_height - kMenuChromeHeight - kMenuRowHeight)
        : 0.0f;
    const int max_visible_rows = std::max(1, static_cast<int>(
        std::floor((available_menu_height - kMenuChromeHeight - help_height) / kMenuRowHeight)));
    const int visible_rows = std::min(static_cast<int>(items.size()), max_visible_rows);
    const int first_row = std::clamp(g_selection - visible_rows + 1, 0,
        std::max(0, static_cast<int>(items.size()) - visible_rows));
    const float height = std::min(available_menu_height,
        kMenuChromeHeight + kMenuRowHeight * static_cast<float>(visible_rows) + help_height);
    const float width = menu_width;
    const float right_x = std::max(8.0f,
        static_cast<float>(viewport_width) - width - kMenuMargin);
    const float menu_x = (g_menu_position == MenuPosition::TopLeft ||
                          g_menu_position == MenuPosition::BottomLeft)
        ? kMenuMargin : right_x;
    const float top = at_menu_bottom ? menu_max_y - height : menu_min_y;
    const float menu_text_x = menu_x + 14.0f;
    if (g_draw_rect) g_draw_rect(hud, {0.02f, 0.035f, 0.05f, 0.84f}, menu_x, top, width, height);
    DrawText(hud, L"REFUNCT UTILITY v1.0.0  [" + (Hotkey(Action::Menu) == 0 ? L"F5 fallback" : KeyName(Hotkey(Action::Menu))) + L" menu]",
             menu_text_x, top + 12.0f, 1.12f,
             {0.43f, 0.87f, 1.0f, 1.0f});
    // Breadcrumb identifies the current module. Keep the main-page controls
    // visible, and leave Back / Hide as explicit selectable rows in submenus.
    const std::wstring menu_context = g_page == Page::Main
        ? L"Up/Down: select   Enter: choose   /: search   Esc: back"
        : (g_page == Page::Search
            ? std::wstring(L"Type: filter  Backspace: erase  Enter: go  Esc: back")
            : std::wstring(MenuBreadcrumb(g_page))) +
            (visible_rows < static_cast<int>(items.size()) ? L"  (scroll)" : L"");
    DrawText(hud, menu_context, menu_text_x, top + 42.0f, 0.83f,
             {0.66f, 0.73f, 0.79f, 1.0f});
    for (int row = 0; row < visible_rows; ++row) {
        const size_t i = static_cast<size_t>(first_row + row);
        const float y = top + 73.0f + static_cast<float>(row) * kMenuRowHeight;
        const bool selected = static_cast<int>(i) == g_selection;
        if (selected && g_draw_rect)
            g_draw_rect(hud, {0.14f, 0.27f, 0.35f, 0.90f}, menu_x + 9.0f, y - 3.0f, width - 18.0f, 25.0f);
        DrawText(hud, std::wstring(selected ? L">  " : L"   ") + items[i], menu_text_x, y, 0.93f,
                 selected ? LinearColor{0.55f, 0.95f, 1.0f, 1.0f} :
                            LinearColor{0.95f, 0.97f, 1.0f, 1.0f});
    }
    if (show_menu_help && g_selection >= 0 &&
        static_cast<size_t>(g_selection) < items.size()) {
        const float help_top = top + 74.0f + static_cast<float>(visible_rows) * kMenuRowHeight;
        const float help_text_x = menu_text_x + 4.0f;
        if (g_draw_rect)
            g_draw_rect(hud, {0.05f, 0.10f, 0.13f, 0.95f},
                        menu_x + 9.0f, help_top, width - 18.0f, help_height - 5.0f);
        DrawText(hud, L"SELECTED OPTION", help_text_x, help_top + 5.0f, 0.76f,
                 {0.49f, 0.84f, 0.95f, 1.0f});
        const size_t max_drawn_lines = std::max<size_t>(1, static_cast<size_t>(
            std::floor((help_height - 34.0f) / kMenuHelpLineHeight)));
        const size_t drawn_lines = std::min(max_drawn_lines, menu_help_lines.size());
        for (size_t line = 0; line < drawn_lines; ++line) {
            std::wstring value = menu_help_lines[line];
            if (line + 1 == drawn_lines && drawn_lines < menu_help_lines.size()) {
                // Smaller viewports receive an excerpt, clearly marked as such.
                while (!value.empty() &&
                       text_width(value + L"...", kHelpTextScale) > menu_help_text_width)
                    value.pop_back();
                value += L"...";
            }
            DrawText(hud, value, help_text_x,
                     help_top + 25.0f + static_cast<float>(line) * kMenuHelpLineHeight,
                     kHelpTextScale, {0.91f, 0.94f, 0.97f, 1.0f});
        }
    }
    if (g_name_editing.load()) {
        DrawText(hud, L"NAME: " + g_name_buffer + L"_   [Enter save / Esc cancel]",
                 menu_text_x, top + height - 29.0f, 0.90f,
                 {1.0f, 0.82f, 0.36f, 1.0f});
    } else if (g_rebinding.load()) {
        if (g_draw_rect) g_draw_rect(hud, {0.03f, 0.09f, 0.11f, 0.95f}, menu_x + 6.0f,
                                    top + height - 31.0f, width - 12.0f, 25.0f);
        DrawText(hud, L"PRESS KEY  -  ESC / BACKSPACE = NONE", menu_text_x, top + height - 29.0f, 0.94f,
                 {1.0f, 0.82f, 0.36f, 1.0f});
    } else if (!g_notice.empty() && GetTickCount64() < g_notice_expires) {
        DrawText(hud, g_notice, menu_text_x, top + height - 27.0f, 0.86f,
                 {0.65f, 0.95f, 0.72f, 1.0f});
    }
}


} // namespace refunct
