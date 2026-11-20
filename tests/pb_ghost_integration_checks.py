"""Source integration checks; does not claim Windows x86 or in-game validation."""
from pathlib import Path
p = Path(__file__).resolve().parents[1]
read=lambda name:(p/name).read_text(encoding='utf-8')
cmake=read('CMakeLists.txt')
tick=read('src/game_tick.cpp')
record=read('src/records.cpp')
menu=read('src/menu.cpp')
hud=read('src/hud.cpp')
ghost=read('src/ghost.cpp')
assert 'src/ghost.cpp' in cmake and 'refunct_ghost_tests' in cmake
assert '#include "refunct/features/ghost.hpp"' in read('include/refunct/internal_api.hpp')
assert tick.index('UpdateButtonSplits();') < tick.index('UpdatePBGhostRecording(player);') < tick.index('FinishPersonalBestIfNeeded();')
assert 'ResetPBGhostCapture();' in tick
assert record.index('if (!SavePersonalBestFile(next))') < record.index('CommitPBGhostOnNewPB(next.total_seconds);')
assert 'LoadPBGhostForActiveProfile();' in record
assert 'RenamePBGhostProfile(from, new_record);' in record
assert 'MoveFileW(old_dir.c_str(), new_dir.c_str())' in record
assert 'DrawPBGhost(hud, viewport_width, viewport_height);' in hud
assert 'ProjectWorldLocationToScreen' in ghost
assert 'FindClassPropertyOffset(function, name)' in ghost
assert 's_loaded_profile != g_pb_path' in ghost
assert 'CustomMapActive() || CustomMapApplying()' in ghost
assert 'MOVEFILE_REPLACE_EXISTING' in ghost
assert 'GhostPath(g_pb_path)' in ghost
assert 'g_engine_timer_valid' in ghost
assert 'QueueSettingsValue(L"Ghost", L"ShowPBGhost"' in ghost
assert 'SetPBGhostEnabled(!IsPBGhostEnabled());' in menu
assert 'HasPBGhostForActiveProfile()' in ghost and 'PBGhostRecordingStatus()' in ghost
assert 'PBGhostRecordingStatus()' not in menu
assert 'ghost_data::Serialize' in ghost and 'ghost_data::Deserialize' in ghost
assert 'g_run_started_at_zero' in ghost
assert 'g_practice_active' in ghost
assert 's_playback.clear();' in ghost
print('PASS: PB ghost storage, playback, UI and PB transaction integration')
