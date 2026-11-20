"""Compile real 3D replay orbit code with controller/mouse and arrow-key mocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/ghost.cpp').read_text(encoding='utf8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf8')
header = (root / 'include/refunct/features/ghost.hpp').read_text(encoding='utf8')
prelude = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf8')
assert 'bool IsGhostReplayOrbitCameraActive();' in header
assert 's_replay_camera_mode == 3' in source
assert 'saved >= 0 && saved <= 3' in source  # Old 0,1,2 stay compatible.
assert 'case 3: return L"Third person";' in source
assert 'default: return L"Locked third person";' in source
assert 'UpdateReplayOrbitCamera(pose, location, rotation)' in source
assert 'scroll wheel up/down to zoom' in menu
assert 'AdvanceReplayNativeButtons(player, progress, now)' in source
assert 'UpdateGhostReplayLifts(player,' in source
assert 'TouchGhostReplayTiles(player,' in source

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
static int move_calls = 0;
void CaptureOrbit(void*, void*, void* args) {
    ++move_calls;
    std::memcpy(&seen_location, args, sizeof(Vec3));
    std::memcpy(&seen_rotation,
        static_cast<unsigned char*>(args) + sizeof(Vec3), sizeof(Rotator));
}
static bool Near(float a,float b,float tolerance=0.3f){return std::fabs(a-b)<tolerance;}
int main() {
    s_loaded_settings=true;
    UObjectHeader camera_class{};
    UObjectHeader actor{};
    actor.class_ptr=&camera_class;
    GlobalObjectItem item{};
    item.object=&actor;
    item.serial_number=77;
    GlobalObjectArray objects{};
    objects.objects=&item;
    objects.num_elements=objects.max_elements=1;
    g_game_module=&objects;
    g_process_event=&CaptureOrbit;
    s_replay.camera_class=&camera_class;
    s_replay.camera={&actor,0,77,{}};
    s_replay.move_fn=reinterpret_cast<void*>(0x34);
    s_replay.move_location=0;
    s_replay.move_rotation=sizeof(Vec3);
    s_replay.move_teleport=-1;
    PlayerController controller{};
    MyCharacter player{};
    player.controller=&controller;
    s_replay.player=&player;
    s_replay.active=true;
    ghost_data::Sample sample{1000,100.f,200.f,300.f,90.f};

    // Current Third person view is still the exact old fixed follow transform.
    assert(GhostReplayCameraLabel()==L"Locked third person");
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_location.x,100.f));
    assert(Near(seen_location.y,-85.f));
    assert(Near(seen_location.z,435.f));
    assert(Near(seen_rotation.yaw,90.f));

    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Third person");
    assert(IsGhostReplayOrbitCameraActive());
    assert(!IsGhostReplayFreeCameraActive());
    assert(ReplayMoveCamera(sample));
    assert(s_replay.orbit_camera.initialized);
    assert(Near(seen_rotation.yaw,90.f));
    assert(Near(seen_rotation.pitch,-18.f));
    const float initial_distance=s_replay.orbit_camera.distance;
    assert(Near(initial_distance,325.f));

    // Mouse-look changes the viewing angle WITHOUT turning the ghost itself.
    controller.control_rotation.yaw=30.f;
    controller.control_rotation.pitch=15.f;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_rotation.yaw>90.f && seen_rotation.yaw<120.f);
    for (int i=0;i<12;++i) {g_mock_tick_ms+=100; assert(ReplayMoveCamera(sample));}
    assert(Near(seen_rotation.yaw,120.f));
    assert(Near(seen_rotation.pitch,-3.f));

    // Wheel-up zooms toward ghost; wheel-down zooms away, both at rest or moving.
    QueueGhostReplayOrbitWheel(+120);
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(s_replay.orbit_camera.distance<initial_distance);
    const float closer=s_replay.orbit_camera.distance;
    QueueGhostReplayOrbitWheel(-240);
    for (int i=0;i<4;++i) {g_mock_tick_ms+=100; assert(ReplayMoveCamera(sample));}
    assert(s_replay.orbit_camera.distance>closer);

    // Menu arrows stay reserved for menu navigation; wheel zoom works with F5 open.
    g_menu_open.store(true);
    const float target_before_menu=s_replay.orbit_camera.target_distance;
    g_mock_keys[VK_UP]=true;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(Near(s_replay.orbit_camera.target_distance,target_before_menu,0.01f));
    QueueGhostReplayOrbitWheel(+120);
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(s_replay.orbit_camera.target_distance<target_before_menu);
    g_mock_keys[VK_UP]=false;
    g_menu_open.store(false);

    // The camera remains tied to ghost movement and its orientation stays independent.
    sample.x=250.f; sample.y=480.f; sample.z=360.f; sample.yaw=120.f;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_rotation.yaw,150.f));
    assert(std::hypot(seen_location.x-sample.x,seen_location.y-sample.y)>90.f);
    // Playback pause does not block camera movement.
    s_replay.paused=true;
    controller.control_rotation.yaw=50.f;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_rotation.yaw>150.f);

    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"First person");
    assert(IsGhostReplayFirstPerson());
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Free cam");
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Locked third person");
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Third person");
    assert(!s_replay.orbit_camera.initialized); // No stale orbit state.
    s_replay={};
    s_replay_orbit_active.store(false); // Native cleanup publishes this state.
    assert(!IsGhostReplayOrbitCameraActive());
    assert(GhostReplayCameraLabel()==L"Third person"); // Mode survives replay stop.
    assert(move_calls>0);
    return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'C++20 compiler required'
with tempfile.TemporaryDirectory(prefix='refunct_replay_orbit_') as td:
    cpp = Path(td) / 'test.cpp'
    cpp.write_text(prelude + 'namespace refunct {' + source.split('namespace refunct {', 1)[1] + main, encoding='utf8')
    exe = Path(td) / 'test'
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(cpp), '-o', str(exe)], check=True, timeout=100)
    subprocess.run([str(exe)], check=True, timeout=10)
print('PASS: locked follow, mouse orbit, wheel zoom, menu isolation, pause, profile mode and clean switching')
