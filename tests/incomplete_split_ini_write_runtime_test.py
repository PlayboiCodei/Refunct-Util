"""Compile the real INI snapshot writer with mocked Win32 file I/O.

Checks the one-file format, metadata, gold retention and safety backups.
Does not constitute a Windows x86 build or game test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/records.cpp').read_text('utf-8')
state = (root / 'include/refunct/state.hpp').read_text('utf-8')
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'A C++20 compiler is required'

def extract(signature: str) -> str:
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == '{': depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0: return source[start:i + 1]
    raise AssertionError('No matched closing brace')

struct = state[state.index('struct PersonalBest {'):]
struct = struct[:struct.index('\n};') + 3]
writer = extract('bool WritePersonalBestSnapshot(const std::wstring& path, const PersonalBest& best,')
assert 'FurthestButton=' in writer
assert 'const int incomplete_button = best.total_seconds < 0.0' in writer

preamble = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <algorithm>
#include <string>
#include <map>
#include <cmath>
#include <cstdint>
#include "refunct/ghost_data_paths.hpp"
#include "refunct/section_compare.hpp"
namespace refunct {
constexpr int kRefunctButtonCount=37;
enum class SegmentSource : uint8_t {Unknown,Run,Practice};
'''

mocks = r'''
using DWORD=unsigned long;
using HANDLE=unsigned long;
constexpr HANDLE INVALID_HANDLE_VALUE=~0ul;
constexpr unsigned long GENERIC_WRITE=0x40000000,CREATE_ALWAYS=2,FILE_ATTRIBUTE_NORMAL=0x80;
constexpr unsigned long MOVEFILE_WRITE_THROUGH=8,MOVEFILE_REPLACE_EXISTING=1;
constexpr int FALSE=0;
std::map<std::wstring,std::string> files;
std::wstring active_temp;
HANDLE active_file=INVALID_HANDLE_VALUE;
std::wstring PBKey(int button) {return L"Button"+std::to_wstring(button);}
std::wstring SectionPBKey(int section){return L"Section"+std::to_wstring(section);}
std::wstring FormatSplitTime(double seconds){
    const long ms=std::lround(seconds*1000.0);
    return std::to_wstring(ms/1000)+L"."+std::to_wstring(ms%1000);
}
bool IsRegularFile(const std::wstring& path){return files.contains(path);}
bool EnsureProfileDirectories(const std::wstring& name){return !name.empty();}
HANDLE CreateFileW(const wchar_t* path,unsigned long,int,void*,unsigned long,unsigned long,void*){
    active_temp=path;files[active_temp]="";active_file=1;return active_file;
}
bool WriteFile(HANDLE handle,const char* text,DWORD length,DWORD* written,void*){
    if(handle!=active_file)return false;
    files[active_temp].append(text,length);*written=length;return true;
}
bool FlushFileBuffers(HANDLE handle){return handle==active_file;}
void CloseHandle(HANDLE handle){if(handle==active_file)active_file=INVALID_HANDLE_VALUE;}
bool CopyFileW(const wchar_t* from,const wchar_t* to,int){
    if(!files.contains(from)||files.contains(to))return false;
    files[to]=files[from];return true;
}
bool MoveFileExW(const wchar_t* from,const wchar_t* to,unsigned long flags){
    if(!files.contains(from)||(!(flags&MOVEFILE_REPLACE_EXISTING)&&files.contains(to)))return false;
    files[to]=files[from];files.erase(from);return true;
}
bool DeleteFileW(const wchar_t* path){return files.erase(path)!=0;}
'''
main = r'''
int check(){
    const std::wstring any=L"RefunctUtility\\Profiles\\Any%\\Splits\\records.ini";
    const std::wstring hundred=L"RefunctUtility\\Profiles\\100%\\Splits\\records.ini";
    PersonalBest partial{};
    partial.incomplete_reached=4;
    partial.button_seconds[0]=2.0;
    partial.button_seconds[1]=4.0;
    partial.button_seconds[2]=6.0;
    partial.button_seconds[3]=8.0;
    partial.section_seconds[0]=2.0;
    partial.best_segment_seconds[0]=1.4;
    partial.best_segment_sources[0]=SegmentSource::Run;
    assert(WritePersonalBestSnapshot(any,partial));
    const auto original=files.at(any);
    assert(original.find("[PersonalBest]\r\nRunTime=\r\nFurthestButton=4\r\n")!=std::string::npos);
    assert(original.find("[ButtonSplits]\r\nButton1=2.0\r\n")!=std::string::npos);
    assert(original.find("[BestSegments]\r\nButton1=1.400\r\n")!=std::string::npos ||
           original.find("[BestSegments]\r\nButton1=1.4\r\n")!=std::string::npos);
    assert(original.find("[BestSegmentSources]\r\nButton1=Run\r\n")!=std::string::npos);
    assert(!files.contains(any+L".partial") && !files.contains(any+L".furthest"));
    // A separate profile keeps a separate copy, not another file for partials.
    assert(WritePersonalBestSnapshot(hundred,partial));
    assert(files.at(hundred)==original);
    // Promoting an eligible completed PB overwrites the SAME INI and clears
    // the incomplete marker. A .bak remains as the existing safety backup.
    PersonalBest completed=partial;
    completed.total_seconds=54.321;
    completed.incomplete_reached=0;
    assert(WritePersonalBestSnapshot(any,completed));
    const auto updated=files.at(any);
    assert(updated.find("RunTime=54.321\r\nFurthestButton=0")!=std::string::npos);
    assert(files.at(any+L".bak")==original);
    assert(files.at(hundred)==original);
    assert(!WritePersonalBestSnapshot(any,completed,false));
    return 0;
}
} // namespace refunct
int main(){return refunct::check();}
'''
with tempfile.TemporaryDirectory(prefix='refunct_partial_ini_') as d:
    cpp=Path(d)/'mock.cpp';exe=Path(d)/'mock'
    cpp.write_text(preamble+struct+mocks+writer+'\n'+main,'utf-8')
    try:
        subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                        '-I',str(root/'include'),str(cpp),'-o',str(exe)],
                       check=True,text=True,capture_output=True)
    except subprocess.CalledProcessError as e:
        print(e.stderr)
        raise
    subprocess.run([str(exe)],check=True,text=True,capture_output=True)
print('PASS: real snapshot writer; one records.ini per profile, partial marker, completed PB promotion, independent golds, automatic .bak')
