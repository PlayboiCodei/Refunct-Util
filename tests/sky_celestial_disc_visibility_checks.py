"""Static sky/menu integration checks for the consolidated Moon/Sun toggle."""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
sky=(r/'src/sky.cpp').read_text(encoding='utf8')
menu=(r/'src/menu.cpp').read_text(encoding='utf8')
header=(r/'include/refunct/features/sky.hpp').read_text(encoding='utf8')
for fragment in (
    'bool IsSkyDiscVisible();', 'void SetSkyDiscVisible(bool visible);',
    'void ResetSkyDiscVisibility();',
):
    assert fragment in header, fragment
for fragment in (
    'bool disc_visible = true;',
    'L"SunMoonVisible"', 'L"SunDiscVisible"', 'L"MoonDiscVisible"',
    's_settings.disc_visible = sun && moon;',
    'brightness.has_original = true;',
    'if (!brightness.has_original)',
    'disc brightness restore not confirmed; will retry',
    'void UpdateDiscVisibility()', '    UpdateDiscVisibility();',
):
    assert fragment in sky, fragment
for fragment in (
    'L"Sky  >"', 'Customisation / Sky', 'Moon/Sun visible:',
    'SetSkyDiscVisible(!IsSkyDiscVisible());',
    'ResetSkyDiscVisibility(); ResetSkyEffectVisibility();',
    'else if (i == 4) {',
    'if (g_page == Page::CustomisationSkyVisibility && g_selection == 0)',
):
    assert fragment in menu, fragment
assert 'enum class SkyDisc' not in header
assert 'L"Sun visible:"' not in menu and 'L"Moon visible:"' not in menu
assert 'L"Sky Colours  >"' in menu  # Separate colour page
assert 's_target.sun_height' not in sky
assert 'SkyDisc::Sun' not in menu and 'SkyDisc::Moon' not in menu
assert sky.index('    UpdateDiscVisibility();') < sky.index('        auto& value = s_target.values[i];')
assert 'MarkCheatAction(' not in sky
print('Consolidated Moon/Sun menu, persistence, and restoration safeguards passed')
