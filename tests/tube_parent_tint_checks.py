"""Regression checks for material-name inference for tube colours.

Gameplay shader appearance and Refunct's Windows/x86 ABI require in-game testing.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / 'src/tubes.cpp').read_text()
for marker in (
    'ObserveExistingMaterialInstance(item.object);',
    'ClassName(object) == L"MaterialInstanceConstant"',
    'FindClassPropertyOffset(cls, L"Parent")',
    's_parent_tint[parent]',
    'evidence.ambiguous = true',
    '!it->second.verified || it->second.ambiguous',
    'InferredParentTint(source, parameter)',
    'target.no_tint_revision == s_parent_tint_revision',
    'GetMaterial(target.component)',
    'CreateMID(target.component, source)',
    'SetMaterial(target.component, target.source)',
    's_pending_apply',
    'ColourWorkBudget scan_budget(800)',
):
    assert marker in source, f'Missing material-inference safeguard: {marker}'
# Observing an MIC must be read-only: only the later, isolated per-tube MID
# path may call SetMaterial or SetTint.
observation = source.split('void ObserveExistingMaterialInstance', 1)[1].split('bool InferredParentTint', 1)[0]
assert 'SetMaterial(' not in observation and 'SetTint(' not in observation
print('Tube material-parent inference and restoration checks passed')
