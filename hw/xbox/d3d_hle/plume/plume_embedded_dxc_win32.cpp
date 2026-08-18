#include "plume_embedded_dxc.h"

#include "plume_embedded_dxc_manifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

namespace xgpu {
namespace plume {

namespace {

constexpr int kDxcExeResource = 201;
constexpr int kDxcompilerResource = 202;
constexpr int kDxilResource = 203;

struct EmbeddedFile {
    int resource;
    const wchar_t *name;
    uint64_t size;
    const char *sha256;
};

constexpr EmbeddedFile kFiles[] = {
    {kDxcExeResource, L"dxc.exe", XRECOMP_DXC_EXE_SIZE,
     XRECOMP_DXC_EXE_SHA256},
    {kDxcompilerResource, L"dxcompiler.dll", XRECOMP_DXC_COMPILER_DLL_SIZE,
     XRECOMP_DXC_COMPILER_DLL_SHA256},
    {kDxilResource, L"dxil.dll", XRECOMP_DXC_VALIDATOR_DLL_SIZE,
     XRECOMP_DXC_VALIDATOR_DLL_SHA256},
};

std::atomic<uint64_t> g_temporarySequence{0};

std::string win32Error(const char *operation, DWORD error)
{
    std::ostringstream message;
    message << operation << " failed with Win32 error " << error;
    return message.str();
}

bool sha256(const uint8_t *data, size_t size, std::string &hex,
            std::string &diagnostics)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD received = 0;
    std::vector<uint8_t> object;
    std::vector<uint8_t> digest;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        diagnostics = "BCryptOpenAlgorithmProvider(SHA256) failed";
        return false;
    }
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectSize),
                               sizeof(objectSize), &received, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                   reinterpret_cast<PUCHAR>(&hashSize),
                                   sizeof(hashSize), &received, 0);
    }
    if (status >= 0) {
        object.resize(objectSize);
        digest.resize(hashSize);
        status = BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                                  nullptr, 0, 0);
    }
    if (status >= 0 && size != 0) {
        status = BCryptHashData(hash, const_cast<PUCHAR>(data),
                                static_cast<ULONG>(size), 0);
    }
    if (status >= 0)
        status = BCryptFinishHash(hash, digest.data(), hashSize, 0);
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0 || digest.size() != 32) {
        diagnostics = "Windows SHA-256 operation failed";
        return false;
    }

    static constexpr char digits[] = "0123456789abcdef";
    hex.clear();
    hex.reserve(digest.size() * 2);
    for (const uint8_t byte : digest) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    return true;
}

bool loadResource(const EmbeddedFile &file, const uint8_t *&data,
                  size_t &size, std::string &diagnostics)
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(file.resource),
                                   MAKEINTRESOURCEW(10));
    if (!resource) {
        diagnostics = win32Error("FindResourceW(DXC)", GetLastError());
        return false;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) {
        diagnostics = win32Error("LoadResource(DXC)", GetLastError());
        return false;
    }
    const DWORD resourceSize = SizeofResource(module, resource);
    const void *resourceData = LockResource(loaded);
    if (!resourceData || resourceSize == 0) {
        diagnostics = "Embedded DXC resource is empty";
        return false;
    }
    data = static_cast<const uint8_t *>(resourceData);
    size = resourceSize;
    return true;
}

bool payloadMatches(const uint8_t *data, size_t size,
                    const EmbeddedFile &file, std::string &diagnostics)
{
    if (size != file.size) {
        std::ostringstream message;
        message << "Embedded " << std::filesystem::path(file.name).string()
                << " has size " << size << ", expected " << file.size;
        diagnostics = message.str();
        return false;
    }
    std::string actual;
    if (!sha256(data, size, actual, diagnostics))
        return false;
    if (actual != file.sha256) {
        diagnostics = "Embedded DXC payload failed SHA-256 verification";
        return false;
    }
    return true;
}

bool cachedFileMatches(const std::filesystem::path &path,
                       const EmbeddedFile &file)
{
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size != file.size)
        return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream || static_cast<size_t>(stream.gcount()) != bytes.size())
        return false;
    std::string actual;
    std::string ignored;
    return sha256(bytes.data(), bytes.size(), actual, ignored)
        && actual == file.sha256;
}

