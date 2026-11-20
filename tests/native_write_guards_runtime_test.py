"""Portable C++ simulation of the real Win32 writable-memory guard.

Also statically checks the writer timeout and post-UE4-tick revalidation.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
core = (root / 'src/core.cpp').read_text(encoding='utf-8')
fn = core[core.index('bool IsWritable('):core.index('bool IsRvaInSection(')]
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')
assert 'WaitForSingleObject(s_gold_writer->idle, kDrainTimeoutMs)' in records
assert 'WaitForSingleObject(s_gold_writer->idle, INFINITE)' not in records
assert 'const bool post_tick_valid = valid &&' in tick
assert tick.index('if (g_original_tick) g_original_tick(player, dt);') < tick.index('const bool post_tick_valid') < tick.index('ApplyMovementTuning(player);')

prelude = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
namespace refunct {
using DWORD = unsigned long;
constexpr DWORD MEM_COMMIT=0x1000, MEM_RESERVE=0x2000;
constexpr DWORD PAGE_NOACCESS=0x01, PAGE_READONLY=0x02, PAGE_READWRITE=0x04;
constexpr DWORD PAGE_WRITECOPY=0x08, PAGE_EXECUTE_READ=0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE=0x40, PAGE_EXECUTE_WRITECOPY=0x80;
constexpr DWORD PAGE_GUARD=0x100;
struct MEMORY_BASIC_INFORMATION {void* BaseAddress{};size_t RegionSize{};DWORD State{},Protect{};};
char page[256]{};
DWORD protect=PAGE_READWRITE, state=MEM_COMMIT;
size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION* mbi, size_t sz) {
    mbi->BaseAddress=page;mbi->RegionSize=sizeof(page);
    mbi->State=state;mbi->Protect=protect;
    return sz;
}
}
'''
main = r'''
int main() {
    using namespace refunct;
    assert(IsWritable(page,256));
    assert(IsWritable(page+252,4));
    assert(!IsWritable(page+252,5));
    assert(!IsWritable(nullptr,4));
    assert(!IsWritable(page,0));
    protect=PAGE_READONLY;assert(!IsWritable(page,8));
    protect=PAGE_EXECUTE_READ;assert(!IsWritable(page,8));
    protect=PAGE_GUARD|PAGE_READWRITE;assert(!IsWritable(page,8));
    protect=PAGE_EXECUTE_READWRITE;assert(IsWritable(page,8));
    protect=PAGE_WRITECOPY;assert(IsWritable(page,8));
    protect=PAGE_READWRITE;state=MEM_RESERVE;assert(!IsWritable(page,8));
}
'''
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp)/'native_guard.cpp'
    exe = Path(temp)/'native_guard'
    cpp.write_text(prelude+'\nnamespace refunct {\n'+fn+'\n}\n'+main,encoding='utf-8')
    r = subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)], capture_output=True,text=True,timeout=30)
    assert r.returncode == 0,r.stderr
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: writable-memory permissions/range, bounded PB writer, post-tick pointer recheck')
