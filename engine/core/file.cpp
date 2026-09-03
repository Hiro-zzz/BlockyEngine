#include "engine/core/file.hpp"

#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <cstdio>
#endif

namespace blocky {
namespace {

#ifdef _WIN32

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), n);
    return w;
}

std::string lastErrorMessage() {
    DWORD code = GetLastError();
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, sizeof(buffer) - 1, nullptr);
    std::string msg(buffer);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return msg.empty() ? ("error " + std::to_string(code)) : msg;
}

#endif // _WIN32

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool isSeparator(char c) { return c == '/' || c == '\\'; }

} // namespace

bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out, std::string* error) {
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(utf8Path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setError(error, "cannot open " + utf8Path + ": " + lastErrorMessage());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        setError(error, "cannot size " + utf8Path + ": " + lastErrorMessage());
        CloseHandle(h);
        return false;
    }
    out.resize(size_t(size.QuadPart));
    size_t offset = 0;
    while (offset < out.size()) {
        DWORD chunk = DWORD(std::min<size_t>(out.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(h, out.data() + offset, chunk, &read, nullptr) || read == 0) {
            setError(error, "read failed on " + utf8Path + ": " + lastErrorMessage());
            CloseHandle(h);
            return false;
        }
        offset += read;
    }
    CloseHandle(h);
    return true;
#else
    FILE* f = std::fopen(utf8Path.c_str(), "rb");
    if (!f) { setError(error, "cannot open " + utf8Path); return false; }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(size));
    bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) setError(error, "read failed on " + utf8Path);
    return ok;
#endif
}

bool writeFileBytes(const std::string& utf8Path, const uint8_t* bytes, size_t size,
                    std::string* error) {
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(utf8Path).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setError(error, "cannot create " + utf8Path + ": " + lastErrorMessage());
        return false;
    }
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = DWORD(std::min<size_t>(size - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, bytes + offset, chunk, &written, nullptr) || written == 0) {
            setError(error, "write failed on " + utf8Path + ": " + lastErrorMessage());
            CloseHandle(h);
            return false;
        }
        offset += written;
    }
    CloseHandle(h);
    return true;
#else
    FILE* f = std::fopen(utf8Path.c_str(), "wb");
    if (!f) { setError(error, "cannot create " + utf8Path); return false; }
    bool ok = size == 0 || std::fwrite(bytes, 1, size, f) == size;
    std::fclose(f);
    if (!ok) setError(error, "write failed on " + utf8Path);
    return ok;
#endif
}

bool fileExists(const std::string& utf8Path) {
#ifdef _WIN32
    return GetFileAttributesW(widen(utf8Path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st{};
    return ::stat(utf8Path.c_str(), &st) == 0;
#endif
}

bool removeFile(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    if (DeleteFileW(widen(utf8Path).c_str())) return true;
    const DWORD reason = GetLastError();
    return reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND;
}

bool isDirectory(const std::string& utf8Path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(widen(utf8Path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st{};
    return ::stat(utf8Path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool listDirectory(const std::string& utf8Path, std::vector<DirEntry>& out) {
    out.clear();
#ifdef _WIN32
    std::wstring pattern = widen(utf8Path) + L"\\*";

    WIN32_FIND_DATAW find{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return false;

    do {
        std::wstring wide(find.cFileName);
        if (wide == L"." || wide == L"..") continue;

        int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) continue;
        std::string name(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), name.data(), n, nullptr, nullptr);

        out.push_back({std::move(name), (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(handle, &find));

    FindClose(handle);
    return true;
#else
    (void)utf8Path;
    return false;
#endif
}

std::string parentPath(const std::string& utf8Path) {
    for (size_t i = utf8Path.size(); i-- > 0;) {
        if (isSeparator(utf8Path[i])) return utf8Path.substr(0, i);
    }
    return {};
}

bool createDirectories(const std::string& utf8Path) {
    if (utf8Path.empty()) return true;
    if (fileExists(utf8Path)) return true;

    std::string parent = parentPath(utf8Path);
    // Stop at a drive root such as "C:" so we do not recurse forever.
    if (!parent.empty() && parent != utf8Path && parent.back() != ':') {
        if (!createDirectories(parent)) return false;
    }

#ifdef _WIN32
    if (CreateDirectoryW(widen(utf8Path).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(utf8Path.c_str(), 0755) == 0 || fileExists(utf8Path);
#endif
}

bool createParentDirectories(const std::string& utf8FilePath) {
    std::string parent = parentPath(utf8FilePath);
    return parent.empty() ? true : createDirectories(parent);
}

} // namespace blocky
