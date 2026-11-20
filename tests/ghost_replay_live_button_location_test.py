"""Compile the actual replay-only location resolver with reflected mock actors."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
source = (root / 'src/practice.cpp').read_text()
start = source.index('bool ResolveReplayButtonCenter(')
end = source.index('// The 31 BP_LevelRoot_C actors', start)
prelude = r'''
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cstddef>
#include <cassert>
struct Vec3 { float x=0,y=0,z=0; };
struct UObjectHeader {void* class_ptr;};
struct Actor {UObjectHeader header; void* mesh; Vec3 position;};
struct Component {UObjectHeader header; Vec3 position;};
constexpr int kRefunctButtonCount=37;
std::array<void*,37> g_practice_button_actors{};
bool g_practice_button_actors_ready=true;
int actor_class{}, mesh_class{}, actor_fn{}, mesh_fn{};
bool mesh_fn_available=true, actor_fn_available=true;
int result_offset=8;
int FindClassPropertyOffset(void* cls,const wchar_t* property) {
    if(cls==&actor_class && !std::wcscmp(property,L"Button")) return offsetof(Actor,mesh);
    if((cls==&actor_fn || cls==&mesh_fn) && !std::wcscmp(property,L"ReturnValue")) return result_offset;
    return -1;
}
bool IsReadable(const void* ptr,size_t) {return ptr!=nullptr;}
void* FindReflectedFunction(void* ptr,const wchar_t* name) {
    auto* header=static_cast<UObjectHeader*>(ptr);
    if(header->class_ptr==&mesh_class && mesh_fn_available && !std::wcscmp(name,L"K2_GetComponentLocation")) return &mesh_fn;
    if(header->class_ptr==&actor_class && actor_fn_available && !std::wcscmp(name,L"K2_GetActorLocation")) return &actor_fn;
    return nullptr;
}
void (*g_process_event)(void*,void*,void*) = +[](void* object,void* fn,void* args) {
    const Vec3 p=fn==&mesh_fn ? static_cast<Component*>(object)->position : static_cast<Actor*>(object)->position;
    std::memcpy(static_cast<uint8_t*>(args)+result_offset,&p,sizeof(p));
};
'''
main=r'''
int main() {
    Component mesh{{&mesh_class},{600,100,0}};
    Actor actor{{&actor_class},&mesh,{100,200,300}};
    g_practice_button_actors[6]=&actor;
    Vec3 center{};
    assert(ResolveReplayButtonCenter(7,center) && center.x==600 && center.y==100);
    mesh_fn_available=false;
    assert(ResolveReplayButtonCenter(7,center) && center.x==100 && center.z==300);
    actor_fn_available=false;
    assert(!ResolveReplayButtonCenter(7,center));
    actor_fn_available=true; mesh_fn_available=true; result_offset=125;
    assert(!ResolveReplayButtonCenter(7,center));
    result_offset=8; mesh.position.x=INFINITY;
    assert(ResolveReplayButtonCenter(7,center) && center.x==100);
    assert(!ResolveReplayButtonCenter(0,center));
    g_practice_button_actors_ready=false;
    assert(!ResolveReplayButtonCenter(7,center));
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(prelude+source[start:end]+main)
    exe=Path(d)/'test'; subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: production replay location resolver, live mesh, actor fallback, invalid offset safety')
