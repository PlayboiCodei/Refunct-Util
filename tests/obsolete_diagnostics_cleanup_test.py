"""Retired diagnostics must not return; native gameplay and log migration remain."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
read = lambda name: (root / name).read_text(encoding='utf-8')
sections, water, tubes, records = map(read, ('src/sections.cpp','src/water.cpp','src/tubes.cpp','src/records.cpp'))
for source, obsolete in ((sections, ('ButtonContactTracking', 'WriteButtonDiagnostic', 'ObserveNativeButtonProgress', 'WriteButtonNativeProbe', 'actor_snapshots', 'RecordButtonContact', 'NativeButtonComponentState')),
                         (water, ('ProbeRaisedWater', 'WaterHeightNativeBrushTop', 'WaterObserveRaisedSnap', 's_water_height_probe')),
                         (tubes, ('DiagnoseTubeMaterial', 'Tube colours diagnostic:')),
                         (records, ('Section vs PB AUDIT:',))):
    for token in obsolete:
        assert token not in source, token
for token in ('CaptureNativeButtonSplit(player, before, after, actor);', 'NotifyNativeTileContactCompleted(actor, player);', 'IsSectionPhysicallyRaised(matched->section, allowed)', 'if (allowed) invoke_native();', 'UpdatePressAllAvailableButtons();', 'UpdateButtonFix();', 'rollback incomplete; restart map'):
    assert token in sections, token
for token in ('UpdateNativeFixedWaterline(player);', 'UpdateRaisedWaterFallback(player);', 'SwimRestoreVolumes();', 'UpdateWaterPhysicsHeight();', 'WaterUpdateReflections(player);', 'WaterUpdateGeometryReflections(player, WaterObjects());', 'UpdateUnderwaterColours(player);'):
    assert token in water, token
assert 'WarnMaterial(conflicting ?' in tubes
assert 'SectionDurationFromRecordedPB(' in records
assert 'button-sync-diagnostics.txt' in read('include/refunct/runtime_logs.hpp')
assert 'button-sync-diagnostics' in read('include/refunct/runtime_logs.hpp')
assert 'performance.cpp' in read('CMakeLists.txt')
print('PASS: retired diagnostic paths absent, native gameplay and log compatibility retained')
