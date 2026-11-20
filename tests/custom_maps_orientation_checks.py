from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'src/maps.cpp').read_text(encoding='utf8')
for part in ('ToRotationBasis(', 'OrientationsMatch(', 'ScalesMatch(',
             'PositionsMatch(', 'orientation=', 'actual PRY=', 'actual scale='):
    assert part in s, part
assert 'std::remainder(actual.rotation.pitch - desired.rotation.pitch' not in s
assert 'SetPoseViaScene(api, item.handle, pose)' in s
assert 'QueueOriginalWorldReload(L"native rollback failed")' in s
print('PASS: UE4 equivalent rotations and relative scale checks; targeted failure diagnostics')
