"""Exercise the real native-root writer and transactional map switch with injected failures.

This is a portable C++ memory simulation, NOT an Unreal/Win32 integration test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/maps.cpp').read_text(encoding='utf-8')
writer = source[source.index('bool WriteRoot('):source.index('int s_source_offset =')]
transaction = source[source.index('bool BeginApplyRoots('):source.index('bool ApplyNextActor(')]
load = source[source.index('bool RequestCustomMapLoad('):source.index('bool RequestCustomMapRestore(')]
assert 'if (s_map.active) {' in load.split('if (!CurrentSnapshotValid()) {',1)[1].split('} else {',1)[0]
assert load.index('BeginApplyRoots(false, &parsed)') < load.index('s_map.loaded_map = std::move(parsed)')
assert load.index('BeginApplyRoots(false, &parsed)') < load.index('s_map.current_file = s_map.files[index]')

prelude = r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
namespace refunct {
namespace custom_maps {constexpr size_t kClusterCount = 31;}
struct Vec3 { float x{},y{},z{}; };
struct ActorHandle { void* actor{}; };
struct NativeRoot { ActorHandle handle{}; Vec3 source{}; float speed{}; };
struct Map { struct Cluster { float z{}, rise_speed{}; }; std::array<Cluster,31> clusters{}; };
struct MapState {std::array<NativeRoot,31> roots{}; Map loaded_map{};};
MapState s_map{};
struct SimRoot { Vec3 source; float speed; };
std::array<SimRoot,31> sim{};
int s_source_offset = offsetof(SimRoot, source);
int s_speed_offset = offsetof(SimRoot, speed);
int check_counter=0, failure_check=-1, log_count=0;
bool ValidHandle(const ActorHandle& h) {return h.actor != nullptr;}
bool IsReadable(const void* p, size_t n) {return p && n>0;}
bool IsWritable(const void* p, size_t n) {
    if (!p || !n) return false;
    if (check_counter++ == failure_check) return false;
    return true;
}
void Log(const wchar_t*) {++log_count;}
}
'''

main = r'''
int main() {
    using namespace refunct;
    Map newer{};
    for (int i=0;i<31;++i) {
        sim[i].source={float(i+10), float(i+20), float(i+30)};
        sim[i].speed=float(i+2);
        s_map.roots[i].handle.actor=&sim[i];
        s_map.roots[i].source=sim[i].source;
        s_map.roots[i].speed=sim[i].speed;
        s_map.loaded_map.clusters[i]={float(i+1000),float(i+20)};
        newer.clusters[i]={float(i+2000),float(i+40)};
    }
    assert(BeginApplyRoots(false));
    // Current native state is now the old custom map, not the pristine stock map.
    auto previous=sim;
    // 31 roots * 2 permission preflights + 3 successful root writes * 2:
    // transiently fail the fourth root's first write-permission check.
    check_counter=0; failure_check=68;
    assert(!BeginApplyRoots(false,&newer));
    for (int i=0;i<31;++i) {
        assert(sim[i].source.x==previous[i].source.x);
        assert(sim[i].source.y==previous[i].source.y);
        assert(sim[i].source.z==previous[i].source.z);
        assert(sim[i].speed==previous[i].speed);
    }
    // A successful switch updates all roots exactly once, without compounding.
    check_counter=0; failure_check=-1;
    assert(BeginApplyRoots(false,&newer));
    for (int i=0;i<31;++i) {
        assert(sim[i].source.z==newer.clusters[i].z);
        assert(sim[i].speed==newer.clusters[i].rise_speed);
    }
    assert(BeginApplyRoots(true));
    for (int i=0;i<31;++i) {
        assert(sim[i].source.z==float(i+30));
        assert(sim[i].speed==float(i+2));
    }
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'map_root_test.cpp'
    binary = Path(directory) / 'map_root_test'
    cpp.write_text(prelude + '\nnamespace refunct {\n' + writer + transaction + '\n}\n' + main,
                   encoding='utf-8')
    command = ['clang++', '-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror',
               str(cpp), '-o', str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: map root snapshot, injected mid-switch write failure, rollback, switch, original restoration')
