"""Native C++ test for replay-only section staging; no Windows/UE4 dependency.

Extracts the real replay section lifecycle functions from sections.cpp, then
exercises success, error rollback, stale-world protection and restoring
existing user modes/animations against a deterministic native-world mock.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
ghost = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
headers = (root / 'include/refunct/features/practice.hpp').read_text(encoding='utf-8')
assert 'bool BeginGhostReplaySectionDisplay();' in headers
assert 'void EndGhostReplaySectionDisplay();' in headers
assert 'EndGhostReplaySectionDisplay();' in ghost.split('void StopGhostReplayInternal() {', 1)[1].split('void CreateReplayCamera', 1)[0]
assert 'if (!BeginGhostReplaySectionDisplay())' in ghost.split('void UpdateGhostReplayViewer(MyCharacter* player) {', 1)[1]
assert '!s_sections.replay.active &&' in sections.split('void UpdateSectionPlacements() {', 1)[1]
assert 'now >= s_sections.replay.next_enforce_ms' in sections
assert 's_sections.replay.next_enforce_ms = now + 250' in sections
assert 'Stop ghost replay before changing section positions' in sections
assert 'Stop ghost replay before changing section placements' in sections
start = sections.index('bool BeginGhostReplaySectionDisplay() {')
end = sections.index('// Intercept the real native contact.', start)
impl = sections[start:end]

mock = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
namespace refunct {
using ULONGLONG = std::uint64_t;
constexpr int kPracticeClusterCount=31;
struct Vec3 {float x=0,y=0,z=0;};
struct UObjectHeader {int id=0;};
struct LevelState {int resets=4;};
enum class SectionPlacement : std::uint8_t {Auto=0,Raised=1,Lowered=2};
struct SectionPlacementContext {
    struct Transition {
        bool active=false;
        float initial_z=0, target_z=0;
        ULONGLONG started_ms=0;
    };
    std::array<SectionPlacement,kPracticeClusterCount> modes{};
    std::array<Transition,kPracticeClusterCount> transitions{};
    struct ReplayDisplay {
        bool active=false;
        int world_resets=-1;
        ULONGLONG started_ms=0, next_enforce_ms=0;
        std::array<void*,kPracticeClusterCount> actors{};
        std::array<Vec3,kPracticeClusterCount> original_locations{};
        std::array<SectionPlacement,kPracticeClusterCount> previous_modes{};
        std::array<Transition,kPracticeClusterCount> previous_transitions{};
    } replay;
    bool ready=true;
    int last_resets=4;
} s_sections;
UObjectHeader actors[kPracticeClusterCount]{};
std::array<void*,kPracticeClusterCount> g_practice_level_actors{};
std::array<Vec3,kPracticeClusterCount> positions{};
LevelState state{};
bool g_recording_tainted=false;
bool g_buttons_sync_with_sections=false;
bool g_mock_ready=true;
int g_mock_fail_set=-1;
int g_mock_attempts=0, g_mock_cheats=0, g_mock_sync=0;
std::wstring g_mock_log;
void Log(const wchar_t* line) {g_mock_log=line;}
void Notify(const wchar_t*) {}
ULONGLONG g_tick=1000;
ULONGLONG GetTickCount64() {return g_tick;}
const LevelState* PracticeLevelState() {return &state;}
bool EnsureSectionPlacementsReady() {return g_mock_ready;}
void ResetSectionPlacementInternals() {s_sections={};}
float SectionZ(int i) {return -800.f - 10.f * static_cast<float>(i);}
bool SectionTargetZ(int i,SectionPlacement mode,float& out) {
    if (i<0 || i>=kPracticeClusterCount) return false;
    out=(mode==SectionPlacement::Raised)?100.f:SectionZ(i);
    return true;
}
bool IsReadable(const void* p, std::size_t) {return p!=nullptr;}
bool GetSectionRootLocation(void* a,Vec3& out) {
    for(int i=0;i<kPracticeClusterCount;++i) {
        if(a==g_practice_level_actors[i]) {out=positions[i];return true;}
    }
    return false;
}
bool SetSectionRootLocation(void* a,const Vec3& target) {
    for(int i=0;i<kPracticeClusterCount;++i) {
        if(a!=g_practice_level_actors[i]) continue;
        ++g_mock_attempts;
        if(g_mock_fail_set==i) {
            g_mock_fail_set=-1; // one-time failed call, rollback can succeed
            positions[i]=target; // UE may move before returning failure
            return false;
        }
        positions[i]=target;
        return true;
    }
    return false;
}
bool MoveSectionToMode(int i,SectionPlacement mode,bool) {
    float target_z=0;
    if(!SectionTargetZ(i,mode,target_z)) return false;
    Vec3 loc{};
    if(!GetSectionRootLocation(g_practice_level_actors[i],loc)) return false;
    s_sections.transitions[i].active=false;
    loc.z=target_z;
    return SetSectionRootLocation(g_practice_level_actors[i],loc);
}
bool SyncSectionButtons(bool) {++g_mock_sync;return true;}
enum class CheatAction {Sections};
void MarkCheatAction(CheatAction) {++g_mock_cheats;}
'''

tests = r'''
void ResetFixture() {
    s_sections={};s_sections.ready=true;s_sections.last_resets=4;
    state.resets=4;g_tick=1000;g_mock_ready=true;
    g_mock_cheats=0;g_mock_sync=0;g_mock_attempts=0;g_mock_fail_set=-1;
    g_recording_tainted=false;g_buttons_sync_with_sections=false;
    for(int i=0;i<kPracticeClusterCount;++i) {
        actors[i].id=i+1;
        g_practice_level_actors[i]=&actors[i];
        positions[i]={static_cast<float>(i),static_cast<float>(-i),SectionZ(i)};
        s_sections.modes[i]=SectionPlacement::Auto;
    }
    s_sections.modes[3]=SectionPlacement::Lowered;
    s_sections.modes[5]=SectionPlacement::Raised;
    s_sections.transitions[8]={true,SectionZ(8),100.f,960};
}
void VerifyRestored(const std::array<Vec3,kPracticeClusterCount>& originals) {
    for(int i=0;i<kPracticeClusterCount;++i) {
        assert(positions[i].x==originals[i].x);
        assert(positions[i].y==originals[i].y);
        assert(positions[i].z==originals[i].z);
    }
}
} // namespace refunct
int main() {
    using namespace refunct;
    ResetFixture();
    const auto initial=positions;
    assert(BeginGhostReplaySectionDisplay());
    assert(IsGhostReplaySectionDisplayActive());
    assert(g_mock_cheats==1 && g_recording_tainted);
    for(int i=0;i<kPracticeClusterCount;++i) {
        assert(positions[i].z==100.f);
        assert(s_sections.modes[i]==SectionPlacement::Raised);
    }
    assert(BeginGhostReplaySectionDisplay()); // idempotent
    assert(g_mock_cheats==1 && g_mock_attempts==31);
    g_tick=8000;
    EndGhostReplaySectionDisplay();
    assert(!IsGhostReplaySectionDisplayActive());
    VerifyRestored(initial);
    assert(s_sections.modes[3]==SectionPlacement::Lowered);
    assert(s_sections.modes[5]==SectionPlacement::Raised);
    assert(s_sections.transitions[8].active);
    assert(s_sections.transitions[8].started_ms==7960); // paused by 7000 ms
    EndGhostReplaySectionDisplay(); // idempotent

    ResetFixture();
    g_mock_ready=false;
    assert(!BeginGhostReplaySectionDisplay());
    VerifyRestored(positions);
    assert(g_mock_cheats==0);

    ResetFixture();
    const auto before_error=positions;
    g_mock_fail_set=14;
    assert(!BeginGhostReplaySectionDisplay());
    assert(!IsGhostReplaySectionDisplayActive());
    VerifyRestored(before_error);
    assert(s_sections.modes[3]==SectionPlacement::Lowered);
    assert(s_sections.transitions[8].active);
    assert(g_mock_cheats==0 && !g_recording_tainted);

    ResetFixture();
    assert(BeginGhostReplaySectionDisplay());
    const auto raised=positions;
    ++state.resets; // native map restart, roots are no longer the same world
    EndGhostReplaySectionDisplay();
    assert(!IsGhostReplaySectionDisplayActive());
    VerifyRestored(raised); // MUST NOT restore stale old world actors

    ResetFixture();
    g_buttons_sync_with_sections=true;
    assert(BeginGhostReplaySectionDisplay());
    assert(g_mock_sync==0); // overlay never edits button states
    EndGhostReplaySectionDisplay();
    assert(g_mock_sync==1); // resync once after restoration
    return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'No portable C++ compiler for native regression test'
with tempfile.TemporaryDirectory(prefix='replay_section_scaffold_') as td:
    source = Path(td)/'section_test.cpp'
    binary = Path(td)/'section_test'
    source.write_text(mock+impl+tests,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True,timeout=60)
    subprocess.run([str(binary)],check=True,timeout=12)
print('PASS: replay sections full raise, restoration, user modes/animation, rollback, map restart, sync')