bool writeAtomically(const std::filesystem::path &destination,
                     const uint8_t *data, size_t size,
                     const EmbeddedFile &file, std::string &diagnostics)
{
    std::filesystem::path temporary = destination;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(g_temporarySequence.fetch_add(1)) + L".tmp";
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        diagnostics = win32Error("CreateFileW(DXC temporary file)",
                                 GetLastError());
        return false;
    }

    size_t writtenTotal = 0;
    bool ok = true;
    while (writtenTotal < size) {
        const size_t remaining = size - writtenTotal;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 1024u * 1024u ? 1024u * 1024u : remaining);
        DWORD written = 0;
        if (!WriteFile(handle, data + writtenTotal, chunk, &written, nullptr)
            || written != chunk) {
            diagnostics = win32Error("WriteFile(DXC)", GetLastError());
            ok = false;
            break;
        }
        writtenTotal += written;
    }
    if (ok && !FlushFileBuffers(handle)) {
        diagnostics = win32Error("FlushFileBuffers(DXC)", GetLastError());
        ok = false;
    }
    CloseHandle(handle);
    if (ok && !MoveFileExW(temporary.c_str(), destination.c_str(),
                           MOVEFILE_REPLACE_EXISTING |
                           MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        if (!cachedFileMatches(destination, file)) {
            diagnostics = win32Error("MoveFileExW(DXC)", error);
            ok = false;
        }
    }
    DeleteFileW(temporary.c_str());
    return ok;
}

std::filesystem::path localRuntimeDirectory(std::string &diagnostics)
{
    std::wstring localAppData(32768, L'\0');
    DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(),
        static_cast<DWORD>(localAppData.size()));
    if (length == 0 || length >= localAppData.size()) {
        length = GetTempPathW(static_cast<DWORD>(localAppData.size()),
                              localAppData.data());
        if (length == 0 || length >= localAppData.size()) {
            diagnostics = win32Error("GetTempPathW", GetLastError());
            return {};
        }
    }
    localAppData.resize(length);
    return std::filesystem::path(localAppData) / L"XboxRecompGame" /
        L"Runtime" / L"DXC" / XRECOMP_DXC_BUNDLE_ID;
}

} /* namespace */

bool ensureEmbeddedDxc(std::filesystem::path &executable,
                       std::string &diagnostics)
{
    static std::mutex mutex;
    static bool attempted = false;
    static bool succeeded = false;
    static std::filesystem::path cachedExecutable;
    static std::string cachedDiagnostics;
    std::lock_guard<std::mutex> lock(mutex);

    if (attempted) {
        executable = cachedExecutable;
        diagnostics = cachedDiagnostics;
        return succeeded;
    }
    attempted = true;

    const std::filesystem::path directory =
        localRuntimeDirectory(cachedDiagnostics);
    if (directory.empty()) {
        diagnostics = cachedDiagnostics;
        return false;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        cachedDiagnostics = "Unable to create the per-user DXC cache: " +
            directoryError.message();
        diagnostics = cachedDiagnostics;
        return false;
    }

    for (const EmbeddedFile &file : kFiles) {
        const uint8_t *data = nullptr;
        size_t size = 0;
        if (!loadResource(file, data, size, cachedDiagnostics) ||
            !payloadMatches(data, size, file, cachedDiagnostics)) {
            diagnostics = cachedDiagnostics;
            return false;
        }
        const std::filesystem::path destination = directory / file.name;
        if (!cachedFileMatches(destination, file) &&
            !writeAtomically(destination, data, size, file,
                             cachedDiagnostics)) {
            diagnostics = cachedDiagnostics;
            return false;
        }
    }

    cachedExecutable = directory / L"dxc.exe";
    cachedDiagnostics = "embedded DXC " XRECOMP_DXC_RELEASE_TAG;
    succeeded = true;
    executable = cachedExecutable;
    diagnostics = cachedDiagnostics;
    return true;
}

} /* namespace plume */
} /* namespace xgpu */
