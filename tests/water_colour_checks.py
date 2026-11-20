"""Water customisation integration and PB-safety regression checks.

Source-level checks: Windows x86/game runtime behaviour still requires testing.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
read = lambda name: (root / name).read_text(encoding="utf-8")
water = read("src/water.cpp")
header = read("include/refunct/features/water.hpp")
api = read("include/refunct/internal_api.hpp")
menu = read("src/menu.cpp")
hud = read("src/hud.cpp")
records = read("src/records.cpp")
tick = read("src/game_tick.cpp")
state = read("include/refunct/state.hpp")
cmake = read("CMakeLists.txt")

assert "src/water.cpp" in cmake
assert '#include "refunct/features/water.hpp"' in api
for function in (
    "IsWaterCustomColourEnabled", "SetWaterCustomColourEnabled",
    "GetWaterCustomColourHex", "SetWaterCustomColourHex",
    "OpenWaterColourPicker", "UpdateWaterColours",
):
    assert function in header
    assert function in water
assert "UpdateWaterColours(player);" in tick

# Verify runtime discovery uses the shipped ocean material, rather than a
# hard-coded actor/component name which can miss Blueprint-generated meshes.
for token in (
    'L"StaticMeshComponent"', 'WaterIsLevelWaterComponent(item.object)',
    'WaterGetMaterial(item.object)', 'WaterIsOceanMaterial(material)',
    'BP_WaterSurface_C', 'M_Infinite_Ocean_Inst', 'M_Infinite_Ocean_Linux',
    'L"base_color"', 'WaterBaseColourParameter', 'WaterObserveNameLibrary',
    'L"Default__KismetStringLibrary"', 'L"Conv_StringToName"',
    'L"VectorParameterValues"', 'L"GetMaterial"', 'L"SetMaterial"',
    'WaterObserveLoadedParameter',
    'L"MaterialExpressionVectorParameter"', 'kEntryStrides',
    'L"CreateDynamicMaterialInstance"', 'L"SetVectorParameterValue"',
    'L"SourceMaterial"', 'L"OptionalName"',
    'target.original_material', 'target.dynamic_material',
    'WaterGetMaterial(mesh) == mid',
    's_restore_pending', 's_scan_cursor', 's_next_scan',
    'kApplyBatch', 'WaterIsTargetValid', 'serial_number',
    'L"WaterColours"', 'L"CustomEnabled"', 'L"Hex"',
    'CC_FULLOPEN', 'CC_RGBINIT', 'kPickerCancelled',
):
    assert token in water, f"Missing material-based water discovery/lifecycle: {token}"
# Guard against reintroducing the removed, undefined V3 observer.
assert 'WaterObserveOceanParameter' not in water
# Never colour arbitrary world meshes: require an exact packaged ocean
# material and a live actor-owned mesh; never touch class-default templates.
assert 'if (!material || !WaterIsOceanMaterial(material)) continue;' in water
assert '(static_cast<uint32_t>(header->flags) & 0x30u) != 0' in water
assert 'WaterIsA(owner, L"Actor")' in water
assert 'WaterSetMaterial(target.mesh, original)' in water
for token in ('g_recording_tainted =', 'g_run_pb_eligible =',
              'MarkCheatAction(', 's_used_actions'):
    assert token not in water, f"Water cosmetic must not taint recordings: {token}"
assert 'if (WaterConfig().custom == enabled) return;' in water
assert 'if (!surface_active && !world_active && !s_restore_pending &&' in water
# Game Default must restore the original material and keep saved state if
# SetMaterial fails. Never silently abandon a tinted MID on a failed restore.
for token in (
    'WaterClearOwnMID', 'L"ClearParameterValues"',
    'if (original && WaterSetMaterial(target.mesh, original))',
    'target.original_material = original;',
    'target.cleared_on_restore = true;',
    'target.next_verify = now + 1000;',
    'target.restore_warning_logged = true;',
    'WaterApplyColour(target, false, 0);',
    'if (s_targets.empty() && s_scan_cursor >= 0 && (surface_active || world_active))',
    'if (s_targets.empty() && s_scan_cursor < 0)',
    'native surface material restored (Game Default)',
):
    assert token in water, f'Missing robust Game Default restoration: {token}'
restore_case = water.split('if (!enabled) {', 1)[1].split('const bool ours =', 1)[0]
assert restore_case.index('WaterSetMaterial(target.mesh, original)') < restore_case.index('WaterClearOwnMID(displayed)')
assert restore_case.index('WaterClearOwnMID(displayed)') < restore_case.index('target.next_verify = now + 1000;')
assert 'if (ours && !WaterSetMaterial(target.mesh, target.original_material)) return;' not in water


assert 'WorldWater, WorldWaterColours' in state
for token in (
    'L"Water  >"',
    'case Page::WorldWater:', 'case Page::WorldWaterColours:',
    'SetPage(Page::WorldWaterColours)',
    'WaterColourMethodLabel()', 'OpenWaterColourMethod()',
    's_water_use_windows_picker', 's_tile_hex_axis = 7;',
    'if (s_tile_hex_axis == 7) SetWaterCustomColourHex(hex);',
    'SetWaterCustomColourEnabled(!IsWaterCustomColourEnabled());',
    'IsWaterCustomColourEnabled()) SetWaterCustomColourEnabled(false);',
):
    assert token in menu, f"Missing menu, colour editor or reset: {token}"
world_rows = menu.split('case Page::World:\n            return {', 1)[1].split('};', 1)[0]
custom_rows = menu.split('case Page::Customisation:\n            return {', 1)[1].split('};', 1)[0]
assert 'L"Water  >"' in world_rows and 'L"Water  >"' in custom_rows
activate = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1]
custom_action = activate.split('case Page::Customisation:', 1)[1].split('case Page::WorldWater:', 1)[0]
assert 'else if (i == 7) {' in custom_action
assert 's_water_colour_return_page = Page::Customisation;' in custom_action
assert 'SetPage(Page::WorldWaterColours);' in custom_action
assert 'else if (i == 16) ResetWorldCustomisation();' in custom_action

assert 'IsWaterCustomColourEnabled()' in hud
assert 'modules.emplace_back(L"Water colours")' in hud
assert 'WaterColour' in records
cosmetic = records.split('constexpr uint64_t kCustomisationMask =', 1)[1].split(';', 1)[0]
assert 'CheatMask(CheatBit::WaterColour)' in cosmetic
assert 'IsWaterCustomColourEnabled() || IsUnderwaterCustomColourEnabled()' in records
assert 'add(CheatBit::WaterColour, L"Water colours");' in records
assert 'return AllSettingMask() & ~kCustomisationMask;' in records
assert 'return AllSettingMask() & kCustomisationMask;' in records

# The underwater post-process setting is independent from the surface water
# material and must not create an additional gameplay-cheat bit.
for fn in (
    'IsUnderwaterCustomColourEnabled', 'SetUnderwaterCustomColourEnabled',
    'GetUnderwaterCustomColourHex', 'SetUnderwaterCustomColourHex',
    'OpenUnderwaterColourPicker',
):
    assert fn in header and fn in water, f'Missing underwater API: {fn}'
for token in (
    'L"PostWater"', 'L"PostProcessVolume"', 'L"PostProcessSettings"',
    'L"SceneColorTint"', 'L"StructProperty"',
    'UnderwaterTargetValid', 'UnderwaterMemberOffset',
    'UnderwaterRestoreBeforeRebind', 'UnderwaterApplyTint',
    'L"UnderwaterCustomEnabled"', 'L"UnderwaterHex"',
    'UnderwaterDiscover(objects)', 'UpdateUnderwaterColours(player);',
):
    assert token in water, f'Missing targeted underwater tint logic: {token}'
for token in (
    'UnderwaterColourMethodLabel()', 'OpenUnderwaterColourMethod()',
    's_underwater_use_windows_picker', 's_tile_hex_axis = 8;',
    'if (s_tile_hex_axis == 8) SetUnderwaterCustomColourHex(hex);',
    'SetUnderwaterCustomColourEnabled(!IsUnderwaterCustomColourEnabled());',
    'IsUnderwaterCustomColourEnabled()) SetUnderwaterCustomColourEnabled(false);',
):
    assert token in menu, f'Missing underwater menu wiring: {token}'
# A FPostProcessSettings colour value is ignored unless the matching
# bOverride bool bit is enabled. Inspect the reflected UBoolProperty descriptor
# and preserve it together with the colour when restoring Game Default.
for token in (
    'L"bOverride_SceneColorTint"', 'L"BoolProperty"',
    'UnderwaterOverrideMember', 'UnderwaterOverrideAddress',
    'UnderwaterSetOverride(override_location, true);',
    'UnderwaterSetOverride(override_location, target.original_override);',
    'UnderwaterSetOverride(override_location,',
    'target.original_override = override_enabled;',
    'const uint8_t byte_offset = d[1];',
    'const uint8_t byte_mask = d[2];',
    'const uint8_t field_mask = d[3];',
    'SceneColorTint and bOverride_SceneColorTint',
):
    assert token in water, f"Missing underwater override-bit handling: {token}"
assert 'if (!UnderwaterTargetValid(objects)) return;' in water
assert 'if (!UnderwaterResolveTint()) {' in water
assert 'UnderwaterMemberOffset(' in water

# Intensity must be persisted, clamped, rendered as an extra row and must
# preserve Game Default state/flag restoration at 0%.
for token in (
    'GetUnderwaterColourIntensityPercent',
    'SetUnderwaterColourIntensityPercent',
):
    assert token in header and token in water
for token in (
    'L"UnderwaterIntensityPercent"',
    'underwater_intensity_percent = 100',
    'GetPrivateProfileIntW(L"WaterColours"',
    'UnderwaterBlendTint',
    'const bool custom_effective = config.underwater_custom &&',
    'config.underwater_intensity_percent > 0;',
    'target.original_override',
    'const LinearColor base = original_override ? original :',
    'LinearColor{1.0f, 1.0f, 1.0f, original.a}',
    's_underwater_restore_pending = percent == 0;',
):
    assert token in water, f'Missing intensity blend/persistence: {token}'
assert 'config.underwater_intensity_percent);' in water
assert 'UnderwaterSetOverride(override_location, target.original_override);' in water
assert 'SetUnderwaterColourIntensityPercent(100);' in menu
assert 'AdjustUnderwaterColourIntensity(key == VK_RIGHT ? +1 : -1)' in menu
assert 'GetUnderwaterColourIntensityPercent()' in menu
assert 'L"Underwater intensity: "' in menu

assert 'IsUnderwaterCustomColourEnabled()' in hud
assert 'modules.emplace_back(L"Underwater colours")' in hud
assert 'IsWaterCustomColourEnabled() || IsUnderwaterCustomColourEnabled()' in records
assert 'CheatBit::WaterColour' in cosmetic
assert 'MarkCheatAction(' not in water

assert 'std::vector<std::wstring> ActiveCustomisationLabels()' in records
print('PASS: water surface colour, persistence, lifecycle, menu, reset and PB safety')

# V3: validate packed-bool descriptor offset against adjacent reflected fields
# instead of assuming +0x58 works on the game's Win32 ABI.
assert 'UnderwaterAdjacentBoolBits' in water
assert 'bOverride_FilmWhiteClip' in water
assert 'bOverride_SceneFringeIntensity' in water
assert 'kDescriptorOffsets[]' in water
assert 'if (valid_candidates != 1)' in water
assert 's_underwater_next_apply = GetTickCount64() + 3000;' in water
assert 'if (!prev_valid && !next_valid) continue;' in water

# Latency regression: changing an already-tracked water colour should NOT
# restart a GUObjectArray scan. Persist file updates after input settles,
# independently of on-screen tint application.
change = water.split('void WaterSettingsChanged(', 1)[1].split('void UnderwaterSettingsChanged(', 1)[0]
assert 's_targets.empty() && s_scan_cursor < 0' in change
assert 's_scan_cursor = 0;' in change
assert 's_next_update = 0;' in change
assert 'QueueWaterConfigSave(changed_fields);' in change
assert 's_scan_cursor = WaterConfig().custom ? 0 : -1;' not in change
assert 'WaterSaveConfig();' not in change
underwater_change = water.split('void UnderwaterSettingsChanged(', 1)[1].split('DWORD WINAPI WaterPickerThread', 1)[0]
assert 's_underwater_next_apply = 0;' in underwater_change
assert 'QueueWaterConfigSave(changed_fields);' in underwater_change
assert 'WaterSaveConfig();' not in underwater_change
assert 'kSettingsSaveDebounceMs = 350;' in water
assert 'static_cast<uint8_t>(SurfaceHex | SurfaceEnabled)' in water
assert 'static_cast<uint8_t>(UnderwaterHex | UnderwaterEnabled)' in water
assert 'static_cast<uint8_t>(UnderwaterIntensity | UnderwaterEnabled)' in water
assert 's_settings_save_dirty = failed;' in water
assert 'FlushWaterConfigIfDue();' in water
assert water.index('FlushWaterConfigIfDue();', water.index('void UpdateWaterColours')) < water.index('if (!player || g_restart_requested', water.index('void UpdateWaterColours'))
assert 'kScanCpuBudgetMs = 2;' in water
assert 's_underwater_next_apply = now + (s_underwater_scan_cursor >= 0 ? 16 :' in water
assert '(raised_effect_needed ? 30 : 150)' in water
assert 'if (s_underwater_target.actor && s_underwater_settings_struct)' in water
start = water.index('void UpdateWaterColours(MyCharacter* player)')
update = water[start:]
assert update.index('if (s_targets.empty() && s_scan_cursor >= 0 && (surface_active || world_active))') < update.index('size_t seen = 0;')
assert update.index('size_t seen = 0;') < update.index('if (!discovered_this_tick && !s_targets.empty() &&')
print('PASS: colour-change latency and debounced persistence guards')

# Unlimited underwater intensity: preserve 0–100% interpolation, avoid
# integer overflow in menu adjustment, and never cap the HDR tint at 5.0.
assert 'kMaxUnderwaterColourIntensityPercent' not in header + water + menu
assert 'No artificial upper limit' in water
assert 'if (percent < 0) return;' in water
assert 'const double hdr_strength = static_cast<double>((std::max)(0, percent)) / 100.0;' in water
assert 'const float strength = static_cast<float>(hdr_strength);' in water
assert 'if (strength <= 1.0f)' in water
assert 'const auto boosted = [hdr_strength]' in water
assert '(std::max)(0.0, extrapolated) * hdr_strength;' in water
assert '0.0f, 5.0f' not in water
assert 'const bool our_last_tint = target.applied &&' in water
assert 'UnderwaterColoursEqual(current, target.last_written);' in water
assert 'if (!our_last_tint &&' in water
assert 'GetPrivateProfileIntW(L"WaterColours"' in water
assert 'std::numeric_limits<int>::max' in menu
assert 'static_cast<int64_t>(current)' in menu
assert 'SetUnderwaterColourIntensityPercent(100);' in menu

print("PASS: unlimited underwater intensity, safe HDR maths and legacy controls")

# RGB updates must remain cheap: no repeated file logging, FName allocation
# heavy UFunction reflection, or parallel discovery when a MID is tracked.
source_change = water.split('void WaterSettingsChanged(', 1)[1].split('void UnderwaterSettingsChanged(', 1)[0]
assert 's_applied_logged = false;' not in source_change.split('if (surface_active && !s_targets.empty())', 1)[1].split('} else if (!surface_active)', 1)[0]
assert 's_surface_edit_quiet_until = GetTickCount64() + kSurfaceColourEditQuietMs;' in source_change
assert 'kSurfaceColourEditQuietMs = 900;' in water
assert 'thread_local WaterCallCache cached{};' in water
assert water.count('thread_local WaterCallCache cached{};') == 2
assert 'IsReadable(cached.function, sizeof(UFieldHeader))' in water
assert 'cached = {static_cast<const UObjectHeader*>(mesh)->class_ptr, fn, index, result};' in water
assert 'cached = {static_cast<const UObjectHeader*>(mid)->class_ptr, fn, name, value};' in water
assert 'g_process_event(mid, cached.function, args.data());' in water
assert 'now >= s_surface_edit_quiet_until;' in water
assert 's_scan_cursor >= 0 && (surface_active || world_active) && can_discover' in water
assert 's_applied_logged = false;' in water.split('if (s_player != player) {', 1)[1].split('s_scan_cursor = 0;', 1)[0]
assert 'WaterIsLevelWaterComponent(target.mesh)' not in water.split('void WaterApplyColour(', 1)[1].split('void WaterDiscover(', 1)[0]
assert 'WaterIsLevelWaterComponent(target.actor)' in water.split('bool WaterIsTargetValid(', 1)[1].split('int32_t WaterParam(', 1)[0]
print('PASS: cached surface-colour calls, bounded rescans, no per-colour disk logging')

# Surface intensity is independently saved, remains unlimited and follows
# the existing cached dynamic-material fast path (no per-adjustment scan).
for token in (
    'GetWaterColourIntensityPercent', 'SetWaterColourIntensityPercent',
):
    assert token in header and token in water
for token in (
    'surface_intensity_percent = 100', 'L"SurfaceIntensityPercent"',
    'SurfaceIntensity = 1 << 5', 'WaterReadNativeBaseColour',
    'SurfaceBlendTint', 'target.last_intensity_percent == intensity_percent',
    'target.native_colour', 'const bool surface_active = config.custom &&',
    'config.surface_intensity_percent > 0;',
    's_restore_pending = percent == 0;',
    'SurfaceBlendTint(rgb, native_colour,',
):
    assert token in water, f'Missing surface intensity implementation: {token}'
for token in (
    'L"Surface intensity: "', 'GetWaterColourIntensityPercent()',
    'AdjustWaterColourIntensity(key == VK_RIGHT ? +1 : -1)',
    'SetWaterColourIntensityPercent(100);',
    'if (next > (std::numeric_limits<int>::max)()) return;',
):
    assert token in menu, f'Missing surface intensity menu: {token}'
assert 'UnderwaterBlendTint(' in water and 'SurfaceBlendTint(' in water
assert 'GetUnderwaterColourIntensityPercent()' in menu
assert 'WaterTint(mid, parameter, tint)' in water
assert 'MarkCheatAction(' not in water
print('PASS: surface intensity 0% default / 100% original behaviour / unlimited HDR')

# Regression: FVectorParameterValue is an array of packed structs, while the
# reusable ABI array wrapper stores its buffer as void**. A static_cast to a
# byte pointer does not compile under MSVC; the buffer must be reinterpreted.
native_reader = water.split('bool WaterReadNativeBaseColour(', 1)[1].split(
    '// Match the existing surface colour conversion', 1)[0]
assert 'reinterpret_cast<const uint8_t*>(values.data)' in native_reader
assert 'static_cast<const uint8_t*>(values.data)' not in native_reader
assert 'std::memcpy(&candidate, entry + sizeof(FNameView),' in native_reader
print('PASS: native surface intensity material-array pointer types')
