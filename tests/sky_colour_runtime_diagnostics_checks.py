"""No-op / missing-parameter regression checks for the sky-colour controller.
No claims about Windows x86 or real-game material rendering.
"""
from pathlib import Path
source = (Path(__file__).resolve().parents[1] / 'src' / 'sky.cpp').read_text()
assert 'reinterpret_cast<const uint8_t*>(item.object)' in source
assert 'std::array<int, kSkyRegionCount> s_diagnostic_stage' in source
assert 's_diagnostic_stage.fill(-1);' in source
assert 'kSkyRegionLabels[region] + L": " + detail' in source
assert '_wcsicmp(runtime_name.c_str(), name) == 0' in source
assert 'SkyStage(region, 4, L"Conv_StringToName argument signature not recognised")' in source
assert 'SkyStage(region, 5, L"Conv_StringToName returned a different/invalid name")' in source
assert 'SkyStage(i, 7, L"MID vector getter unavailable or returned invalid colour")' in source
assert 'SkyStage(i, 8, L"MID vector setter unavailable; no colour written")' in source
assert 'SkyStage(i, 9, (std::wstring(L"override sent to sky MID: #") + colour).c_str())' in source
assert 'SkyStage(i, 10, L"restored original sky MID colour")' in source
assert 'SkyStage(i, 11, L"could not restore original sky MID colour")' in source
assert 'SkyStage(i, 12, L"native sky colour changed since override; keeping native value")' in source
assert 'void UpdateSkyColours(MyCharacter* player)' in source
assert 'if (!s_settings.enabled[i] && !value.modified) continue;' in source
assert 'SaveSoon();' in source
assert 'SetMIDColor(s_target.mid, value.name, value.original)' in source
assert 'Log(line.c_str());' in source
print('Sky runtime diagnostic checks passed')
