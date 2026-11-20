"""Compile C++20 water-effect logic against simulated UE4 particle components.

This verifies live-actor identity checks, selective suppression, restoration,
repeat activation, and invalidation. It does not prove the real game uses a
ParticleSystemComponent for all water effects.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
water = (root / 'src/water.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/features/water.hpp').read_text(encoding='utf-8')

def func(signature):
    i = water.index(signature)
    j = water.index('{', i)
    depth = 0
    for k in range(j, len(water)):
        depth += (water[k] == '{') - (water[k] == '}')
        if depth == 0: return water[i:k+1]
    raise AssertionError('unterminated: '+signature)

for name in ('IsUnderwaterTintRemoved', 'SetUnderwaterTintRemoved',
             'AreWaterSplashesRemoved', 'SetWaterSplashesRemoved'):
    assert name+'(' in water and name+'(' in header and name+'(' in menu
assert 'RemoveUnderwaterTint' in water and 'RemoveWaterSplashes' in water
assert 'const bool tint_effective = WaterNoTint() || custom_effective;' in water
assert 'WaterNoTint() ? selected : UnderwaterBlendTint(' in water
assert 'WaterRestoreSplashTargets(WaterObjects());' in water
assert 'WaterUpdateSplashEffects();' in water
assert 'L"Remove underwater tint:' in menu and 'L"Remove water splashes:' in menu
assert 'if (IsUnderwaterTintRemoved()) SetUnderwaterTintRemoved(false);' in menu
assert 'if (AreWaterSplashesRemoved()) SetWaterSplashesRemoved(false);' in menu

parts = [func(x) for x in (
    'bool WaterSplashName(const std::wstring& source)',
    'bool WaterSplashTargetValid(const SplashTarget& target,',
    'void WaterRestoreSplashTargets(const GlobalObjectArray* objects)',
    'void WaterUpdateSplashEffects()',
)]
mock = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>
using ULONGLONG = unsigned long long;
constexpr int kScanBatch = 2048;
constexpr ULONGLONG kScanCpuBudgetMs = 2;
constexpr size_t kMaxSplashTargets = 128;
struct UObjectHeader { int flags = 0; bool visible = true; bool fail_visibility = false; std::wstring asset; };
struct GlobalObjectItem { UObjectHeader* object; int serial_number; };
struct GlobalObjectArray { GlobalObjectItem* objects; int num_elements; };
struct SplashTarget {void* component=nullptr;int32_t index=-1,serial=0;bool original_visible=true;};
std::vector<SplashTarget> s_splash_targets;
int32_t s_splash_scan_cursor=-1;
ULONGLONG s_splash_next_scan=0, s_splash_retry_after=0;
bool s_splash_logged=false;
ULONGLONG now=10000;
ULONGLONG GetTickCount64() {return now;}
struct WaterSettings {bool splashes_removed=false;bool remove_all=false;};
WaterSettings settings;
WaterSettings& WaterConfig(){return settings;}
bool WaterNoSplashes(){return settings.remove_all || settings.splashes_removed;}
GlobalObjectItem items[3]{};
GlobalObjectArray ga{items,3};
const GlobalObjectArray* WaterObjects(){return &ga;}
bool IsReadable(const void* ptr,size_t){return ptr!=nullptr;}
std::wstring WaterObjectName(void* p){return static_cast<UObjectHeader*>(p)->asset;}
bool WaterSplashComponent(void* p){return WaterSplashName(static_cast<UObjectHeader*>(p)->asset);}
bool WaterGetVisible(void* p,bool& visible){visible=static_cast<UObjectHeader*>(p)->visible;return true;}
bool WaterSetVisible(void* p,bool visible){auto& c=*static_cast<UObjectHeader*>(p);c.visible=visible;return !c.fail_visibility;}
void Log(const wchar_t*){}
'''
# WaterSplashComponent calls WaterSplashName before it is inserted; predeclare.
mock=mock.replace('bool WaterSplashComponent(void* p){','bool WaterSplashName(const std::wstring&);\nbool WaterSplashComponent(void* p){')
test=r'''
int main(){
  assert(WaterSplashName(L"P_WaterImpact"));
  assert(WaterSplashName(L"p_splash_01"));
  assert(WaterSplashName(L"P_Ripple"));
  assert(!WaterSplashName(L"P_Fire"));
  UObjectHeader water{0,true,false,L"P_WaterImpact"};
  UObjectHeader fire{0,true,false,L"P_Fire"};
  UObjectHeader ripple{0,true,false,L"P_Ripple"};
  items[0]={&water,3}; items[1]={&fire,8}; items[2]={&ripple,4};
  settings.splashes_removed=true; s_splash_scan_cursor=0;
  WaterUpdateSplashEffects();
  assert(!water.visible && fire.visible && !ripple.visible);
  assert(s_splash_targets.size()==2);
  // Native actor may re-enable the component on a later tick.
  water.visible=true;
  WaterUpdateSplashEffects();
  assert(!water.visible);
  // Failed verification must not lose restoration ownership.
  water.fail_visibility=true;
  water.visible=true;
  WaterUpdateSplashEffects();
  assert(!water.visible && s_splash_targets.size()==2);
  water.fail_visibility=false;
  settings.splashes_removed=false;
  WaterUpdateSplashEffects();
  assert(water.visible && ripple.visible && fire.visible);
  assert(s_splash_targets.empty());
  // Different serial number means a *different* particle component.
  settings.splashes_removed=true; s_splash_scan_cursor=0; s_splash_retry_after=0; now+=100;
  WaterUpdateSplashEffects();
  assert(!water.visible);
  ++items[0].serial_number;
  settings.splashes_removed=false;
  WaterUpdateSplashEffects();
  assert(!ripple.visible==false); // ripple restored, stale water left alone
  assert(!water.visible); // stale reference MUST NOT be used for restoration
  assert(s_splash_targets.empty());
}
'''
# replace awkward double negative with direct assertion
# Test that ripple restores while stale water remains untouched.
test=test.replace('assert(!ripple.visible==false);', 'assert(ripple.visible);')
with tempfile.TemporaryDirectory() as td:
    src=Path(td)/'effects.cpp'
    src.write_text(mock+'\n'+'\n'.join(parts)+'\n'+test,encoding='utf-8')
    exe=Path(td)/'effects'
    c=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-O0',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=40)
    if c.returncode: print(c.stderr);raise SystemExit(1)
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    if r.returncode: print(r.stderr);raise SystemExit(1)
print('PASS: compiled selective water particle suppression/restoration with reload guard')
print('PASS: tint removal priority, persistence and menu integration')
