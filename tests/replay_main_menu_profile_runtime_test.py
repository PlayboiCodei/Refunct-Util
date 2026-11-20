"""Compile the real Replay menu rows, handlers and profile switch with isolated storage mocks."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root/'src/menu.cpp').read_text()
restart = (root/'src/restart.cpp').read_text()
page_enum = (root/'include/refunct/state.hpp').read_text().split('enum class Page {', 1)[1].split('};', 1)[0]

def between(s, start, end):
    return s.split(start, 1)[1].split(end, 1)[0]

def case(s, name):
    return re.split(r'\n        case Page::|\n        default:', s.split(f'case Page::{name}:', 1)[1], maxsplit=1)[0]

entries = between(menu, 'std::vector<std::wstring> BuildEntriesForPage(Page page) {', 'void RefreshMenuSearch()')
actions = between(menu, 'void ActivateSelected(MyCharacter* player) {', 'namespace {\n// Left/Right settings')
arrows = between(menu, 'void HandleHorizontalRecordsAndSettings(int key, MyCharacter* player) {', 'void HandleHorizontalMovement(')
helper = 'static void CycleReplayProfile(int direction) {' + between(menu, 'static void CycleReplayProfile(int direction) {', 'void ActivateSelected(')
back = between(menu, 'void GoBack() {', 'void ToggleProgressRiseAnimation()')
assert 'Replay viewer' not in case(entries, 'Ghost')
assert 'SetPage(Page::GhostReplay)' not in case(actions, 'Ghost')
assert 'case Page::GhostReplay: return L"Replay";' in menu
restart_line = next(line.strip() for line in restart.splitlines() if 'if (!IsGhostReplayRequested() && g_page' in line)

code = r'''
#include <algorithm>
#include <cassert>
#include <string>
#include <vector>
#include <cstddef>
enum class Page {''' + page_enum + r'''};
Page g_page = Page::Main;
int g_selection=0, s_maps_page=0;
constexpr int VK_RIGHT=39, VK_LEFT=37;
std::wstring g_active_profile=L"Any%";
std::vector<std::wstring> g_profiles={L"Any%",L"100%",L"Any%_PB"};
std::wstring message, loaded=L"Any%";
std::vector<int> calls;
bool active=false, reject=false;
void Notify(const std::wstring& msg){message=msg;}
void SetPage(Page p){g_page=p;g_selection=0;}
void RefreshCustomMapFiles(){}
void OpenMenuSearch(){SetPage(Page::Search);}
std::wstring MenuHotkeyDisplayName(){return L"F5";}
bool IsGhostReplayRequested(){return active;}
bool IsGhostReplayPaused(){return false;}
std::wstring GhostReplaySourceLabel(){return L"PB";}
std::wstring GhostReplaySpeedLabel(){return L"1x";}
std::wstring GhostReplayStatus(){return L"Stopped";}
std::wstring GhostReplayCameraLabel(){return L"Locked third person";}
bool SelectProfile(const std::wstring& name, bool reset_run){assert(reset_run);calls.push_back(1);if(reject)return false;g_active_profile=name;return true;}
void StopGhostReplayViewer(){calls.push_back(2);active=false;}
void LoadPersonalBest(){assert(!active);calls.push_back(3);loaded=g_active_profile;}
bool StartGhostReplayViewer(){calls.push_back(10);active=!active;return true;}
void ToggleGhostReplaySource(){calls.push_back(12);}
void ToggleGhostReplayPause(){calls.push_back(13);}
void RestartGhostReplay(){calls.push_back(14);}
void SeekGhostReplaySeconds(int n){calls.push_back(n<0?15:16);}
void CycleGhostReplaySpeed(int n){calls.push_back(n>0?17:27);}
void ToggleGhostReplayCameraMode(){calls.push_back(19);}
''' + helper + '\nvoid GoBack(){switch(g_page){case Page::GhostReplay:' + case(back,'GhostReplay') + r''' default: SetPage(Page::Main);}}
std::vector<std::wstring> Rows(Page page){switch(page){case Page::Main:''' + case(entries,'Main') + 'case Page::GhostReplay:' + case(entries,'GhostReplay') + r''' default:return {};}}
void Activate(int i){switch(g_page){case Page::Main:''' + case(actions,'Main') + 'case Page::GhostReplay:' + case(actions,'GhostReplay') + r''' default:assert(false);}}
void Arrow(int key){switch(g_page){case Page::GhostReplay:''' + case(arrows,'GhostReplay') + r''' default:assert(false);}}
void OnRestart(){''' + restart_line + r'''}
int main(){
    auto rows=Rows(Page::Main);
    assert(rows.size()==13 && rows[7]==L"Ghost  >" && rows[8]==L"Replay  >" && rows[9]==L"Profiles  >" && rows[10]==L"Settings  >");
    Activate(8); assert(g_page==Page::GhostReplay);
    rows=Rows(g_page);
    assert(rows.size()==11 && rows[0].starts_with(L"Replay: ") && rows[1]==L"Replay profile: Any%  < >" && rows[2].starts_with(L"Source: ") && rows[10]==L"Back");
    assert(calls.empty()); // Opening or rendering does not reload profiles.
    g_selection=1;
    active=true; Arrow(VK_RIGHT);
    assert((calls==std::vector<int>{1,2,3}) && !active && loaded==L"100%");
    assert(Rows(g_page)[1]==L"Replay profile: 100%  < >");
    OnRestart(); assert(g_page==Page::GhostReplay && g_selection==1);
    calls.clear(); Arrow(VK_LEFT); assert(loaded==L"Any%");
    Arrow(VK_LEFT); assert(loaded==L"Any%_PB");
    Activate(1); assert(loaded==L"Any%");
    // Failed writes must not stop playback or load a different profile.
    calls.clear(); reject=true; active=true; Activate(1);
    assert((calls==std::vector<int>{1}) && active && loaded==L"Any%" && g_active_profile==loaded);
    reject=false; calls.clear(); g_profiles={L"Any%"}; Activate(1); assert(calls.empty() && active);
    g_profiles.clear(); Activate(1); assert(calls.empty() && active && !message.empty());
    g_profiles={L"Any%",L"100%",L"Any%_PB"}; g_active_profile=L"Missing"; Arrow(VK_LEFT); assert(loaded==L"Any%_PB");
    calls.clear(); for(int row: {0,2,3,4,5,6,7,8,9}) Activate(row);
    assert((calls==std::vector<int>{10,12,13,14,15,16,17,19}));
    calls.clear(); for(int row: {2,5,6,7,8,9}){g_selection=row;Arrow(VK_LEFT);}
    assert((calls==std::vector<int>{12,15,16,27,19}));
    Activate(10); assert(g_page==Page::Main);
    Activate(9); assert(g_page==Page::ProfilesHub);
    SetPage(Page::Main); Activate(10); assert(g_page==Page::Settings);
    SetPage(Page::Main); Activate(11); assert(g_page==Page::Search);
    active=false; OnRestart(); assert(g_page==Page::Main);
}
'''
with tempfile.TemporaryDirectory() as td:
    cpp=Path(td)/'test.cpp'; exe=Path(td)/'test'; cpp.write_text(code)
    subprocess.run([shutil.which('g++') or shutil.which('clang++'), '-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=5)
print('PASS: replay main page, profile cycle, playback cleanup, arrows, all rows and restart routing')
