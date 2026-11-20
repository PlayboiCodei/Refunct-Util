"""Compile the actual ghost renderer with native UE4 actor/controller mocks.
Covers camera independence, movement, focus, mode switching and save compatibility.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/ghost.cpp').read_text(encoding='utf8')
menu = (root/'src/menu.cpp').read_text(encoding='utf8')
input_source = (root/'src/input.cpp').read_text(encoding='utf8')
header = (root/'include/refunct/features/ghost.hpp').read_text(encoding='utf8')
prelude = (root/'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf8')
assert 'IsGhostReplayFreeCameraActive' in header
assert 'IsGhostReplayFreeCameraActive()' in input_source
assert 'if (freecam_key)' in input_source
assert 'L"ReplayCameraMode"' in source
assert 'L"ReplayFirstPerson"' in source  # Migrate old user preference.
assert 's_replay.free_camera = {}' in source
assert 'TouchGhostReplayTiles(player,' in source
assert 'UpdateGhostReplayLifts(player,' in source
assert 'AdvanceReplayNativeButtons(player, progress, now)' in source
assert 'L"Camera: " + GhostReplayCameraLabel()' in menu
assert 'Free cam uses Fly movement' in menu
assert 'hide_for_first_person = s_replay.active && s_replay_camera_mode == 1' in source

main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
using namespace refunct;
static Vec3 seen_position{};
static Rotator seen_rotation{};
static int calls = 0;
static void CaptureReplayCam(void*, void*, void* params) {
    ++calls;
    std::memcpy(&seen_position, params, sizeof(seen_position));
    std::memcpy(&seen_rotation,
        static_cast<unsigned char*>(params) + sizeof(Vec3), sizeof(seen_rotation));
}
static bool Near(float x, float y, float tol=0.05f) { return std::fabs(x-y) <= tol; }
int main() {
    s_loaded_settings = true;
    UObjectHeader camera_type{};
    UObjectHeader camera_actor{};
    camera_actor.class_ptr = &camera_type;
    GlobalObjectItem item{};
    item.object = &camera_actor;
    item.serial_number = 44;
    GlobalObjectArray objects{};
    objects.objects=&item;
    objects.num_elements=objects.max_elements=1;
    g_game_module=&objects;
    g_process_event=&CaptureReplayCam;
    s_replay.camera_class=&camera_type;
    s_replay.camera={&camera_actor,0,44,{}};
    s_replay.move_fn=reinterpret_cast<void*>(0x88);
    s_replay.move_location=0;
    s_replay.move_rotation=sizeof(Vec3);
    s_replay.move_teleport=-1;
    s_replay.active=true;
    PlayerController controller{};
    MyCharacter player{};
    player.controller=&controller;
    s_replay.player=&player;
    const ghost_data::Sample sample{1000,100.f,200.f,300.f,90.f};

    assert(GhostReplayCameraLabel()==L"Locked third person");
    assert(!IsGhostReplayFreeCameraActive());
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Third person");
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"First person");
    assert(!IsGhostReplayFreeCameraActive());
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Free cam");
    assert(IsGhostReplayFreeCameraActive());
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_position.x,100.f));
    assert(Near(seen_position.y,-85.f));
    assert(Near(seen_position.z,435.f));
    assert(Near(seen_rotation.yaw,90.f));

    // The first frame accelerates rather than instantly jumping to max speed.
    s_replay.paused=true;
    g_mock_keys['W']=true;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    const float first_distance=seen_position.y+85.f;
    assert(first_distance>0.f && first_distance<120.f);
    assert(s_replay.free_camera.velocity.y>0.f &&
           s_replay.free_camera.velocity.y<1200.f);
    // Holding the key increases speed without exceeding Movement -> Fly speed.
    const Vec3 before_second=seen_position;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_position.y-before_second.y>first_distance);
    assert(s_replay.free_camera.velocity.y<=1200.1f);
    // Letting go decelerates smoothly, rather than abruptly stopping.
    g_mock_keys['W']=false;
    const Vec3 before_release=seen_position;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_position.y>before_release.y);
    const float release_delta=seen_position.y-before_release.y;
    assert(release_delta<seen_position.y-before_second.y);
    for (int i=0;i<35;++i) {
        g_mock_tick_ms+=100;
        assert(ReplayMoveCamera(sample));
    }
    assert(std::fabs(s_replay.free_camera.velocity.y)<0.1f);

    // Relative controller mouse yaw is smoothed, never snapped to its target.
    controller.control_rotation.yaw=20.f;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_rotation.yaw>90.f && seen_rotation.yaw<110.f);
    for (int i=0;i<10;++i) {g_mock_tick_ms+=100;assert(ReplayMoveCamera(sample));}
    assert(Near(seen_rotation.yaw,110.f,0.1f));
    // Forward movement remains horizontal when looking up.
    const Vec3 before_pitch=seen_position;
    controller.control_rotation.pitch=60.f;
    g_mock_keys['W']=true;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_position.z,before_pitch.z));
    assert(seen_position.x<before_pitch.x);

    // Space / Shift and the user's Fly speed control vertical flight.
    g_mock_keys['W']=false;
    g_mock_keys[VK_SPACE]=true;
    g_fly_speed=2000.f;
    g_mock_tick_ms+=100;
    const Vec3 before_rise=seen_position;
    assert(ReplayMoveCamera(sample));
    assert(seen_position.z>before_rise.z && seen_position.z<before_rise.z+200.f);
    g_mock_keys[VK_SPACE]=false;
    g_mock_keys[VK_SHIFT]=true;
    for (int i=0;i<6;++i) {g_mock_tick_ms+=100;assert(ReplayMoveCamera(sample));}
    assert(seen_position.z<before_rise.z);
    // Opposing Space + Shift cancels the target vertical velocity.
    g_mock_keys[VK_SPACE]=true;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(s_replay.free_camera.velocity.z>-2000.f);
    g_mock_keys[VK_SHIFT]=false;
    g_mock_keys[VK_SPACE]=false;
    for (int i=0;i<30;++i) {g_mock_tick_ms+=100;assert(ReplayMoveCamera(sample));}
    assert(std::fabs(s_replay.free_camera.velocity.z)<0.1f);

    // F5 menu: WASD and Space/Shift still fly, mouse look still responds.
    g_menu_open.store(true);
    g_mock_keys['W']=true;
    const Vec3 before_menu=seen_position;
    controller.control_rotation.yaw=35.f;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(std::hypot(seen_position.x-before_menu.x,seen_position.y-before_menu.y)>0.1f);
    const float menu_yaw=seen_rotation.yaw;
    assert(menu_yaw>110.f);
    // Arrow keys are reserved for menu navigation, not camera look.
    g_mock_keys[VK_RIGHT]=true;
    controller.control_rotation.yaw=35.f;
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_rotation.yaw>=menu_yaw);
    assert(Near(s_replay.free_camera.target_rotation.yaw,125.f,0.1f));
    g_mock_keys[VK_RIGHT]=false;

    // Search text and editable fields suspend flying so WASD can type safely.
    g_mock_menu_search_active=true;
    g_mock_tick_ms+=100;
    const Vec3 before_search=seen_position;
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_position.x,before_search.x) && Near(seen_position.y,before_search.y));
    g_mock_menu_search_active=false;
    g_name_editing.store(true);
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_position.x,before_search.x) && Near(seen_position.y,before_search.y));
    g_name_editing.store(false);
    g_mock_keys['W']=false;
    g_menu_open.store(false);
    g_mock_tick_ms+=100;
    assert(ReplayMoveCamera(sample));
    assert(seen_rotation.yaw<126.f); // No camera snap after F5 closes.

    // 3D ghost remains visible in Free Cam, unlike first person.
    assert(s_replay_camera_mode==2);
    assert(s_replay.active);
    ToggleGhostReplayCameraMode();
    assert(GhostReplayCameraLabel()==L"Locked third person");
    assert(!IsGhostReplayFreeCameraActive());
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_position.y,-85.f));
    ToggleGhostReplayCameraMode();
    ToggleGhostReplayCameraMode();
    ToggleGhostReplayCameraMode();
    assert(IsGhostReplayFreeCameraActive());
    assert(ReplayMoveCamera(sample));
    assert(Near(seen_position.y,-85.f)); // Reenter at current ghost position.
    s_replay={};
    assert(!IsGhostReplayFreeCameraActive());
    assert(GhostReplayCameraLabel()==L"Free cam"); // Saved preference survives stop.
    return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'C++20 compiler required'
with tempfile.TemporaryDirectory(prefix='refunct_ghost_freecam_') as td:
    cpp=Path(td)/'test.cpp'
    cpp.write_text(prelude + 'namespace refunct {' + source.split('namespace refunct {',1)[1] + main,encoding='utf8')
    exe=Path(td)/'test'
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(cpp),'-o',str(exe)],check=True,timeout=90)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: smoothed Free Cam flight, mouse look, F5 movement, text editing and reentry')
