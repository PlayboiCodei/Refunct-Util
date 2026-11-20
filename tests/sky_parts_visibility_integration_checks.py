"""Check all visibility controls, persistence, HUD reporting, menu routing.

The material parameter strings were verified from the supplied cooked PAK.
"""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
source = (r/'src/sky.cpp').read_text(encoding='utf-8')
menu = (r/'src/menu.cpp').read_text(encoding='utf-8')
records = (r/'src/records.cpp').read_text(encoding='utf-8')
header = (r/'include/refunct/features/sky.hpp').read_text(encoding='utf-8')
for part in (
    'enum class SkyEffect : uint8_t { Clouds = 0, Stars = 1 };',
    'bool IsSkyEffectVisible(SkyEffect effect);',
    'void SetSkyEffectVisible(SkyEffect effect, bool visible);',
    'void ResetSkyEffectVisibility();',
):
    assert part in header, part
for part in (
    'L"Cloud opacity", L"Stars brightness"',
    'L"CloudsVisible", L"StarsVisible"',
    'GetPrivateProfileIntW(kSection, kEffectKeys[i], 1,',
    'QueueSettingsValue(kSection, kEffectKeys[i],',
    'ResolveEffectScalarName(s_target.mid, scalar, kEffectParameterNames[index]);',
    'void UpdateSkyEffectVisibility(size_t index)',
    'UpdateSkyEffectVisibility(i);',
    'ResetSkyEffectVisibility()',
    'scalar.has_original = true;',
    'scalar.modified = false;',
):
    assert part in source, part
for part in (
    'Clouds visible:', 'Stars visible:',
    'SetSkyEffectVisible(SkyEffect::Clouds,',
    'SetSkyEffectVisible(SkyEffect::Stars,',
    'else if (i == 4) {',
    'ResetSkyDiscVisibility(); ResetSkyEffectVisibility();',
    'ResetSkyEffectVisibility();\n    ResetFogColours();\n    ResetFogVisibility();',
    'if (g_page == Page::CustomisationSkyVisibility && g_selection == 1)',
    'if (g_page == Page::CustomisationSkyVisibility && g_selection == 2)',
):
    assert part in menu, part
assert 'add(!IsSkyEffectVisible(SkyEffect::Clouds), L"Clouds hidden");' in records
assert 'add(!IsSkyEffectVisible(SkyEffect::Stars), L"Stars hidden");' in records
assert 'MarkCheatAction(' not in source
assert 'SaveSettings();' not in source
assert 'src/fog.cpp' not in source
print('Sky visibility integration, settings, menu, reset, HUD, and cosmetic classification passed')
