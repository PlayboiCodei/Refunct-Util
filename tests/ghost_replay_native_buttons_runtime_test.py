"""Compile the production native replay button state machine against a fake
Refunct LevelState and check contacts, acknowledgements, failure paths, and
sample-to-native schedule ordering without running the shipping executable.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
source = (root / 'src/ghost.cpp').read_text(encoding='utf8')
prelude = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf8')
impl = source[source.index('bool AdvanceReplayNativeButtons('):source.index('void UpdateGhostReplayViewer(', source.index('bool AdvanceReplayNativeButtons('))]
assert 'ActivatePracticeButton(actor_index, player)' in impl
assert 'progress->buttons == s_replay.next_button + 1' in impl
assert 's_replay.button_timeline.Due(' in impl
assert 'g_run_pb_eligible = false' in impl
assert 'now - s_replay.press_sent_at > 2500' in impl
assert 'BeginGhostReplaySectionDisplay' not in source
sections = (root/'src/sections.cpp').read_text(encoding='utf8')
assert 'BeginGhostReplaySectionDisplay' not in sections
assert 'MoveSectionToMode(moved, SectionPlacement::Raised' not in sections
assert 'UpdatePBGhostRecording(player);' in (root/'src/game_tick.cpp').read_text(encoding='utf8')
assert 'if (!IsGhostReplayRequested()) {' in (root/'src/game_tick.cpp').read_text(encoding='utf8')

# Test the actual native practice coordinate table, not only synthetic fixtures.
practice = (root/'src/practice.cpp').read_text(encoding='utf8')
start = practice.index('static constexpr std::array<Vec3, kRefunctButtonCount> kPracticeButtonLocations')
vanilla_locations = practice[start:practice.index('}};', start)+3]
mock = 'namespace refunct {' + source.split('namespace refunct {',1)[1] + '\nnamespace refunct {\n' + vanilla_locations + '\n}\n' + r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    MyCharacter player{};
    // Three consecutive recorded contacts with exact native confirmation.
    s_replay={}; g_mock_level_state={};
    g_mock_cheat_marks=0; g_mock_native_button_calls=0;
    s_replay.duration_ms=350;
    s_replay.last_tick=1000;
    s_replay.button_timeline.duration_ms=350;
    assert(s_replay.button_timeline.Add(100));
    assert(s_replay.button_timeline.Add(200));
    assert(s_replay.button_timeline.Add(300));
    assert(!s_replay.button_timeline.Add(250)); // Reject reverse contacts.
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1050));
    assert(g_mock_native_button_calls==0);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1100));
    assert(g_mock_native_button_calls==1 && g_mock_level_state.buttons==1);
    assert(s_replay.awaiting_press && s_replay.next_button==0);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1150));
    assert(s_replay.next_button==1 && !s_replay.awaiting_press);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1250));
    assert(g_mock_native_button_calls==2 && g_mock_level_state.buttons==2);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1300));
    assert(s_replay.next_button==2);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1400));
    assert(g_mock_native_button_calls==3 && g_mock_level_state.buttons==3);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,1450));
    assert(s_replay.next_button==3 && !s_replay.awaiting_press);
    assert(g_mock_native_button_calls==3); // No duplicate press after ending.
    assert(g_mock_cheat_marks==1); // Once, not per-frame log spam.
    assert(g_recording_tainted && !g_run_pb_eligible);

    // A native contact call that never advances LevelState times out safely.
    s_replay={}; g_mock_level_state={};
    g_mock_native_button_auto_confirm=false;
    s_replay.duration_ms=400;
    s_replay.button_timeline.duration_ms=400;
    assert(s_replay.button_timeline.Add(100));
    s_replay.last_tick=2000;
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,2100));
    assert(s_replay.awaiting_press && g_mock_level_state.buttons==0);
    assert(AdvanceReplayNativeButtons(&player, &g_mock_level_state,4500));
    assert(!AdvanceReplayNativeButtons(&player, &g_mock_level_state,4601));
    g_mock_native_button_auto_confirm=true;

    // Physical/unexpected progression must NOT be mistaken for a replay press.
    s_replay={}; g_mock_level_state={};
    s_replay.duration_ms=400; s_replay.button_timeline.duration_ms=400;
    assert(s_replay.button_timeline.Add(100));
    ++g_mock_level_state.buttons;
    assert(!AdvanceReplayNativeButtons(&player, &g_mock_level_state,5000));
    // Missing actor cache stops without creating a native contact.
    g_mock_level_state={};s_replay={};
    s_replay.duration_ms=400;s_replay.button_timeline.duration_ms=400;
    s_replay.last_tick=5000;
    assert(s_replay.button_timeline.Add(100));
    g_practice_button_actors_ready=false;
    auto calls=g_mock_native_button_calls;
    assert(!AdvanceReplayNativeButtons(&player, &g_mock_level_state,5100));
    assert(g_mock_native_button_calls==calls);
    g_practice_button_actors_ready=true;

    // Section 7: press the second physical button first. Even when the
    // viewer seeks past both contacts, use each exact split's own position.
    const auto setup=[&](int previous, int count) {
        s_replay={}; g_mock_level_state={};
        s_replay.next_button=previous;
        g_mock_level_state.buttons=previous;
        s_replay.duration_ms=static_cast<std::uint32_t>(count*100);
        s_replay.button_timeline.duration_ms=s_replay.duration_ms;
        for (int i=0; i<count; ++i) {
            assert(s_replay.button_timeline.Add(static_cast<std::uint32_t>((i+1)*100)));
            g_pb.button_seconds[i]=(i+1)*0.1;
        }
        for (int i=0; i<previous; ++i) s_replay.pressed_buttons[i]=true;
        s_replay.paused=true;
        s_replay.elapsed_ms=s_replay.duration_ms;
        s_replay.last_tick=6000;
        g_mock_button_bounds_calls=0;
        g_mock_button_spawn_calls=0;
        g_mock_button_bounds_ready.fill(true);
        s_playback.clear();
        for (auto& gold : s_fastest) gold.clear();
    };
    setup(6,8);
    g_mock_button_centers[6]={0,0,0};
    g_mock_button_centers[7]={600,0,0};
    s_playback={{600,600,0,100,0},{700,605,0,100,0},{800,5,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==7 && g_mock_level_state.buttons==7);
    assert(s_replay.pressed_buttons[7] && !s_replay.pressed_buttons[6]);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==6 && g_mock_level_state.buttons==8);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6002));
    assert(!s_replay.awaiting_press && s_replay.next_button==8);
    assert(g_mock_button_bounds_calls==2);
    assert(g_mock_button_spawn_calls==0);

    // Section 26: three buttons in non-canonical order. First contact is
    // interpolated between samples, not using the pose after a seek.
    setup(28,31);
    g_mock_button_centers[28]={0,0,0};
    g_mock_button_centers[29]={600,0,0};
    g_mock_button_centers[30]={0,600,0};
    s_playback={{2850,0,590,100,0},{2950,0,610,100,0},
                {3000,0,0,100,0},{3100,600,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==30);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==28);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6002));
    assert(g_mock_last_physical_button==29);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6003));
    assert(s_replay.next_button==31);

    // Source isolation: use the gold segment's own ending pose, then the
    // PB contact if the following gold is unavailable.
    setup(6,8);
    s_replay.fastest=true;
    g_pb.best_segment_seconds[6]=0.25;
    s_fastest[6]={{0,0,0,100,0},{250,605,0,100,0}};
    s_playback={{600,0,0,100,0},{700,0,0,100,0},{800,0,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==7);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==6);

    // Independently saved golds can revisit a physical button. Use the
    // remaining actor to complete native progression, never end playback.
    setup(6,8);
    s_replay.fastest=true;
    g_pb.best_segment_seconds[7]=0.2;
    s_fastest[6]={{0,0,0,100,0},{250,600,0,100,0}};
    s_fastest[7]={{0,0,0,100,0},{200,600,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==7);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==6);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6002));
    assert(s_replay.next_button==8);

    // Ambiguous, far-away and missing samples do not end the viewer.
    // The old bounds query can fail for every actor without affecting replay.
    setup(6,8);
    g_mock_button_bounds_ready.fill(false);
    s_playback={{700,300,0,100,0},{800,300,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==6);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==7);
    assert(g_mock_button_bounds_calls==2);
    setup(6,8);
    s_playback={{700,5000,0,100,0},{800,5000,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==7);
    setup(6,8);
    g_mock_button_bounds_ready.fill(false);
    s_playback={{700,0,0,100,0},{800,600,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==6 && g_mock_button_bounds_calls==2);
    setup(6,8);
    s_playback={{0,0,0,100,0},{100,600,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==6);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==7);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6002));
    assert(s_replay.next_button==8);
    // Three-button section: if a later contact is closer to an already
    // pressed actor, select the nearest UNPRESSED actor, not a duplicate.
    setup(28,31);
    s_playback={{2900,0,600,100,0},{3000,10,590,100,0},{3100,600,0,100,0}};
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_last_physical_button==30);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6001));
    assert(g_mock_last_physical_button==28);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6002));
    assert(g_mock_last_physical_button==29);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6003));
    assert(s_replay.next_button==31);
    // All permutations of the five actual multi-button sections, including
    // player positions offset from the centre as at an edge contact.
    g_mock_button_centers = kPracticeButtonLocations;
    for (int start_button=1; start_button<=kRefunctButtonCount; ) {
        const auto section = SectionContainingButton(start_button);
        const int first=section.previous_end_button, end=section.end_button;
        start_button=end+1;
        if (end-first<2) continue;
        std::vector<int> order;
        for(int i=first;i<end;++i) order.push_back(i);
        do {
            setup(first,end);
            g_mock_button_bounds_ready.fill(false);
            for(int i=first;i<end;++i) {
                const auto p=g_mock_button_centers[order[i-first]];
                s_playback.push_back({static_cast<std::uint32_t>((i+1)*100),
                                      p.x+80.f,p.y-60.f,p.z+90.f,0});
            }
            for(int i=first;i<end;++i) {
                assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,
                                                  6000+i-first));
                assert(g_mock_last_physical_button==order[i-first]);
            }
            assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000+end-first));
            assert(s_replay.next_button==end && !s_replay.awaiting_press);
            assert(g_mock_button_bounds_calls>0);
        } while(std::next_permutation(order.begin(),order.end()));
    }
    // The live actor mapping can differ from the old static spawn table.
    setup(6,8);
    g_mock_button_centers[6]={0,0,0};
    g_mock_button_centers[7]={600,0,0};
    g_mock_override_live_centers=true;
    g_mock_live_button_centers[6]={600,0,0};
    g_mock_live_button_centers[7]={0,0,0};
    s_playback={{700,0,0,100,0},{800,600,0,100,0}};
    assert(ResolveReplayButtonActorIndex(6)==7);
    // The recorded native identity wins even if the transform is misleading.
    s_playback[0].contact=ghost_data::ButtonContactTag(7,7);
    g_mock_button_bounds_calls=0;
    assert(ResolveReplayButtonActorIndex(6)==6);
    assert(g_mock_button_bounds_calls==0);
    s_replay.fastest=true;
    g_pb.best_segment_seconds[6]=0.25;
    s_fastest[6]={{0,0,0,100,0},{250,600,0,100,0,0,ghost_data::ButtonContactTag(7,8)}};
    assert(ResolveReplayButtonActorIndex(6)==7);
    // The real capture entry point tags the actor actually passed by the hook.
    g_mock_override_live_centers=false;
    s_replay={}; s_capture={};
    s_capture.started=true; s_capture.usable=true; s_capture.start_clock=10.;
    SceneComponent root{}; PlayerController controller{};
    player.root_component=&root; player.controller=&controller;
    int actor_a=0, actor_b=0;
    g_practice_button_actors[6]=&actor_a; g_practice_button_actors[7]=&actor_b;
    s_capture.samples={{600,600,0,0,0}};
    RecordPBGhostButtonContact(&player,&actor_b,7,10.,0.7);
    assert(s_capture.samples.size()==2 && s_capture.samples.back().contact==ghost_data::ButtonContactTag(7,8));
    RecordPBGhostButtonContact(&player,&actor_a,8,11.,0.8); // wrong run
    assert(s_capture.samples.size()==2);
    RecordPBGhostButtonContact(&player,&actor_a,8,10.,0.8);
    assert(s_capture.samples.back().contact==ghost_data::ButtonContactTag(8,7));
    // Single-button sections don't need coordinate or bounds reads.
    setup(0,1);
    assert(AdvanceReplayNativeButtons(&player,&g_mock_level_state,6000));
    assert(g_mock_button_bounds_calls==0 && g_mock_button_spawn_calls==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_native_button_replay_') as td:
    source_file=Path(td)/'replay_button_test.cpp'
    executable=Path(td)/'replay_button_test'
    source_file.write_text(prelude+mock,encoding='utf8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(source_file),'-o',str(executable)],
                    check=True,timeout=90)
    subprocess.run([str(executable)],check=True,timeout=10)
print('PASS: native confirmation; reversed two/three-button order; seek; gold and PB contacts; duplicate prevention and ambiguous-contact recovery; idle safety')
