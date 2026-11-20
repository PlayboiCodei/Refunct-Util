"""Native-mock regression for the Shipping-compatible world mesh ghost.

ON: six 3D actors follow recorded coordinates; no camera/HUD projections.
OFF: previous 2D projection still works. No recorded sample or collision is changed.
The test does not simulate Unreal actor spawning or verify actual game rendering.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: no portable C++20 compiler')
    raise SystemExit(0)

source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
assert 'QueueSettingsValue(L"Ghost", L"PassThroughCamera"' in source
assert 'GetPrivateProfileIntW(L"Ghost", L"PassThroughCamera", 1,' in source
assert 'BeginDeferredActorSpawnFromClass' in source
assert 'FinishSpawningActor' in source
assert 'SetActorEnableCollision' in source
assert 'SetCollisionEnabled' in source
assert 'GetCollisionEnabled' in source
assert 'WorldGhostBoxApi' not in source  # No debug geometry path.
assert 'GetActorBounds' in source
assert 'initialization failed at' in source  # Pinpoints native spawn failures in user logs.
spawn_source = source.split('bool SpawnWorldGhostPart(', 1)[1].split('void PlaceWorldGhost(', 1)[0]
assert 'GhostPrepareDeferredMobility(component)' in spawn_source
assert spawn_source.index('GhostPrepareDeferredMobility(component)') < spawn_source.index('api.finish, args.data()')
assert spawn_source.index('set_mesh, args.data()') < spawn_source.index('api.finish, args.data()')
assert 'GhostDeferredMobilityIsMovable(component)' in spawn_source
assert 'L"SetMobility"' not in spawn_source
assert 'distance2 < cutoff * cutoff' not in source # No camera-proximity hiding.
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

std::vector<refunct::Vec3> mesh_locations;
int hud_rectangles = 0;
int screen_projections = 0;

int main() {
    using namespace refunct;
    SceneComponent root{{1000.f, 0.f, 188.f}};
    struct FakeController {void* cls; unsigned char reserved[96]{};};
    int fake_class = 0, fake_projection = 0;
    FakeController camera{&fake_class};
    MyCharacter player{&root, reinterpret_cast<PlayerController*>(&camera)};
    g_player = &player;
    s_loaded_profile = g_pb_path;
    s_loaded_settings = false;
    assert(IsPBGhostPassThroughEnabled()); // Defaults to real 3D ghost.
    s_button_sync = false;
    s_use_fastest_sections = false;
    s_show = true;
    g_pb.total_seconds = 1.0;
    g_elapsed_seconds = 0.0;
    s_playback = {{0,0.f,0.f,188.f,0.f}, {1000,0.f,0.f,188.f,0.f}};
    const auto original = s_playback;

    // Stable native-class/static-asset pointers and six already-spawned actors.
    // Spawning is separately guarded by reflected method/argument checks.
    UObjectHeader gameplay{}, klass{}, mesh{};
    gameplay.index = 0; klass.index = 1; mesh.index = 2;
    UObjectHeader actors[6]{};
    GlobalObjectItem slots[9]{};
    slots[0].object = &gameplay;
    slots[1].object = &klass;
    slots[2].object = &mesh;
    for (int i = 0; i < 6; ++i) {
        actors[i].class_ptr = &klass;
        actors[i].index = i + 3;
        slots[i+3].object = &actors[i];
        slots[i+3].serial_number = i + 100;
    }
    GlobalObjectArray registry{};
    registry.objects = slots;
    registry.num_elements = registry.max_elements = 9;
    g_game_module = &registry;
    int fake_begin = 0, fake_finish = 0, fake_move = 0;
    s_world_api.gameplay = &gameplay;
    s_world_api.actor_class = &klass;
    s_world_api.block_mesh = &mesh;
    s_world_api.begin = &fake_begin;
    s_world_api.finish = &fake_finish;
    s_ghost_motion.actor_class = &klass;
    s_ghost_motion.combined = &fake_move;
    s_ghost_motion.combined_pos = 0;
    s_ghost_motion.combined_rot = 16;
    s_world_parts.player = &player;
    s_world_parts.count = 6;
    s_world_parts.bounds_ready = true;
    s_world_parts_ready = true;
    s_world_parts_hidden = false;
    for (int i=0; i<6; ++i) {
        s_world_parts.parts[i] = {&actors[i],i+3,i+100,{1.f,1.f,1.f}};
    }
    g_process_event = +[](void*, void* fn, void* params) {
        if (fn == s_ghost_motion.combined) {
            Vec3 location{};
            std::memcpy(&location, params, sizeof(location));
            mesh_locations.push_back(location);
        } else if (fn == s_projection.method) {
            auto* bytes = static_cast<std::uint8_t*>(params);
            Vec3 world{};
            std::memcpy(&world, params, sizeof(world));
            const float screen[]{400.f, 700.f-world.z};
            std::memcpy(bytes + 32, screen, sizeof(screen));
            bytes[48]=1;
            ++screen_projections;
        }
    };
    g_draw_rect = +[](void*,LinearColor,float,float,float,float) {
        ++hud_rectangles;
    };

    DrawWorldPBGhost(&player);
    assert(mesh_locations.size() == 6);
    assert(screen_projections == 0 && hud_rectangles == 0);
    assert(std::abs(mesh_locations[0].z - 258.f) < .01f);
    assert(std::abs(mesh_locations[4].z - 135.f) < .01f);
    // The head's top and the foot's bottom match the 176-UE-unit capsule.
    assert(std::abs(mesh_locations[0].z + 18.f - 276.f) < .01f);
    assert(std::abs(mesh_locations[4].z - 35.f - 100.f) < .01f);
    // Repeating a frozen pose must not dispatch six redundant native
    // transform calls. Actors stay alive for subsequent motion.
    mesh_locations.clear();
    DrawWorldPBGhost(&player);
    assert(mesh_locations.empty());
    DrawPBGhost(nullptr, 1920, 1080); // ON must not draw old HUD sprites.
    assert(hud_rectangles == 0 && screen_projections == 0);

    s_playback[0].yaw = s_playback[1].yaw = 90.f;
    mesh_locations.clear();
    DrawWorldPBGhost(&player);
    assert(mesh_locations.size() == 6);
    // Rotating the replay rotates the two arms around the capsule centre.
    assert(std::abs(mesh_locations[2].x - 26.f) < .01f);
    assert(std::abs(mesh_locations[3].x + 26.f) < .01f);
    s_playback[0].yaw = s_playback[1].yaw = 0.f;
    // A world-space, collision-free object must stay visible even if the
    // player/camera occupies the same position: no old proximity hide.
    root.location = {0.f, 0.f, 188.f};
    mesh_locations.clear();
    DrawWorldPBGhost(&player);
    assert(mesh_locations.size() == 6);
    assert(!s_world_parts_hidden && s_world_parts_ready);

    SetPBGhostPassThroughEnabled(false); // Clears all ghost actors.
    assert(!IsPBGhostPassThroughEnabled());
    assert(s_world_parts.count == 0 && !s_world_parts.player);
    s_projection = {};
    s_projection.controller_class = &fake_class;
    s_projection.method = &fake_projection;
    s_projection.world = 0;
    s_projection.screen = 32;
    s_projection.result = 48;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(hud_rectangles == 6 && screen_projections == 2);
    SetPBGhostPassThroughEnabled(true);
    DrawPBGhost(nullptr, 1920, 1080);
    assert(hud_rectangles == 6 && screen_projections == 2);
    for (size_t i = 0; i < original.size(); ++i) {
        assert(s_playback[i].x == original[i].x &&
               s_playback[i].y == original[i].y &&
               s_playback[i].z == original[i].z);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_world_') as directory:
    unit = Path(directory) / 'world_mock.cpp'
    unit.write_text(preamble + source + main, encoding='utf-8')
    exe = Path(directory) / 'world_mock'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(root / 'include'), str(unit), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: world-anchored non-HUD 3D ghost, yaw, toggle, and replay integrity')
