"""Source-level regression checks for sky MID native parameter lookup.

This does not compile the Windows ASI or test behaviour inside the game.
"""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "src" / "sky.cpp").read_text()

for fragment in (
    "bool SkyVectorValuesOffset(void* instance, int32_t& offset)",
    'SkyName(property->object.class_ptr) != L"ArrayProperty"',
    'FindClassPropertyOffset(cls, L"VectorParameterValues")',
    "constexpr size_t kSkyVectorEntrySize = sizeof(FNameView) + sizeof(LinearColor) + 16;",
    "static_assert(kSkyVectorEntrySize == 40);",
    "entries.count > 0 && entries.count <= 32",
    "entries.capacity >= entries.count && entries.capacity <= 512",
    "static_cast<size_t>(entries.count) * kSkyVectorEntrySize",
    "candidate.index == 0 || candidate.index >= 65536",
    "candidate.number > 255",
    "_wcsicmp(actual.c_str(), name) != 0",
    "value.name = candidate;",
    "value.name_known = true;",
    "s_native_name_retry_after[region] = now + 1000;",
    "if (ResolveNameFromSkyMID(value, mid, name, region)) return true;",
    'FindReflectedFunction(s_name_library, L"Conv_StringToName")',
    "ResolveName(value, s_target.mid, kParameterNames[i], i)",
    'SkyStage(region, 13, L"resolved native sky MID vector parameter")',
):
    assert fragment in source, f"Missing sky safeguard: {fragment}"

# The new scalar fallback has its own optional converter earlier in the file;
# verify ordering inside the original vector-colour resolver only.
vector_resolver = source.split('bool ResolveName(SkyValue& value,', 1)[1]
assert vector_resolver.index("ResolveNameFromSkyMID(value, mid, name, region)") < vector_resolver.index(
    'FindReflectedFunction(s_name_library, L"Conv_StringToName")'
)
assert 'SetSkyColourEnabled(SkyRegion region, bool enabled)' in source
assert 'ResetSkyColours()' in source
print("Sky native parameter fallback source checks passed")
