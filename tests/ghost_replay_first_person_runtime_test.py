"""Exercise both camera poses against a UE4 process-event mock.

The actual CameraActor handoff and model visibility still require in-game testing.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/ghost.cpp').read_text(encoding='utf8')
header = (root/'include/refunct/features/ghost.hpp').read_text(encoding='utf8')
menu = (root/'src/menu.cpp').read_text(encoding='utf8')
prelude = (root/'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf8')
assert 'bool IsGhostReplayFirstPerson();' in header
assert 'void ToggleGhostReplayCameraMode();' in header
assert 'L"ReplayCameraMode"' in source
assert 'QueueSettingsValue(L"Ghost", L"ReplayCameraMode"' in source
assert 'const bool hide_for_first_person = s_replay.active && s_replay_camera_mode == 1;' in source
assert 's_world_parts_hidden != hide_for_first_person' in source
assert 'SetActorHiddenInGame' in source[source.index('void DrawWorldPBGhost('):]
assert 's_replay = {};' in source  # Camera choice is not stored in transient playback.
assert 'L"Camera: " + GhostReplayCameraLabel()' in menu
assert 'else if (i == 9) ToggleGhostReplayCameraMode();' in menu
assert 'else if (i == 10) GoBack();' in menu
assert 'g_selection == 9) ToggleGhostReplayCameraMode();' in menu
# Preserve the original button/tiles/lifts replay pipeline.
assert 'AdvanceReplayNativeButtons(player, progress, now)' in source
assert 'TouchGhostReplayTiles(player,' in source
assert 'UpdateGhostReplayLifts(player,' in source

main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
using namespace refunct;
static Vec3 seen_location{};
static Rotator seen_rotation{};
static unsigned moves=0;
void CaptureCamera(void*, void*, void* args) {
    ++moves;
    std::memcpy(&seen_location, static_cast<unsigned char*>(args), sizeof(Vec3));
    std::memcpy(&seen_rotation, static_cast<unsigned char*>(args) + sizeof(Vec3), sizeof(Rotator));
}
static bool Near(float a, float b) {return std::abs(a-b)<0.001f;}
int main() {
    s_loaded_settings=true; // Mock has no real Win32 settings file.
    assert(!IsGhostReplayFirstPerson());
    assert(GhostReplayCameraLabel()==L"Locked third person");

    UObjectHeader camera_class{};
    UObjectHeader camera_actor{};
    camera_actor.class_ptr=&camera_class;
    camera_actor.index=0;
    GlobalObjectItem item{};
    item.object=&camera_actor;
    item.serial_number=29;
    GlobalObjectArray objects{};
    objects.objects=&item;
    objects.num_elements=1;
    objects.max_elements=1;
    g_game_module=&objects;
    g_process_event=&CaptureCamera;
    s_replay.camera_class=&camera_class;
    s_replay.camera={&camera_actor,0,29,{}};
    s_replay.move_fn=reinterpret_cast<void*>(0x30);
    s_replay.move_location=0;
    s_replay.move_rotation=sizeof(Vec3);
    s_replay.move_teleport=-1;
    const ghost_data::Sample pose{1000,100.f,200.f,300.f,90.f,-35.f};

    assert(ReplayMoveCamera(pose));
    assert(moves==1);
    assert(Near(seen_location.x,100.f));
    assert(Near(seen_rotation.pitch,-53.f));
    assert(Near(seen_rotation.yaw,90.f));
    assert(seen_location.y > -85.f && seen_location.z > 435.f);
    const auto down_location = seen_location;
    auto level = pose; level.pitch = 0.f;
    assert(ReplayMoveCamera(level));
    assert(Near(seen_location.y,-85.f) && Near(seen_location.z,435.f));
    assert(Near(seen_rotation.pitch,-18.f));
    auto up = pose; up.pitch = 45.f;
    assert(ReplayMoveCamera(up));
    assert(Near(seen_rotation.pitch,27.f));
    assert(seen_location.z < 435.f);
    // The orbit follows the same focal point at the same radius.
    constexpr float rad = 0.01745329251994329577f;
    const float distance = 285.f / std::cos(-18.f * rad);
    const float focus = 435.f + distance * std::sin(-18.f * rad);
    assert(Near(down_location.z + distance * std::sin(-53.f * rad),focus));
    assert(Near(seen_location.z + distance * std::sin(27.f * rad),focus));
    auto extreme = pose; extreme.pitch = -89.f;
    assert(ReplayMoveCamera(extreme));
    assert(Near(seen_rotation.pitch,-85.f));
    PlayerController controller{};
    MyCharacter player{};
    player.controller=&controller;
    s_replay.player=&player;
    s_replay.controller=&controller;
    assert(!IsGhostReplayFirstPersonActive(&player));

    ToggleGhostReplayCameraMode(); // Orbit third person
    assert(GhostReplayCameraLabel()==L"Third person");
    ToggleGhostReplayCameraMode(); // First person
    assert(IsGhostReplayFirstPerson());
    assert(GhostReplayCameraLabel()==L"First person");
    assert(ReplayMoveCamera(pose));
    assert(moves==5);
    assert(!IsGhostReplayFirstPersonActive(&player)); // selected, not viewing
    s_replay.active = true;
    assert(IsGhostReplayFirstPersonActive(&player));
    assert(!IsGhostReplayFirstPersonActive(nullptr));
    assert(Near(seen_location.x,100.f) && Near(seen_location.y,200.f));
    assert(Near(seen_location.z,364.f));
    assert(Near(seen_rotation.pitch,-35.f) && Near(seen_rotation.yaw,90.f));

    auto upward = pose;
    upward.pitch = 45.f;
    assert(ReplayMoveCamera(upward));
    assert(Near(seen_rotation.pitch,45.f));

    // Playback stop/rewind does not wipe the camera setting.
    s_replay={};
    assert(IsGhostReplayFirstPerson());
    assert(!IsGhostReplayFirstPersonActive(&player));
    ToggleGhostReplayCameraMode(); // Free cam
    assert(!IsGhostReplayFirstPerson());
    assert(GhostReplayCameraLabel()==L"Free cam");
    ToggleGhostReplayCameraMode(); // Locked third person
    assert(GhostReplayCameraLabel()==L"Locked third person");
    assert(ReplayMoveCamera(pose)==false); // A deleted camera is not usable.
    return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'A C++20 compiler is required'
with tempfile.TemporaryDirectory(prefix='ghost_replay_fp_') as td:
    cpp=Path(td)/'test.cpp'
    cpp.write_text(prelude + 'namespace refunct {' + source.split('namespace refunct {',1)[1] + main,encoding='utf8')
    exe=Path(td)/'test'
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(cpp),'-o',str(exe)],check=True,timeout=100)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: third/first camera transforms, mode switch, replay-state independence and menu')
