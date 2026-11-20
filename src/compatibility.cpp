#include "refunct/internal_api.hpp"
#include "refunct/features/compatibility.hpp"
#include <bcrypt.h>

namespace refunct {
namespace {
constexpr wchar_t kPinFile[] = L"RefunctUtility.game.sha256";
constexpr size_t kDigestBytes = 32;
constexpr size_t kHexLength = kDigestBytes * 2;

struct HandleCloser {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~HandleCloser() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HandleCloser() = default;
    explicit HandleCloser(HANDLE value) : handle(value) {}
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
};

struct CryptoHandles {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~CryptoHandles() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

bool MakePinPath(std::wstring& out) {
    // The pin must live with the installed ASI, not with user-specific
    // settings or PB profiles, so moving the DLL is an explicit new install.
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    constexpr size_t name_length = std::size(kPinFile);
    if (name_length > MAX_PATH - static_cast<size_t>(slash + 1 - path)) return false;
    out.assign(path, static_cast<size_t>(slash + 1 - path));
    out += kPinFile;
    return true;
}

bool HashExecutable(const wchar_t* path, std::array<uint8_t, kDigestBytes>& digest) {
    HandleCloser file(CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) {
        Log(L"COMPATIBILITY ERROR: cannot open Refunct executable for SHA-256 check");
        return false;
    }

    CryptoHandles crypto;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &crypto.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        Log(L"COMPATIBILITY ERROR: BCrypt SHA-256 provider unavailable");
        return false;
    }
    DWORD object_size = 0;
    DWORD returned = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(crypto.algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0)) ||
        returned != sizeof(object_size) || object_size == 0 || object_size > 65536) {
        Log(L"COMPATIBILITY ERROR: invalid SHA-256 object length");
        return false;
    }
    std::vector<UCHAR> object(object_size);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(crypto.algorithm, &crypto.hash,
            object.data(), object_size, nullptr, 0, 0))) {
        Log(L"COMPATIBILITY ERROR: BCryptCreateHash failed");
        return false;
    }

    std::array<UCHAR, 65536> buffer{};
    DWORD bytes_read = 0;
    for (;;) {
        if (!ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                &bytes_read, nullptr)) {
            Log(L"COMPATIBILITY ERROR: unable to read the entire executable");
            return false;
        }
        if (bytes_read == 0) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(crypto.hash, buffer.data(), bytes_read, 0))) {
            Log(L"COMPATIBILITY ERROR: BCryptHashData failed");
            return false;
        }
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(crypto.hash, digest.data(),
            static_cast<ULONG>(digest.size()), 0))) {
        Log(L"COMPATIBILITY ERROR: BCryptFinishHash failed");
        return false;
    }
    return true;
}

std::string HexDigest(const std::array<uint8_t, kDigestBytes>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(kHexLength, '0');
    for (size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return result;
}

bool IsValidDigest(const std::string& text) {
    if (text.size() != kHexLength) return false;
    for (char ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

// Read a pin that may have been written manually or by our own first run.
// A corrupt/partial pin fails closed rather than being replaced automatically.
bool ReadPin(const std::wstring& path, std::string& stored) {
    HandleCloser file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 64 || size.QuadPart > 128) return false;
    std::array<char, 129> bytes{};
    DWORD read = 0;
    if (!ReadFile(file.handle, bytes.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) ||
        read != static_cast<DWORD>(size.QuadPart)) return false;
    std::string input(bytes.data(), read);
    while (!input.empty() && (input.back() == '\n' || input.back() == '\r')) input.pop_back();
    if (!IsValidDigest(input)) return false;
    stored = std::move(input);
    return true;
}

enum class PinCreateStatus { Created, AlreadyExists, Failed };

PinCreateStatus WriteFirstPin(const std::wstring& path, const std::string& digest) {
    // CREATE_NEW ensures no concurrent Refunct process can overwrite a pin.
    HandleCloser file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            ? PinCreateStatus::AlreadyExists : PinCreateStatus::Failed;
    }
    const std::string content = digest + "\r\n";
    DWORD written = 0;
    const bool complete = WriteFile(file.handle, content.data(),
        static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size() && FlushFileBuffers(file.handle);
    if (!complete) {
        // Leave the malformed file in place: never silently re-trust a changed
        // executable merely because pin creation failed partway through.
        Log(L"COMPATIBILITY ERROR: SHA-256 pin could not be written completely");
    }
    return complete ? PinCreateStatus::Created : PinCreateStatus::Failed;
}

void LogHash(const wchar_t* prefix, const std::string& hash) {
    std::wstring message(prefix);
    message.append(hash.begin(), hash.end());
    Log(message.c_str());
}
} // namespace

bool ValidateGameExecutable(const wchar_t* executable_path) {
    if (!g_module || !executable_path || !*executable_path) return false;
    std::wstring pin_path;
    if (!MakePinPath(pin_path)) {
        Log(L"COMPATIBILITY ERROR: cannot resolve ASI-local fingerprint path");
        return false;
    }

    std::array<uint8_t, kDigestBytes> digest{};
    if (!HashExecutable(executable_path, digest)) return false;
    const std::string current = HexDigest(digest);
    LogHash(L"COMPATIBILITY: current Refunct SHA-256 = ", current);

    // Once we have a verified reference EXE, a compiled-in allowlist enables
    // genuinely safe first-run detection, including after a game update.
    const std::string known(kKnownCompatibleRefunctExeSha256);
    if (!known.empty()) {
        if (!IsValidDigest(known)) {
            Log(L"COMPATIBILITY ERROR: invalid hard-coded release fingerprint");
            return false;
        }
        if (known != current) {
            LogHash(L"COMPATIBILITY: compiled supported SHA-256 = ", known);
            Log(L"COMPATIBILITY ERROR: unsupported Refunct game executable; hooks disabled");
            return false;
        }
        Log(L"COMPATIBILITY: exact release-approved Refunct executable confirmed");
        return true;
    }

    DWORD file_attributes = GetFileAttributesW(pin_path.c_str());
    if (file_attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        const PinCreateStatus status = WriteFirstPin(pin_path, current);
        if (status == PinCreateStatus::Created) {
            Log(L"COMPATIBILITY: first-run game EXE pinned; future changes will disable hooks");
            Log(L"COMPATIBILITY NOTE: first-run pinning does NOT prove this build is supported");
            return true;
        }
        if (status == PinCreateStatus::Failed) {
            Log(L"COMPATIBILITY ERROR: unable to create fingerprint; refusing hooks");
            return false;
        }
        // If another process created the file in the meantime, validate it.
        file_attributes = GetFileAttributesW(pin_path.c_str());
    }
    if (file_attributes == INVALID_FILE_ATTRIBUTES || (file_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        Log(L"COMPATIBILITY ERROR: missing/inaccessible game fingerprint; refusing hooks");
        return false;
    }

    std::string stored;
    if (!ReadPin(pin_path, stored)) {
        Log(L"COMPATIBILITY ERROR: malformed/unreadable game fingerprint; refusing hooks");
        return false;
    }
    if (stored != current) {
        LogHash(L"COMPATIBILITY: pinned Refunct SHA-256 = ", stored);
        Log(L"COMPATIBILITY ERROR: game executable changed; ALL hooks disabled");
        Log(L"COMPATIBILITY: verify the new game build before deliberately updating the pin");
        return false;
    }
    Log(L"COMPATIBILITY: exact pinned executable confirmed");
    return true;
}
} // namespace refunct
