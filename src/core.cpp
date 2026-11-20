#include "refunct/internal_api.hpp"
#include <deque>
#include <new>
#include "refunct/runtime_logs.hpp"

namespace refunct {
namespace {
// The bootstrap thread queues early diagnostics in memory. Logging is only
// written once LoadSettings has found the game-local RefunctUtility directory
// and loaded the user's preference.
constexpr size_t kLogQueueLimit = 2048;
constexpr size_t kLogBatchLimit = 128;
struct LogEntry {
    std::wstring message;
    std::uint32_t count = 1;
};
enum class ClearStatus { Ready, Pending, Cleared, Failed };
std::atomic<ClearStatus> s_clear_status{ClearStatus::Ready};
struct AsyncLogger {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr;
    HANDLE idle = nullptr;
    std::deque<LogEntry> pending;
    std::uint64_t dropped = 0;
    std::wstring path; // Published only once, under lock, before writer uses it.
    bool configured = false;
    bool clear_requested = false;
    std::atomic_uint64_t generation{0}; // Invalidates in-flight batches on disable or clear.
};
INIT_ONCE s_log_once = INIT_ONCE_STATIC_INIT;
AsyncLogger* s_log_worker = nullptr;

DWORD WINAPI LogWriterMain(LPVOID context) {
    auto* worker = static_cast<AsyncLogger*>(context);
    // Only this background thread accesses the file handle.
    HANDLE file = INVALID_HANDLE_VALUE;
    for (;;) {
        if (WaitForSingleObject(worker->wake, INFINITE) != WAIT_OBJECT_0) return 1;
        for (;;) {
            std::vector<LogEntry> batch;
            batch.reserve(kLogBatchLimit);
            std::uint64_t dropped = 0;
            std::uint64_t generation = 0;
            AcquireSRWLockExclusive(&worker->lock);
            const bool configured = worker->configured;
            generation = worker->generation.load(std::memory_order_acquire);
            const bool enabled = g_util_log_enabled.load(std::memory_order_acquire);
            const bool clear = worker->clear_requested;
            worker->clear_requested = false;
            if (clear) {
                // Preserve messages submitted after the request for the fresh log.
            } else if (!enabled) {
                worker->pending.clear();
                worker->dropped = 0;
            } else if (configured) {
                while (!worker->pending.empty() && batch.size() < kLogBatchLimit) {
                    batch.push_back(std::move(worker->pending.front()));
                    worker->pending.pop_front();
                }
                dropped = worker->dropped;
                worker->dropped = 0;
            }
            if (configured && !clear && batch.empty() && !dropped) SetEvent(worker->idle);
            ReleaseSRWLockExclusive(&worker->lock);

            if (clear) {
                if (file != INVALID_HANDLE_VALUE) {
                    CloseHandle(file);
                    file = INVALID_HANDLE_VALUE;
                }
                bool success = false;
                try {
                    success = runtime_logs::ClearDirectory(runtime_logs::ParentDirectory(worker->path));
                } catch (...) {} // The worker must survive filesystem failures.
                s_clear_status.store(success ? ClearStatus::Cleared : ClearStatus::Failed,
                                     std::memory_order_release);
                continue;
            }
            if (!enabled) {
                // Closing is also done off the game thread. A previously
                // in-flight write may finish, but no new batches are written.
                if (file != INVALID_HANDLE_VALUE) {
                    CloseHandle(file);
                    file = INVALID_HANDLE_VALUE;
                }
                break;
            }
            if (!configured || (batch.empty() && !dropped)) break;

            std::wstring payload;
            payload.reserve(batch.size() * 100);
            for (const auto& entry : batch) {
                payload.append(entry.message).append(L"\r\n");
                if (entry.count > 1) {
                    payload.append(L"(previous message repeated ")
                           .append(std::to_wstring(entry.count - 1))
                           .append(L" additional times)\r\n");
                }
            }
            if (dropped) {
                payload.append(L"WARNING: dropped ")
                       .append(std::to_wstring(dropped))
                       .append(L" log messages due to full asynchronous queue\r\n");
            }
            // Disabling logging stops both disk and debugger diagnostics.
            if (!g_util_log_enabled.load(std::memory_order_acquire) ||
                worker->generation.load(std::memory_order_acquire) != generation) continue;
            OutputDebugStringW(payload.c_str());
            if (file == INVALID_HANDLE_VALUE &&
                g_util_log_enabled.load(std::memory_order_acquire) &&
                worker->generation.load(std::memory_order_acquire) == generation) {
                file = CreateFileW(worker->path.c_str(), FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (file != INVALID_HANDLE_VALUE &&
                g_util_log_enabled.load(std::memory_order_acquire) &&
                worker->generation.load(std::memory_order_acquire) == generation) {
                const auto bytes = payload.size() * sizeof(wchar_t);
                if (bytes <= MAXDWORD) {
                    DWORD written = 0;
                    if (!WriteFile(file, payload.data(), static_cast<DWORD>(bytes),
                                   &written, nullptr) || written != bytes) {
                        CloseHandle(file);
                        file = INVALID_HANDLE_VALUE;
                    }
                }
            }
        }
    }
}

BOOL CALLBACK InitializeLogWriter(PINIT_ONCE, PVOID, PVOID*) {
    // Create on the bootstrap thread, not DllMain or a render callback.
    auto* worker = new (std::nothrow) AsyncLogger;
    if (!worker) return TRUE;
    worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker->idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (worker->wake && worker->idle) {
        HANDLE thread = CreateThread(nullptr, 0, LogWriterMain, worker, 0, nullptr);
        if (thread) {
            CloseHandle(thread); // Worker stays alive until process termination.
            s_log_worker = worker;
            return TRUE;
        }
    }
    if (worker->wake) CloseHandle(worker->wake);
    if (worker->idle) CloseHandle(worker->idle);
    delete worker;
    return TRUE;
}
} // namespace

void ConfigureUtilLogDirectory(const std::wstring& storage_directory) {
    if (storage_directory.empty()) return;
    InitOnceExecuteOnce(&s_log_once, InitializeLogWriter, nullptr, nullptr);
    AsyncLogger* worker = s_log_worker;
    if (!worker) return;
    AcquireSRWLockExclusive(&worker->lock);
    if (!worker->configured) {
        worker->path = storage_directory + L"\\Logs\\RefunctUtility.log";
        worker->configured = true;
    }
    if (!g_util_log_enabled.load(std::memory_order_acquire)) {
        worker->pending.clear();
        worker->dropped = 0;
    }
    ReleaseSRWLockExclusive(&worker->lock);
    SetEvent(worker->wake);
}

void SetUtilLogEnabled(bool enabled) {
    AsyncLogger* worker = s_log_worker;
    if (!enabled && worker)
        worker->generation.fetch_add(1, std::memory_order_acq_rel);
    g_util_log_enabled.store(enabled, std::memory_order_release);
    if (!worker) return;
    AcquireSRWLockExclusive(&worker->lock);
    if (!enabled) {
        worker->pending.clear();
        worker->dropped = 0;
    }
    ReleaseSRWLockExclusive(&worker->lock);
    SetEvent(worker->wake);
}

bool RequestClearRuntimeLogs() {
    auto* worker = s_log_worker;
    if (!worker) return false;
    AcquireSRWLockExclusive(&worker->lock);
    if (!worker->configured || s_clear_status.load(std::memory_order_acquire) == ClearStatus::Pending) {
        ReleaseSRWLockExclusive(&worker->lock);
        return false;
    }
    worker->generation.fetch_add(1, std::memory_order_acq_rel);
    worker->pending.clear();
    worker->dropped = 0;
    worker->clear_requested = true;
    s_clear_status.store(ClearStatus::Pending, std::memory_order_release);
    ResetEvent(worker->idle);
    ReleaseSRWLockExclusive(&worker->lock);
    SetEvent(worker->wake);
    return true;
}

const wchar_t* ClearLogsMenuLabel() {
    switch (s_clear_status.load(std::memory_order_acquire)) {
        case ClearStatus::Pending: return L"Clear Logs (clearing...)";
        case ClearStatus::Cleared: return L"Clear Logs (cleared)";
        case ClearStatus::Failed: return L"Clear Logs (some logs could not be cleared)";
        default: return L"Clear Logs";
    }
}

void Log(const wchar_t* message) {
    if (!message || !g_util_log_enabled.load(std::memory_order_acquire)) return;
    InitOnceExecuteOnce(&s_log_once, InitializeLogWriter, nullptr, nullptr);
    AsyncLogger* writer = s_log_worker;
    // Do not write on the game thread if the asynchronous worker is unavailable.
    if (!writer) return;
    AcquireSRWLockExclusive(&writer->lock);
    // Recheck while holding the lock: a toggle to OFF must discard queued rows.
    if (!g_util_log_enabled.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&writer->lock);
        return;
    }
    if (!writer->pending.empty() && writer->pending.back().message == message &&
        writer->pending.back().count != UINT32_MAX) {
        ++writer->pending.back().count;
    } else if (writer->pending.size() < kLogQueueLimit) {
        writer->pending.push_back(LogEntry{message, 1});
    } else {
        ++writer->dropped;
    }
    ResetEvent(writer->idle);
    const bool configured = writer->configured;
    ReleaseSRWLockExclusive(&writer->lock);
    if (configured) SetEvent(writer->wake);
}

// For explicit non-game-tick operations only; never called from DllMain.
bool DrainLogWriter(DWORD timeout_ms) {
    if (!s_log_worker) return true;
    AcquireSRWLockExclusive(&s_log_worker->lock);
    const bool configured = s_log_worker->configured;
    ReleaseSRWLockExclusive(&s_log_worker->lock);
    if (!configured) return false;
    return WaitForSingleObject(s_log_worker->idle, timeout_ms) == WAIT_OBJECT_0;
}

bool IsReadable(const void* ptr, size_t size) {
    if (!ptr || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start <= end && size <= end - start;
}

// Guard native field writes separately from reads. UE4 objects normally live
// on READWRITE pages, but a readable address need not be writable.
bool IsWritable(const void* ptr, size_t size) {
    if (!ptr || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD access = mbi.Protect & 0xFFu;
    if (access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
        access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (start < base) return false;
    const size_t offset = static_cast<size_t>(start - base);
    return offset <= mbi.RegionSize && size <= mbi.RegionSize - offset;
}

bool IsRvaInSection(HMODULE module, uintptr_t rva, bool executable) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!IsReadable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || dos->e_lfanew > 0x1000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (!IsReadable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        rva >= nt->OptionalHeader.SizeOfImage) return false;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uintptr_t first = sections[i].VirtualAddress;
        const uintptr_t size = std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
        if (rva >= first && rva - first < size &&
            (!executable || (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))) return true;
    }
    return false;
}

void Notify(const std::wstring& text) {
    g_notice = text;
    g_notice_expires = GetTickCount64() + 2700;
}

int Hotkey(Action action) {
    return g_keys[static_cast<size_t>(action)].load(std::memory_order_relaxed);
}

std::wstring KeyName(int vk) {
    if (vk == 0) return L"None";
    switch (vk) {
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Escape";
        case VK_SPACE: return L"Space";
        case VK_SHIFT: return L"Shift";
        default: break;
    }
    if (vk >= '0' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F12) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    wchar_t result[48]{};
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scan && GetKeyNameTextW(static_cast<LONG>(scan) << 16, result, 48)) return result;
    return L"VK " + std::to_wstring(vk);
}

// Arrows, Enter, Escape and reserved system modifiers cannot be rebound.
// Avoid taking Windows shortcuts away from the OS or Refunct's movement keys.
bool IsBindable(int key) {
    if (key < 8 || key >= 255) return false;
    switch (key) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_RETURN: case VK_ESCAPE: case VK_TAB: case VK_BACK:
        case VK_LWIN: case VK_RWIN: case VK_MENU:
        case VK_CONTROL: case VK_SHIFT: case VK_SPACE:
        case VK_LBUTTON: case VK_RBUTTON: case VK_F11:
        case 'W': case 'A': case 'S': case 'D':
            return false;
        default: return true;
    }
}


} // namespace refunct
