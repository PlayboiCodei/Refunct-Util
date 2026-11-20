"""Compile production incomplete-run and finish paths with mocked Win32/game data.

This tests real C++ function bodies, not a Windows x86 or in-game build.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/records.cpp').read_text('utf-8')
state = (root / 'include/refunct/state.hpp').read_text('utf-8')
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'C++20 compiler needed for native runtime test'


def body(signature: str) -> str:
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError('Unbalanced function ' + signature)


start = state.index('struct PersonalBest {')
end = state.index('\n};', start) + 3
struct = state[start:end]
record_fn = body('void RecordFurthestIncompleteSplits(int button, double elapsed_seconds)')
finish_fn = body('void FinishPersonalBestIfNeeded()')
# End-to-end call site, persistence marker and parse rules are part of contract.
assert 'RecordFurthestIncompleteSplits(observed_button, observed_elapsed);' in source
assert 'L"\\r\\nFurthestButton="' in source
assert 'L"PersonalBest", L"FurthestButton", 0, g_pb_path.c_str()' in source
assert 'if (valid) g_pb.incomplete_reached = reached;' in source
assert 'next.incomplete_reached = 0' in finish_fn
assert 'g_pb.total_seconds < 0.0 && g_pb.incomplete_reached == 0' in finish_fn
assert 'QueueGoldFileWrite(next)' in record_fn
assert 'SavePersonalBestFile(next)' in record_fn
assert 'next.best_segment_seconds' not in record_fn  # no independent gold reset
assert 'next.section_seconds[i] = SectionDurationFromPB' in record_fn

preamble = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include "refunct/section_compare.hpp"
namespace refunct {
constexpr int kRefunctButtonCount=37;
enum class SegmentSource : uint8_t { Unknown, Run, Practice };
'''

mocks = r'''
PersonalBest g_pb{};
struct ButtonSplit { int button_number; double run_seconds; };
std::vector<ButtonSplit> g_button_splits;
std::wstring g_pb_path=L"RefunctUtility\\Profiles\\Any%\\Splits\\records.ini";
std::wstring g_active_profile=L"Any%";
std::wstring g_pb_popup_text;
uint64_t g_pb_popup_expires=0;
bool g_save_personal_bests=true, g_pb_recording_enabled_for_run=true;
bool g_run_started_at_zero=true, g_run_pb_eligible=true;
bool g_timer_complete=false, g_run_finish_processed=false, g_engine_timer_valid=true;
bool g_pb_cheats_allowed_for_run=false;
bool g_practice_active=false;
enum class PracticeOrigin {ProgressRestart, Section};
enum class PracticePhase {Idle, Ready};
PracticePhase g_practice_phase=PracticePhase::Ready;
PracticeOrigin g_practice_origin=PracticeOrigin::Section;
int g_cheat_record_mode=0,g_practice_target_button=0,g_practice_animate_button=0;
double g_elapsed_seconds=0;
bool allow_pb=true, replay=false, queue_ok=true, sync_ok=true;
int queue_calls=0,sync_calls=0, ghost_commits=0, snapshot_calls=0;
PersonalBest last_queued{}, last_sync{};
std::vector<std::wstring> logs;
void Log(const wchar_t* text) { logs.emplace_back(text); }
bool RecordsAllowedForPB() {return allow_pb;}
bool IsGhostReplayRequested() {return replay;}
bool QueueGoldFileWrite(const PersonalBest& best) {
    ++queue_calls; if(!queue_ok) return false;
    last_queued=best; return true;
}
bool SavePersonalBestFile(const PersonalBest& best) {
    ++sync_calls; if(!sync_ok) return false;
    last_sync=best; return true;
}
struct LevelState {int buttons=37,cubes=40,platforms=31;};
struct FakeInstance{unsigned char pad[0x9c]{};LevelState level{};};
FakeInstance game_instance;
void* GetGameInstance() {return &game_instance;}
bool IsReadable(const void*,std::size_t) {return true;}
bool CheatRecordsPB(int){return false;}
void CommitPBGhostOnNewPB(double){++ghost_commits;}
void SaveAutomaticPBProfile(const PersonalBest&){++snapshot_calls;}
std::wstring FormatSplitTime(double){return L"time";}
uint64_t GetTickCount64(){return 200;}

void reset() {
    g_pb=PersonalBest{};g_pb_path=L"RefunctUtility\\Profiles\\Any%\\Splits\\records.ini";
    g_button_splits.clear();g_pb_popup_text.clear();g_pb_popup_expires=0;
    g_save_personal_bests=g_pb_recording_enabled_for_run=true;
    g_run_started_at_zero=g_run_pb_eligible=true;
    g_timer_complete=g_run_finish_processed=false;g_engine_timer_valid=true;
    g_pb_cheats_allowed_for_run=g_practice_active=false;
    g_practice_phase=PracticePhase::Ready;g_practice_origin=PracticeOrigin::Section;
    g_cheat_record_mode=0;g_practice_target_button=g_practice_animate_button=0;
    allow_pb=true;replay=false;queue_ok=sync_ok=true;
    queue_calls=sync_calls=ghost_commits=snapshot_calls=0;
    last_queued=PersonalBest{};last_sync=PersonalBest{};
    g_elapsed_seconds=0;game_instance.level=LevelState{};
    logs.clear();
}
void append_run(int until,double multiplier) {
    g_button_splits.clear();
    for(int i=1;i<=until;++i) g_button_splits.push_back({i,i*multiplier});
}
'''

main = r'''
int mock_main() {
    reset();
    g_pb.minimum_cubes=5;
    g_pb.best_segment_seconds[1]=1.25;
    g_pb.best_segment_sources[1]=SegmentSource::Practice;
    append_run(4,2.0);
    RecordFurthestIncompleteSplits(4,8.0);
    assert(queue_calls==1 && sync_calls==0);
    assert(g_pb.incomplete_reached==4 && g_pb.total_seconds<0);
    assert(g_pb.button_seconds[3]==8.0 && g_pb.button_seconds[4]<0);
    assert(g_pb.section_seconds[3]==2.0 && g_pb.section_seconds[4]<0);
    assert(g_pb.minimum_cubes==5);
    assert(g_pb.best_segment_seconds[1]==1.25);
    assert(g_pb.best_segment_sources[1]==SegmentSource::Practice);
    // A shorter attempt cannot replace the farther reference.
    append_run(3,0.25);RecordFurthestIncompleteSplits(3,0.75);
    assert(queue_calls==1 && g_pb.incomplete_reached==4);
    // Equal reach only updates when faster.
    append_run(4,3.0);RecordFurthestIncompleteSplits(4,12.0);
    assert(queue_calls==1);
    append_run(4,1.5);RecordFurthestIncompleteSplits(4,6.0);
    assert(queue_calls==2 && g_pb.incomplete_reached==4 && g_pb.button_seconds[3]==6.0);
    // Farther runs win regardless of arrival time.
    append_run(5,5.0);RecordFurthestIncompleteSplits(5,25.0);
    assert(queue_calls==3 && g_pb.incomplete_reached==5 && g_pb.button_seconds[4]==25.0);

    // Missing a native contact must not create fabricated button times.
    reset();g_button_splits={{1,1.0},{3,3.0}};
    RecordFurthestIncompleteSplits(3,3.0);
    assert(queue_calls==0);
    // Reject zero time, nan, corrupt times and incomplete zero-length section.
    reset();append_run(3,0.0);RecordFurthestIncompleteSplits(3,0.0);
    assert(queue_calls==0);
    reset();append_run(3,1.0);g_button_splits[2].run_seconds=std::nan("");
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);

    // Manual split file with blank run time has no automatic marker.
    reset();g_pb.button_seconds[0]=4.0;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);
    assert(queue_calls==0 && g_pb.button_seconds[0]==4.0);

    // Finished PB, saving switches, cheat permission and replay are respected.
    reset();g_pb.total_seconds=90.0;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);
    reset();g_save_personal_bests=false;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);
    reset();allow_pb=false;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);
    reset();replay=true;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);
    reset();g_run_started_at_zero=false;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);
    reset();g_pb_recording_enabled_for_run=false;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);assert(queue_calls==0);
    // Queue failure must use synchronous fallback; full failure changes no PB.
    reset();queue_ok=false;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);
    assert(queue_calls==1 && sync_calls==1 && g_pb.incomplete_reached==3);
    reset();queue_ok=sync_ok=false;append_run(3,1.0);
    RecordFurthestIncompleteSplits(3,3.0);
    assert(g_pb.incomplete_reached==0 && g_pb.button_seconds[0]<0);

    // A completed run takes over the SAME record file and clears the marker.
    reset();append_run(36,2.0);RecordFurthestIncompleteSplits(36,72.0);
    assert(g_pb.incomplete_reached==36);
    append_run(37,2.0);g_elapsed_seconds=74.0;g_timer_complete=true;
    FinishPersonalBestIfNeeded();
    assert(g_run_finish_processed && g_pb.total_seconds==74.0);
    assert(g_pb.incomplete_reached==0 && g_pb.button_seconds[36]==74.0);
    assert(ghost_commits==1 && snapshot_calls==1 && sync_calls==1);
    // After a completed PB, incomplete runs cannot overwrite any button.
    append_run(6,0.1);RecordFurthestIncompleteSplits(6,0.6);
    assert(g_pb.total_seconds==74.0 && g_pb.button_seconds[36]==74.0);
    // A slower complete run cannot overwrite a PB either.
    g_run_finish_processed=false;g_elapsed_seconds=80.0;
    FinishPersonalBestIfNeeded();assert(g_pb.total_seconds==74.0);

    // Existing manual split with blank RunTime stays protected even on finish.
    reset();g_pb.button_seconds[0]=4.0;append_run(37,1.0);
    g_elapsed_seconds=37.0;g_timer_complete=true;
    FinishPersonalBestIfNeeded();
    assert(g_pb.total_seconds<0 && g_pb.button_seconds[0]==4.0);
    // A write failure keeps the partial reference until a retry.
    reset();append_run(36,1.0);RecordFurthestIncompleteSplits(36,36.0);
    append_run(37,1.0);g_timer_complete=true;g_elapsed_seconds=37.0;
    sync_ok=false;FinishPersonalBestIfNeeded();
    assert(g_pb.total_seconds<0 && g_pb.incomplete_reached==36);
    assert(ghost_commits==0 && snapshot_calls==0);
    return 0;
}
} // namespace refunct
int main() {return refunct::mock_main();}
'''

with tempfile.TemporaryDirectory(prefix='refunct_partial_splits_') as folder:
    cpp = Path(folder) / 'main.cpp'
    exe = Path(folder) / 'test'
    cpp.write_text(preamble + struct + mocks + record_fn + '\n' + finish_fn + '\n' + main, 'utf-8')
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(cpp), '-o', str(exe)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
print('PASS: one profile INI; furthest incomplete run, PB promotion, manual split protection, independent golds, no replay/cheat bypass, I/O failure')
