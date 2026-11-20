"""Compile and exercise the actual automatic PB archive routine using Win32 I/O mocks.

The production function body is extracted verbatim. This is NOT a Windows x86
build or an in-game validation, and does not test Win32 file handle semantics.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'C++20 compiler is required for runtime mock'

def get_cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
    open_brace = source.index('{', start)
    depth = 0
    for at in range(open_brace, len(source)):
        if source[at] == '{':
            depth += 1
        elif source[at] == '}':
            depth -= 1
            if depth == 0:
                return source[start:at + 1]
    raise AssertionError('Unbalanced C++ function')

function = get_cpp_function(records, 'void SaveAutomaticPBProfile(const PersonalBest& new_pb)')
assert 'WritePersonalBestSnapshot(destination, snapshot, false)' in function
assert 'CopyFileW(source_ghost.c_str()' in function
assert 'ScanProfiles(false)' in function
assert 'g_active_profile =' not in function
assert 'SelectProfile(' not in function
assert 'g_pb_path =' not in function
assert '64' in function

header = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <string>
#include <cwctype>
#include "refunct/ghost_data_paths.hpp"
namespace refunct {
static constexpr size_t kRefunctButtonCount = 37;
enum class SegmentSource {Unknown, Run, Practice};
struct PersonalBest {
  double total_seconds = -1;
  int minimum_buttons = 0, minimum_cubes = 0, minimum_platforms = 0;
  std::array<double, kRefunctButtonCount> button_seconds{};
  std::array<double, 31> section_seconds{};
  std::array<double, kRefunctButtonCount> best_segment_seconds{};
  std::array<SegmentSource, kRefunctButtonCount> best_segment_sources{};
};
std::vector<std::wstring> g_profiles;
std::wstring g_active_profile = L"Any%", g_pb_path = L"RefunctUtility\\Profiles\\Any%\\Splits\\records.ini", g_storage_dir=L"RefunctUtility";
bool g_auto_create_pb_profile = false, g_auto_pb_copy_best_segments = true;
bool g_mock_pb_ghost_saved=true;
bool PBGhostSavedForLatestPB() {return g_mock_pb_ghost_saved;}
std::map<std::wstring, PersonalBest> snapshots;
std::set<std::wstring> existing, directories;
std::vector<std::wstring> logs, notices;
int ghost_copies=0;
int segment_copy_calls=0;
constexpr int TRUE = 1;
void Log(const wchar_t* value) { logs.emplace_back(value); }
void Notify(const std::wstring& value) { notices.push_back(value); }
bool IsRegularFile(const std::wstring& path) { return existing.contains(path); }
bool ValidProfileName(const std::wstring& name) {
  if (name.empty() || name.size() > 32 || name.front() == L' ' || name.back() == L' ') return false;
  return name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%_- ()") == std::wstring::npos;
}
std::wstring ProfilePath(const std::wstring& name) {
  return ValidProfileName(name) ? ghost_data_paths::Records(g_storage_dir, name) : L"";
}
constexpr unsigned long INVALID_FILE_ATTRIBUTES = 0xFFFFFFFFu;
unsigned long GetFileAttributesW(const wchar_t* path) {
  return directories.contains(path) ? 0x10 :
         (existing.contains(path) ? 0 : INVALID_FILE_ATTRIBUTES);
}
bool EnsureProfileDirectories(const std::wstring& name) {
  if (!ValidProfileName(name)) return false;
  directories.insert(ghost_data_paths::ProfileDir(g_storage_dir, name));
  return true;
}
void ScanProfiles(bool) {
  g_profiles.clear();
  for (const auto& path : existing) {
    const auto name=ghost_data_paths::ProfileName(path);
    if (!ValidProfileName(name) || ghost_data_paths::Records(g_storage_dir,name)!=path) continue;
    if (g_profiles.size() >= 64) break;
    g_profiles.push_back(name);
  }
}
bool WritePersonalBestSnapshot(const std::wstring& path, const PersonalBest& pb, bool overwrite) {
  if (!overwrite && existing.contains(path)) return false;
  snapshots[path] = pb;
  existing.insert(path);
  return true;
}
void CopyFastestSegmentGhosts(const std::wstring&) {++segment_copy_calls;}
bool CopyFileW(const wchar_t* from, const wchar_t* to, int fail_if_exists) {
  if (!existing.contains(from)) return false;
  if (fail_if_exists && existing.contains(to)) return false;
  existing.insert(to);
  ++ghost_copies;
  return true;
}
'''
main = r'''
int main() {
  using namespace refunct;
  existing.insert(g_pb_path);
  existing.insert(ghost_data_paths::PB(g_storage_dir,g_pb_path));
  PersonalBest pb{};
  pb.total_seconds=91.25;
  pb.minimum_buttons=37;
  pb.button_seconds[0]=3.4;
  pb.section_seconds[0]=3.4;
  pb.best_segment_seconds[0]=2.9;
  pb.best_segment_sources[0]=SegmentSource::Practice;

  // Default OFF must never archive even when a PB is committed.
  SaveAutomaticPBProfile(pb);
  assert(snapshots.empty() && ghost_copies==0);
  g_auto_create_pb_profile = true;
  SaveAutomaticPBProfile(pb);
  assert(snapshots.size()==1 && g_active_profile==L"Any%" && g_pb_path==ProfilePath(L"Any%"));
  const auto& first=snapshots.at(ProfilePath(L"Any%_PB_001"));
  assert(first.total_seconds == 91.25 && first.minimum_buttons==37);
  assert(first.button_seconds[0]==3.4 && first.section_seconds[0]==3.4);
  assert(first.best_segment_seconds[0]==2.9 && first.best_segment_sources[0]==SegmentSource::Practice);
  assert(ghost_copies==1 && existing.contains(ghost_data_paths::PB(g_storage_dir,ProfilePath(L"Any%_PB_001"))));
  assert(segment_copy_calls==1);
  // OFF strips ONLY independently earned fastest segments from snapshots.
  g_auto_pb_copy_best_segments=false;
  SaveAutomaticPBProfile(pb);
  const auto& second=snapshots.at(ProfilePath(L"Any%_PB_002"));
  assert(second.total_seconds==91.25 && second.button_seconds[0]==3.4);
  assert(second.best_segment_seconds[0]==-1 && second.best_segment_sources[0]==SegmentSource::Unknown);
  assert(pb.best_segment_seconds[0]==2.9);
  assert(segment_copy_calls==1);
  // No ghost available: still create a valid PB profile without a replay.
  existing.erase(ghost_data_paths::PB(g_storage_dir,g_pb_path));
  SaveAutomaticPBProfile(pb);
  assert(snapshots.contains(ProfilePath(L"Any%_PB_003")) && ghost_copies==2);
  // A failed/disabled PB ghost save must NEVER copy an older PB replay.
  existing.insert(ghost_data_paths::PB(g_storage_dir,g_pb_path));
  g_mock_pb_ghost_saved=false;
  SaveAutomaticPBProfile(pb);
  assert(snapshots.contains(ProfilePath(L"Any%_PB_004")) && ghost_copies==2);
  g_mock_pb_ghost_saved=true;
  // A long category is truncated to the 32-character maximum.
  g_active_profile=L"A Very Long Main Category Title";
  g_pb_path=ProfilePath(g_active_profile);
  existing.insert(g_pb_path);
  SaveAutomaticPBProfile(pb);
  bool found_long=false;
  for (const auto& [key,value] : snapshots) {
    if (key.find(L"A Very Long Main Category") != std::wstring::npos) {
      assert(key.size() <= ghost_data_paths::ProfilesRoot(g_storage_dir).size()+1+32+20);
      found_long=true;
    }
  }
  assert(found_long);
  // Total profile cap must not overwrite the source PB or create archives.
  snapshots.clear(); existing.clear();
  g_active_profile=L"Any%"; g_pb_path=ProfilePath(g_active_profile);
  existing.insert(g_pb_path);
  for (int n=0; n<63; ++n) existing.insert(ProfilePath(L"Custom_"+std::to_wstring(n)));
  SaveAutomaticPBProfile(pb);
  assert(snapshots.empty() && existing.contains(g_pb_path));
  return 0;
}
} // namespace refunct
int main(){return refunct::main();}
'''
with tempfile.TemporaryDirectory(prefix='refunct_auto_pb_') as directory:
    path = Path(directory)
    cpp = path/'archive_test.cpp'
    exe = path/'archive_test'
    cpp.write_text(header + '\n' + function + '\n' + main, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-O2', '-I', str(root/'include'), str(cpp), '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=10)
print('PASS: actual automatic PB snapshot function (default, carry-on/off, naming, ghost copy, limits)')
