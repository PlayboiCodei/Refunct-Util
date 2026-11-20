"""Compile the production clock and pillar update functions with counted UE mocks.

Checks idle work, scheduled sampling, explicit edits, lifetime invalidation,
and pending discovery/restore. Does not exercise the Windows ABI.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def function(file, signature):
    source = (root / 'src' / file).read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(signature)

common = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>
using ULONGLONG=unsigned long long;
ULONGLONG now=10000;
ULONGLONG GetTickCount64(){return now;}
struct MyCharacter{};
struct GlobalObjectArray{};
GlobalObjectArray registry;
bool IsReadable(const void* p,size_t){return p!=nullptr;}
'''
clock = common + r'''
constexpr int kMinutesPerDay=1440;
constexpr ULONGLONG kApplyIntervalMs=100,kLockedApplyIntervalMs=16;
enum class ScalarType{None,Integer,Float};
struct ScalarField{ScalarType type=ScalarType::Float; bool speed=false;};
struct NativeClock{void* object=nullptr;ScalarField minute{},speed{ScalarType::Float,true};
 double original_speed=120,last_minute=0,last_written_speed=0;bool speed_owned=false;} s_clock;
struct Config{bool enabled=false,locked=false;int minute=720,speed_percent=100;} s_settings;
int32_t s_scan_cursor=-1;ULONGLONG s_next_scan=0,s_next_apply=0;
bool g_show_game_time_hud=true,s_restore_after_disable=false,s_pending_minute=false,s_reported_speed_unavailable=false;
int s_visible_minute=-1,registry_reads=0,live_reads=0,scans=0,restores=0,writes=0;
int object;double actual_minute=600,actual_speed=120;bool live=true;
void FlushSave(){} Config& TimeSettings(){return s_settings;}
const GlobalObjectArray* TimeObjects(){++registry_reads;return &registry;}
bool LiveClock(const GlobalObjectArray*){++live_reads;return s_clock.object&&live;}
void RestoreClockIfOwned(const GlobalObjectArray*){++restores;s_clock={};actual_speed=120;}
void DiscoverClock(const GlobalObjectArray*){++scans;s_clock.object=&object;live=true;s_scan_cursor=-1;}
bool ReadScalar(void*,ScalarField field,double& result){result=field.speed?actual_speed:actual_minute;return true;}
bool WriteScalar(void*,ScalarField field,double value){++writes;(field.speed?actual_speed:actual_minute)=value;return true;}
int NormalizeMinute(int value){return value%1440;}
double DesiredClockSpeed(){return s_settings.locked?0:120*s_settings.speed_percent/100.;}
void Log(const wchar_t*){} void Notify(const wchar_t*){}
''' + function('time_of_day.cpp','void UpdateCustomTimeOfDay(MyCharacter* player)') + r'''
int main(){
 MyCharacter player,replacement;s_clock.object=&object;
 UpdateCustomTimeOfDay(&player);assert(registry_reads==1 && s_visible_minute==600 && writes==0);
 for(int i=0;i<240;++i) UpdateCustomTimeOfDay(&player);
 assert(registry_reads==1);
 now+=99;UpdateCustomTimeOfDay(&player);assert(registry_reads==1);
 now+=1;actual_minute=601;UpdateCustomTimeOfDay(&player);assert(registry_reads==2 && s_visible_minute==601);
 // Changing character bypasses delay, even when the native clock survives.
 UpdateCustomTimeOfDay(&replacement);assert(registry_reads==3);
 // Settings edit uses the existing immediate scheduling path.
 s_settings.enabled=true;s_settings.locked=true;s_pending_minute=true;s_next_apply=0;
 UpdateCustomTimeOfDay(&replacement);assert(registry_reads==4 && actual_minute==720 && actual_speed==0);
 now+=15;UpdateCustomTimeOfDay(&replacement);assert(registry_reads==4);
 now+=1;actual_minute=721;UpdateCustomTimeOfDay(&replacement);assert(registry_reads==5 && actual_minute==720);
 // Restore does not wait for the watchdog, including with HUD disabled.
 s_settings.enabled=false;g_show_game_time_hud=false;s_restore_after_disable=true;
 UpdateCustomTimeOfDay(&replacement);assert(restores==1 && actual_speed==120);
 for(int i=0;i<240;++i) UpdateCustomTimeOfDay(&replacement);
 assert(registry_reads==6);
 g_show_game_time_hud=true;UpdateCustomTimeOfDay(&replacement);assert(scans==1);
 now+=100;live=false;UpdateCustomTimeOfDay(&replacement);assert(scans==2);
}
'''
pillar = common + r'''
struct Config{bool enabled=true,removed=false,visible=true;uint32_t rgb=0;} cfg;
Config& Settings(){return cfg;}
struct Target{void* mesh=nullptr;void* mid=nullptr;bool removal_saved=false,visual_saved=false;};
std::vector<Target> s_targets;
MyCharacter* s_player=nullptr;int s_scan_cursor=-1;
size_t s_apply_cursor=0,s_pending=0;
bool s_discovery_complete=false,s_discovery_logged=false,s_save_dirty=false;
ULONGLONG s_next_update=0,s_save_due=0;
constexpr size_t kApplyBatch=5;constexpr uint32_t kNoPick=0xffffffffu;
std::atomic_uint32_t s_picker_result{kNoPick};
bool g_restart_requested=false,g_full_level_reload_requested=false,valid=true;
int scans=0,registry_reads=0,applies=0,mesh,mid;
void SaveNow(){} void SetPillarColourHex(uint32_t){}
const GlobalObjectArray* Objects(){++registry_reads;return &registry;}
void Discover(const GlobalObjectArray*){++scans;s_discovery_complete=true;s_scan_cursor=-1;
 s_targets.push_back({&mesh,nullptr,false,false});++s_pending;}
struct ColourWorkBudget{explicit ColourWorkBudget(int){}bool expired(){return false;}};
bool Valid(const Target&,const GlobalObjectArray*){return valid;}
void Apply(Target& t,const GlobalObjectArray*,bool enabled,uint32_t,ULONGLONG){++applies;t.mid=enabled?&mid:nullptr;}
void ApplyRemoval(Target&,const GlobalObjectArray*,bool,ULONGLONG){}
void ApplyVisibility(Target&,bool,bool,ULONGLONG){}
''' + function('pillars.cpp','void UpdatePillarColours(MyCharacter* player)') + r'''
int main(){
 MyCharacter player,replacement;UpdatePillarColours(&player);
 assert(scans==1 && registry_reads==1 && applies==1 && s_pending==0);
 // A completed pending batch deliberately leaves a final immediately due sweep.
 UpdatePillarColours(&player);assert(registry_reads==2 && applies==2);
 for(int i=0;i<240;++i) UpdatePillarColours(&player);
 assert(registry_reads==2 && applies==2);
 now+=100;UpdatePillarColours(&player);assert(registry_reads==3 && applies==3);
 // A queued edit/restore bypasses the normal refresh deadline.
 cfg.enabled=false;s_pending=s_targets.size();s_next_update=0;
 UpdatePillarColours(&player);assert(registry_reads==4 && !s_targets[0].mid);
 for(int i=0;i<240;++i) UpdatePillarColours(&player);assert(registry_reads==4);
 cfg.enabled=true;s_pending=s_targets.size();s_next_update=0;
 UpdatePillarColours(&player);assert(registry_reads==5 && s_targets[0].mid);
 // New world never reuses the old cached pillar or refresh deadline.
 s_next_update=now+100;UpdatePillarColours(&replacement);
 assert(scans==2 && registry_reads==6 && s_targets.size()==1);
 valid=false;now+=100;UpdatePillarColours(&replacement);assert(!s_discovery_complete);
 valid=true;UpdatePillarColours(&replacement);assert(scans==3);
}
'''
with tempfile.TemporaryDirectory() as temp:
    for name, source in [('clock',clock),('pillar',pillar)]:
        cpp=Path(temp)/f'{name}.cpp'; exe=Path(temp)/name
        cpp.write_text(source)
        subprocess.run(['g++','-std=c++20','-O1',str(cpp),'-o',str(exe)],check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: clock and pillar idle registry avoidance, immediate edits, restoration and rediscovery')
