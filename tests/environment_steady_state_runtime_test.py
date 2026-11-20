"""Compile real hot paths with deterministic registry/time mocks.
Checks work avoided, immediate edits and lifetime guards, not UE rendering.
"""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i+1]
    raise AssertionError(signature)

fog = (root/'src/fog.cpp').read_text()
shadow = (root/'src/shadows.cpp').read_text()
sky = (root/'src/sky.cpp').read_text()
common = r'''
#include <cassert>
#include <array>
#include <cstdint>
#include <cstddef>
using ULONGLONG=unsigned long long;
ULONGLONG now=10000;
ULONGLONG GetTickCount64(){return now;}
struct UObjectHeader{void* outer=nullptr;void* class_ptr=nullptr;};
struct MyCharacter{};
struct GlobalObjectArray{};
GlobalObjectArray registry;
bool IsReadable(const void* p,size_t){return p!=nullptr;}
'''
fog_mock = common + r'''
int checks=0; bool serial_ok=true;
bool Live(const GlobalObjectArray*,const void* p,int32_t,int32_t){++checks;return p&&serial_ok;}
struct FogTarget {void* component=nullptr;int32_t component_index=0,component_serial=0;
void* actor=nullptr;int32_t actor_index=1,actor_serial=1;void* component_class=nullptr;void* actor_class=nullptr;} s_target;
''' + function(fog,'bool TargetLive(const GlobalObjectArray* objects)') + r'''
int main(){
 int a,b,c; UObjectHeader actor{nullptr,&a},component{&actor,&b};
 s_target.component=&component;s_target.actor=&actor;
 s_target.component_class=&b;s_target.actor_class=&a;
 for(int i=0;i<240;++i) assert(TargetLive(&registry));
 assert(checks==480); // two safe slot checks, zero class-name walks
 component.class_ptr=&c;assert(!TargetLive(&registry));
 component.class_ptr=&b;component.outer=nullptr;assert(!TargetLive(&registry));
 component.outer=&actor;serial_ok=false;assert(!TargetLive(&registry));
}
'''
shadow_mock = common + r'''
struct NativeRef{void* ptr=nullptr;};
struct Light{NativeRef component,owner;bool changed=false;} s_light;
struct Post{NativeRef volume,settings_struct;int settings_offset=-1;bool changed=false;} s_post;
struct Config{bool visible=false,tint_enabled=false;uint32_t tint_rgb=0;} s_config;
int32_t s_scan_index=-1; ULONGLONG s_rescan_after=0;
int object_calls=0,light_calls=0,post_calls=0,polls=0,flushes=0,scans=0;
void PollPicker(){++polls;} void Settings(){} void Flush(){++flushes;}
const GlobalObjectArray* Objects(){++object_calls;return &registry;}
bool Live(const GlobalObjectArray*, NativeRef r){return r.ptr!=nullptr;}
void UpdateLight(){++light_calls;s_light.changed=!s_config.visible;}
void UpdatePost(){++post_calls;s_post.changed=s_config.tint_enabled;}
void Discover(const GlobalObjectArray*){++scans;}
''' + function(shadow,'void UpdateShadowCustomisation(MyCharacter* player)') + r'''
int main(){
 MyCharacter player, replacement;
 UObjectHeader owner,component{&owner};s_light.component.ptr=&component;s_light.owner.ptr=&owner;
 s_scan_index=80; // previous discovery ended early, no requested target missing
 UpdateShadowCustomisation(&player);assert(light_calls==1 && object_calls==1);
 for(int i=0;i<30;++i) UpdateShadowCustomisation(&player);
 assert(light_calls==1 && object_calls==1 && polls==31 && flushes==31);
 now+=999;UpdateShadowCustomisation(&player);assert(light_calls==1);
 now+=1;UpdateShadowCustomisation(&player);assert(light_calls==2);
 s_config.visible=true;UpdateShadowCustomisation(&player);assert(light_calls==3 && !s_light.changed);
 s_config.visible=false;UpdateShadowCustomisation(&player);assert(light_calls==4);
 UpdateShadowCustomisation(&replacement);assert(light_calls==5);
 s_config.tint_enabled=true;s_scan_index=0;
 UpdateShadowCustomisation(&replacement);UpdateShadowCustomisation(&replacement);
 assert(scans==2); // missing requested target still discovers each tick
}
'''
sky_mock = common + r'''
constexpr size_t kSkyRegionCount=1,kSkyEffectCount=1;
struct LinearColor{float r=0,g=0,b=0,a=0;};
struct SkyValue{int name=1;bool name_known=true,modified=false;LinearColor original{},last_written{},desired_colour{};uint32_t desired_rgb=0xFFFFFFFFu;};
struct SkyScalar{bool modified=false;};
struct Target{void* actor=nullptr;void* mid=nullptr;std::array<SkyValue,1> values{};SkyScalar sun_brightness{};std::array<SkyScalar,1> effects{};} s_target;
struct Config{std::array<bool,1> enabled{true},effect_visible{true};bool disc_visible=true;std::array<uint32_t,1> colour{0xFFFFFF};} s_settings;
int32_t s_scan_cursor=0; ULONGLONG s_rescan_at=0; bool s_logged_unavailable=false;
void* s_name_library=nullptr;const wchar_t* kParameterNames[]={L"test"};
int scans=0,checks=0,gets=0,sets=0,conversions=0;bool available=false;int actor,mid,lib;
void PollPicker(){} void Settings(){} void FlushSettings(){}
const GlobalObjectArray* SkyObjects(){return &registry;}
bool TargetLive(const GlobalObjectArray*){++checks;return s_target.actor!=nullptr;}
void ResetTarget(){s_target={};}
void FindSky(const GlobalObjectArray*){++scans;s_scan_cursor=-1;s_rescan_at=now+3000;
 if(available){s_target.actor=&actor;s_target.mid=&mid;s_name_library=&lib;}}
void Log(const wchar_t*){}
void UpdateDiscVisibility(){} void UpdateSkyEffectVisibility(size_t){}
bool ResolveName(SkyValue& v,void*,const wchar_t*,size_t){return v.name_known;}
bool GetMIDColor(void*,int,LinearColor& c){++gets;c={0,0,0,1};return true;}
bool SetMIDColor(void*,int,const LinearColor&){++sets;return true;}
LinearColor ColourFromHex(uint32_t){++conversions;return {1,1,1,1};}
bool SameColour(const LinearColor& a,const LinearColor& b){return a.r==b.r&&a.g==b.g&&a.b==b.b;}
void SkyStage(size_t,int,const wchar_t*){}
#include <cwchar>
#include <string>
#define swprintf_s(buf,fmt,...) std::swprintf(buf,sizeof(buf)/sizeof(*(buf)),fmt,__VA_ARGS__)
''' + function(sky,'void UpdateSkyColours(MyCharacter* player)') + r'''
int main(){
 MyCharacter player;UpdateSkyColours(&player);assert(scans==1);
 for(int i=0;i<240;++i) UpdateSkyColours(&player);assert(scans==1);
 available=true;now+=3000;UpdateSkyColours(&player);assert(scans==2 && sets==1 && conversions==1);
 const int old=checks;for(int i=0;i<240;++i) UpdateSkyColours(&player);
 assert(scans==2 && checks-old==240 && sets==241 && conversions==1);
 s_settings.colour[0]=0x112233;UpdateSkyColours(&player);assert(conversions==2);
 s_target.values[0].name_known=false;now+=3000;UpdateSkyColours(&player);assert(scans==2);
}
'''
with tempfile.TemporaryDirectory() as d:
    for label, source in [('fog', fog_mock), ('shadow', shadow_mock), ('sky', sky_mock)]:
        path=Path(d)/f'{label}.cpp'; exe=Path(d)/label
        path.write_text(source)
        subprocess.run(['g++','-std=c++20',str(path),'-o',str(exe)],check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: cached fog identity, shadow cadence / immediate edits, sky discovery / per-frame colours')
