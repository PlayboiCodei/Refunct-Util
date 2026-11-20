"""Map-folder bootstrap and native pipe transform regression checks."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
storage=(root/'src/storage.cpp').read_text(encoding='utf8')
maps=(root/'src/maps.cpp').read_text(encoding='utf8')
assert 'maps_dir = g_storage_dir + L"\\\\Maps"' in storage
assert 'EnsureDirectory(maps_dir)' in storage
assert 'Custom maps: cannot create Maps folder' in storage
assert 'Custom maps folder ready:' in storage
assert 'EnsureDirectory(folder)' in maps
assert 'RootComponent' in maps and 'K2_SetWorldLocationAndRotation' in maps
assert 'K2_SetWorldLocation' in maps and 'K2_SetWorldRotation' in maps
assert 'SetWorldScale3D' in maps
# Refunct's native USceneComponent::set_world_scale uses NewScale (not NewScale3D).
assert 'FindClassPropertyOffset(api.set_scale, L"NewScale")' in maps
assert maps.index('FindClassPropertyOffset(api.set_scale, L"NewScale")') < maps.index('FindClassPropertyOffset(api.set_scale, L"NewScale3D")')
assert 'ResolveSceneRoot(actor, root)' in maps
assert 'scene_first = item.kind >= 3' in maps
assert 'SetPoseViaScene(api, item.handle, pose)' in maps
assert 'SetPoseViaActor(api, item.handle, pose)' in maps
assert 'actor.kind == 4 && !ScenePoseSupported(actor.handle)' in maps
assert maps.index('actor.kind == 4 && !ScenePoseSupported(actor.handle)') < maps.index('BeginApplyRoots(false)')
assert 'BeginApplyRoots(true)' in maps
assert 'transform mismatch for ' in maps
assert 'ScenePoseFailureReason(actor.handle)' in maps
assert 'NewScale parameter missing' in maps
# Pipe is kind 4 and uses the scene-component fallback. This source check
# does not presume a user-supplied Star World map was bundled in the archive.
assert 'per_cluster[cluster][4]' in maps
assert 'actor.kind == 4' in maps
print('PASS: Maps folder bootstrap and native pipe scene-transform fallback wired')
