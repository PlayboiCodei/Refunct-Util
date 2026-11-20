
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <new>
#include <atomic>
#include <string>
#include <vector>
#include "refunct/ghost_data.hpp"
#include "refunct/ghost_furthest_data.hpp"
#include "refunct/ghost_data_paths.hpp"
#include "refunct/ghost_replay_progress.hpp"
#include "refunct/section_compare.hpp"
using DWORD=unsigned long;
using BOOL=int;
using HANDLE=void*;
using LPVOID=void*;
#define WINAPI
struct SRWLOCK {int dummy = 0;};
#define SRWLOCK_INIT {}
constexpr DWORD INFINITE=0xFFFFFFFF;
constexpr BOOL FALSE=0, TRUE=1;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_TIMEOUT=258;
inline void AcquireSRWLockExclusive(SRWLOCK*){}
inline void ReleaseSRWLockExclusive(SRWLOCK*){}
inline DWORD WaitForSingleObject(HANDLE,DWORD){return WAIT_OBJECT_0;}
inline BOOL SetEvent(HANDLE){return 1;}
inline BOOL ResetEvent(HANDLE){return 1;}
inline void Sleep(DWORD){}
inline HANDLE CreateEventW(void*,BOOL,BOOL,const wchar_t*){return nullptr;}
inline HANDLE CreateThread(void*,std::size_t,DWORD (WINAPI*)(LPVOID),LPVOID,DWORD,void*){return nullptr;}
inline BOOL CopyFileW(const wchar_t*,const wchar_t*,BOOL){return 1;}

constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=0x10;
constexpr DWORD FILE_ATTRIBUTE_NORMAL=0x80;
constexpr DWORD GENERIC_READ=1, GENERIC_WRITE=2, FILE_SHARE_READ=4;
constexpr DWORD OPEN_EXISTING=3, CREATE_ALWAYS=2, MOVEFILE_REPLACE_EXISTING=1;
constexpr DWORD INVALID_FILE_ATTRIBUTES=0xFFFFFFFF;
constexpr DWORD MAXDWORD=0xFFFFFFFF;
constexpr int GetFileExInfoStandard=0;
inline std::uint64_t g_mock_tick_ms = 1000;
inline std::uint64_t GetTickCount64() { return g_mock_tick_ms; }
// Free-cam test keyboard, independent of native input hooks.
inline std::array<bool,256> g_mock_keys{};
inline short GetAsyncKeyState(int key) {
    return key >= 0 && key < 256 && g_mock_keys[static_cast<size_t>(key)]
        ? static_cast<short>(0x8000) : 0;
}
constexpr int VK_SPACE=32, VK_SHIFT=16, VK_CONTROL=17;
constexpr int VK_LEFT=37, VK_UP=38, VK_RIGHT=39, VK_DOWN=40;
inline bool HasFocusedGameWindow() {return true;}
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))
struct WIN32_FILE_ATTRIBUTE_DATA {DWORD dwFileAttributes=0;DWORD nFileSizeHigh=0,nFileSizeLow=0;};
inline int GetPrivateProfileIntW(const wchar_t*,const wchar_t*,int def,const wchar_t*){return def;}
inline BOOL GetFileAttributesExW(const wchar_t*,int,WIN32_FILE_ATTRIBUTE_DATA*){return 0;}
inline HANDLE CreateFileW(const wchar_t*,DWORD,DWORD,void*,DWORD,DWORD,void*){return INVALID_HANDLE_VALUE;}
inline BOOL ReadFile(HANDLE,void*,DWORD,DWORD*,void*){return 0;}
inline BOOL WriteFile(HANDLE,const void*,DWORD,DWORD*,void*){return 0;}
inline BOOL CloseHandle(HANDLE){return 1;}
inline BOOL MoveFileExW(const wchar_t*,const wchar_t*,DWORD){return 0;}
inline BOOL MoveFileW(const wchar_t*,const wchar_t*){return 0;}
inline BOOL DeleteFileW(const wchar_t*){return 1;}
inline BOOL RemoveDirectoryW(const wchar_t*){return 1;}
inline DWORD GetFileAttributesW(const wchar_t*){return INVALID_FILE_ATTRIBUTES;}
inline BOOL WritePrivateProfileStringW(const wchar_t*,const wchar_t*,const wchar_t*,const wchar_t*){return 1;}
namespace refunct {
// Test fixture: same configurable speed used by the actual Movement -> Fly mode.
inline float g_fly_speed = 1200.f;
struct Vec3 {float x=0,y=0,z=0;};
struct Rotator {float pitch=0,yaw=0,roll=0;};
struct LinearColor {float r,g,b,a;};
struct SceneComponent {Vec3 location;};
struct PlayerController {Rotator control_rotation;};
struct MyCharacter {SceneComponent* root_component=nullptr;PlayerController* controller=nullptr;};
// Replay native tile/lift contacts are tested against their own actor mocks;
// ghost-only test harnesses intentionally skip world contact execution.
enum class GhostReplayTileStatus { Pending, Ready, Unavailable };
enum class GhostReplayLiftStatus { Pending, Ready, Unavailable };
inline GhostReplayTileStatus PrepareGhostReplayTiles(MyCharacter*) {return GhostReplayTileStatus::Unavailable;}
inline GhostReplayLiftStatus PrepareGhostReplayLifts(MyCharacter*) {return GhostReplayLiftStatus::Unavailable;}
inline void ResetGhostReplayTiles() {}
inline void ResetGhostReplayLifts() {}
inline bool GhostReplayTilesChanged(){return false;}
inline bool GhostReplayLiftsChanged(){return false;}
enum class GhostReplayCubeStatus { Pending, Ready, Unavailable };
inline GhostReplayCubeStatus PrepareGhostReplayCubes(MyCharacter*) {return GhostReplayCubeStatus::Unavailable;}
inline bool GhostReplayCubesChanged() {return false;}
inline void ResetGhostReplayCubes() {}
inline void TouchGhostReplayCubes(MyCharacter*,const Vec3&,const Vec3&,bool) {}
inline void TouchGhostReplayTiles(MyCharacter*,const Vec3&,const Vec3&,bool) {}
inline void UpdateGhostReplayLifts(MyCharacter*,const Vec3&,const Vec3&,bool) {}

struct FNameView {uint32_t index=0,number=0;};
struct FStringView {wchar_t* data=nullptr;int32_t count=0,capacity=0;};
struct UObjectHeader {void* class_ptr=nullptr;FNameView name;int32_t index=-1;};
struct GlobalObjectItem {UObjectHeader* object=nullptr;int32_t flags=0;int32_t cluster_index=0;int32_t serial_number=0;};
struct GlobalObjectArray {int32_t first_gc_index=0,last_non_gc_index=0,max_objects_not_considered_by_gc=0;uint8_t open_for_disregard_for_gc=0,padding[3]{};GlobalObjectItem* objects=nullptr;int32_t max_elements=0,num_elements=0;};
constexpr uintptr_t kGlobalObjectArrayRva=0;
void* g_game_module=nullptr;
struct UFieldHeader {UObjectHeader object;void* next=nullptr;};
struct UStructHeader {UFieldHeader field;void* super_struct=nullptr;void* children=nullptr;};
constexpr int kRefunctButtonCount = 37;
struct ButtonSplit {int button_number = 0; double run_seconds = 0.0;};
std::vector<ButtonSplit> g_button_splits;
struct PersonalBest {
    double total_seconds = 1.0;
    std::array<double, kRefunctButtonCount> button_seconds{};
    std::array<double, kRefunctButtonCount> best_segment_seconds{};
    PersonalBest() {button_seconds.fill(-1.0);best_segment_seconds.fill(-1.0);}
};
PersonalBest g_pb;
std::wstring g_pb_path=L"sample.ini",g_settings_path=L"setting.ini",g_active_profile=L"Any%",g_storage_dir=L"RefunctUtility";
bool g_engine_timer_valid=true,g_game_data_valid=true,g_button_state_initialized=true,g_run_started_at_zero=true;
bool g_timer_complete=false,g_practice_active=false,g_preloaded_restart_active=false,g_progress_restart_active=false;
std::atomic_bool g_menu_open{false};
std::atomic_bool g_rebinding{false}, g_name_editing{false}, g_coordinate_editing{false};
inline bool g_mock_menu_search_active = false;
inline bool IsMenuSearchTypingKey(int key) {
    return g_menu_open.load() && g_mock_menu_search_active && key >= 'A' && key <= 'Z';
}
bool IsPBGhostFastestSectionsEnabled();
double g_last_run_start=100.0,g_elapsed_seconds=0;
int g_last_button_count=0;
MyCharacter* g_player=nullptr;
using DrawRectFn=void(*)(void*,LinearColor,float,float,float,float);
DrawRectFn g_draw_rect=nullptr;
using ProcessEventFn=void(*)(void*,void*,void*);
ProcessEventFn g_process_event=nullptr;
inline std::uint64_t g_mock_readability_checks = 0;
inline bool IsReadable(const void* pointer,size_t){++g_mock_readability_checks;return pointer!=nullptr;}
inline void Log(const wchar_t*){}
// Production ghost options now use the async settings writer. The mock has no disk I/O.
inline bool QueueSettingsValue(const wchar_t*,const wchar_t*,const wchar_t*){return true;}
inline bool IsRegularFile(const std::wstring&){return false;}
inline bool EnsureDirectory(const std::wstring&){return true;}
inline bool EnsureGhostSectionDirectory(const std::wstring&){return true;}
inline bool CustomMapActive(){return false;}
inline bool CustomMapApplying(){return false;}
// Minimal practice/progression mocks for native button replay tests.
struct LevelState {int level=0, buttons=0, resets=3;};
inline LevelState g_mock_level_state{};
inline const LevelState* PracticeLevelState() {return &g_mock_level_state;}
inline bool g_restart_requested = false, g_full_level_reload_requested = false;
inline bool g_preloaded_restart_requested = false, g_practice_restart_requested = false;
inline bool g_practice_button_actors_ready = true;
inline std::array<void*, kRefunctButtonCount> g_practice_button_actors{};
inline void* g_add_based_character = reinterpret_cast<void*>(1);
inline void* g_remove_based_character = reinterpret_cast<void*>(1);
inline bool g_recording_tainted = false, g_run_pb_eligible = true, g_run_finish_processed = false;
enum class CheatAction { Buttons };
inline unsigned g_mock_cheat_marks = 0, g_mock_native_button_calls = 0;
inline bool g_mock_native_button_auto_confirm = true;
inline void MarkCheatAction(CheatAction) {++g_mock_cheat_marks;}
inline bool DiscoverPracticeButtons() {return g_practice_button_actors_ready;}
inline std::array<Vec3, kRefunctButtonCount> g_mock_button_centers{};
inline std::array<Vec3, kRefunctButtonCount> g_mock_live_button_centers{};
inline bool g_mock_override_live_centers = false;
inline std::array<bool, kRefunctButtonCount> g_mock_button_bounds_ready{};
inline int g_mock_last_physical_button = -1;
inline unsigned g_mock_button_bounds_calls = 0;
inline unsigned g_mock_button_spawn_calls = 0;
inline Vec3 PracticeSpawnForButton(int button) {
    ++g_mock_button_spawn_calls;
    return g_mock_button_centers.at(static_cast<std::size_t>(button - 1));
}
inline bool ResolveButtonBoundsCenter(int button, Vec3& out) {
    ++g_mock_button_bounds_calls;
    if (button < 1 || button > kRefunctButtonCount ||
        !g_mock_button_bounds_ready[button - 1]) return false;
    out = g_mock_button_centers[button - 1];
    return true;
}
inline bool ResolveReplayButtonCenter(int button, Vec3& out) {
    if (!ResolveButtonBoundsCenter(button, out)) return false;
    if (g_mock_override_live_centers) out = g_mock_live_button_centers[button - 1];
    return true;
}
inline bool ActivatePracticeButton(int zero_based, MyCharacter*) {
    ++g_mock_native_button_calls;
    const auto section = SectionContainingButton(g_mock_level_state.buttons + 1);
    if (!g_practice_button_actors_ready || !section.section ||
        zero_based < section.previous_end_button || zero_based >= section.end_button) return false;
    g_mock_last_physical_button = zero_based;
    if (g_mock_native_button_auto_confirm) ++g_mock_level_state.buttons;
    return true;
}
inline void Notify(const std::wstring&) {}
// Record permissions in the game are owned by records.cpp. The pure ghost
// test harness supplies toggles so tests exercise both clean/assisted paths.
inline bool g_save_pb_ghosts = true, g_save_fastest_ghosts = true;
inline bool g_mock_pb_record_allowed = true, g_mock_segment_record_allowed = true;
inline bool RecordsAllowedForPB(){return g_mock_pb_record_allowed;}
inline bool RecordsAllowedForSegments(){return g_mock_segment_record_allowed;}
// Synthetic reflected properties, opt-in for the deferred ghost mobility test.
inline int g_mock_mobility_offset = -1;
inline std::wstring UnrealName(FNameView name) {
    if (name.index == 501) return L"Mobility";
    if (name.index == 502) return L"ByteProperty";
    if (name.index == 503) return L"FloatProperty";
    return L"";
}
inline int FindClassPropertyOffset(void*, const wchar_t* name) {
    return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;
}
inline void* FindReflectedFunction(void*,const wchar_t*){return nullptr;}
}
