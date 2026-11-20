"""Source regression checks for the known colour parameter in Refunct's shipped tube materials.

Native UE4 reflected execution still needs to be verified with the real game.
"""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'src' / 'tubes.cpp').read_text()
for signature in (
    'L"M_Basic"',
    'L"M_Basic_TwoSided"',
    'KnownCookedTubeTint(source, parameter)',
    'L"Default__KismetStringLibrary"',
    'L"Conv_StringToName"',
    'L"InString"',
    'L"Color"',
    'UnrealName(result) != L"Color"',
    's_cooked_color_parameter_ready',
    's_parent_tint_revision',
    'CreateMID(target.component, source)',
    'SetMaterial(target.component, target.source)',
    's_pending_apply',
    'ColourWorkBudget scan_budget(800)',
):
    assert signature in source, f'Missing cooked-tube-colour protection: {signature}'

scope = source.split('bool KnownCookedTubeTint', 1)[1].split('// A base UMaterial', 1)[0]
assert 'ClassName(source) != L"Material"' in scope
assert 'name != L"M_Basic" && name != L"M_Basic_TwoSided"' in scope
assert 'SetMaterial(' not in scope and 'SetTint(' not in scope

scope = source.split('void ObserveNameConversionLibrary', 1)[1].split('// Only the exact base materials', 1)[0]
assert '0x10u' in scope  # only CDOs, not all live objects
assert 'UnrealName(result) != L"Color"' in scope
assert 'g_process_event(object, function, args.data())' in scope

print('Cooked tube colour parameter lookup safeguards passed')
