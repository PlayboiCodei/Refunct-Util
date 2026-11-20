"""Regression check: moving-platform tint uses actual Unreal reflection names.

Run with `python tests/platform_material_reflection_checks.py`.
A passing static check does not replace a Windows x86 build or in-game validation.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
platforms = (root / "src/platforms.cpp").read_text(encoding="utf-8")
tubes = (root / "src/tubes.cpp").read_text(encoding="utf-8")

# Check the exact reflected function names, not names of local C++ wrappers.
for wrapper, unreal_method in (("PlatformGetMaterial", "GetMaterial"),
                               ("PlatformSetMaterial", "SetMaterial")):
    found = re.search(rf"\b{wrapper}\s*\([^)]*\)\s*\{{(?P<body>.*?)\n\}}", platforms, re.S)
    assert found is not None, f"Missing wrapper {wrapper}"
    assert f'PlatformCall(mesh, {0 if unreal_method == "GetMaterial" else 1})' in found.group("body"), (
        f"{wrapper} must use the validated {unreal_method} signature cache"
    )
    assert f'FindReflectedFunction(mesh, L"{wrapper}")' not in platforms
    assert f'L"{unreal_method}"' in platforms
    assert f'L"{unreal_method}"' in tubes

assert 'FindReflectedFunction(object, method)' in platforms
assert 'result.first = PlatformParam(result.function, L"ElementIndex", L"IntProperty")' in platforms
assert 'result.second = PlatformParam(result.function,' in platforms
assert 'PlatformGetMaterial(target.mesh)' in platforms
assert 'PlatformSetMaterial(target.mesh, target.source)' in platforms
assert 'L"M_Gold_Rough_Yellow"' in platforms
assert 'L"Color"' in platforms
assert 'CreateDynamicMaterialInstance' in platforms
assert 'SetVectorParameterValue' in platforms
assert 'GetPlayRate' in platforms and 'SetPlayRate' in platforms
print("PASS: platform tint invokes Unreal GetMaterial/SetMaterial and retains speed")
