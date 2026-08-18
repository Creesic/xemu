#include "plume_shader_compiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#if !defined(XEMU_D3D_HLE_NO_DXC_LIBRARY)
#include <unknwn.h>
#include <objidl.h>
#if defined(__MINGW32__)
// dxcapi.h spells its UUID metadata as __declspec(uuid), which the GNU Windows
// ABI cannot consume. The call sites below use explicit interface IIDs.
#define CROSS_PLATFORM_UUIDOF(interface, spec) struct interface;
#endif
#include <dxcapi.h>
#endif
#if defined(XRECOMP_DXC_EMBEDDED)
#include "plume_embedded_dxc.h"
#endif
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#ifndef XRECOMP_DXC_DEFAULT_PATH
#define XRECOMP_DXC_DEFAULT_PATH ""
#endif

#ifndef XRECOMP_SPIRV_CROSS_MSL_DEFAULT_PATH
#define XRECOMP_SPIRV_CROSS_MSL_DEFAULT_PATH ""
#endif

namespace xgpu {
namespace plume {

namespace {

std::atomic<uint64_t> g_shaderCompileCounter{0};

class TemporaryShaderFiles {
public:
    TemporaryShaderFiles() = default;
    TemporaryShaderFiles(const TemporaryShaderFiles &) = delete;
    TemporaryShaderFiles &operator=(const TemporaryShaderFiles &) = delete;
    TemporaryShaderFiles(TemporaryShaderFiles &&) = default;
    TemporaryShaderFiles &operator=(TemporaryShaderFiles &&) = default;

    std::filesystem::path source;
    std::filesystem::path object;
    std::filesystem::path metal;
    std::filesystem::path metalEntryPoint;
    std::filesystem::path diagnostics;
    std::filesystem::path metalDiagnostics;

    ~TemporaryShaderFiles()
    {
        std::error_code error;
        std::filesystem::remove(source, error);
        std::filesystem::remove(object, error);
        std::filesystem::remove(metal, error);
        std::filesystem::remove(metalEntryPoint, error);
        std::filesystem::remove(diagnostics, error);
        std::filesystem::remove(metalDiagnostics, error);
    }
};

uint64_t currentProcessId()
{
#if defined(_WIN32)
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::filesystem::path currentExecutableDirectory()
{
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    return std::filesystem::weakly_canonical(buffer).parent_path();
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(),
                                    buffer.size() - 1);
    if (length <= 0)
        return {};
    buffer.resize(static_cast<size_t>(length));
    return std::filesystem::path(buffer).parent_path();
#else
    return {};
#endif
}

std::string resolveTool(const char *environmentName,
                        const char *adjacentName,
                        const char *configuredPath,
                        const char *pathName)
{
    const char *environmentPath = std::getenv(environmentName);
    if (environmentPath != nullptr && environmentPath[0] != '\0')
        return environmentPath;

    const std::filesystem::path executableDirectory =
        currentExecutableDirectory();
    if (!executableDirectory.empty()) {
        const std::filesystem::path adjacent =
            executableDirectory / adjacentName;
        if (std::filesystem::exists(adjacent))
            return adjacent.string();
    }

    if (configuredPath != nullptr && configuredPath[0] != '\0' &&
        std::filesystem::exists(configuredPath)) {
        return configuredPath;
    }

    return pathName;
}

std::string resolveDxcTool(const char *adjacentName,
                           const char *configuredPath,
                           const char *pathName,
                           std::string &embeddedDiagnostics)
{
    const char *environmentPath = std::getenv("XEMU_DXC");
    if (environmentPath == nullptr || environmentPath[0] == '\0')
        environmentPath = std::getenv("XRECOMP_DXC");
    if (environmentPath != nullptr && environmentPath[0] != '\0')
        return environmentPath;

#if defined(_WIN32) && defined(XRECOMP_DXC_EMBEDDED)
    std::filesystem::path embeddedExecutable;
    if (ensureEmbeddedDxc(embeddedExecutable, embeddedDiagnostics))
        return embeddedExecutable.string();
#endif

    const std::filesystem::path executableDirectory =
        currentExecutableDirectory();
    if (!executableDirectory.empty()) {
        const std::filesystem::path adjacent =
            executableDirectory / adjacentName;
        if (std::filesystem::exists(adjacent))
            return adjacent.string();
    }

    if (configuredPath != nullptr && configuredPath[0] != '\0' &&
        std::filesystem::exists(configuredPath)) {
        return configuredPath;
    }
    return pathName;
}

std::string quoteShellArgument(const std::string &argument)
{
#if defined(_WIN32)
    if (argument.find_first_of("\"\r\n") != std::string::npos)
        return {};
    return "\"" + argument + "\"";
#else
    std::string quoted = "'";
    for (const char ch : argument) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted += ch;
    }
    quoted += "'";
    return quoted;
#endif
}

int runTool(const std::string &executable,
            const std::vector<std::string> &arguments,
            const std::filesystem::path &diagnosticsPath)
{
    std::string command = quoteShellArgument(executable);
    if (command.empty())
        return -1;

    for (const std::string &argument : arguments) {
        const std::string quoted = quoteShellArgument(argument);
        if (quoted.empty())
            return -1;
        command += " " + quoted;
    }

    const std::string quotedDiagnostics =
        quoteShellArgument(diagnosticsPath.string());
    if (quotedDiagnostics.empty())
        return -1;
    command += " > " + quotedDiagnostics + " 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    return std::system(command.c_str());
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

std::string readTextFile(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

bool writeSourceFile(const std::filesystem::path &path, const char *source)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    stream.write(source, static_cast<std::streamsize>(std::char_traits<char>::length(source)));
    return stream.good();
}

TemporaryShaderFiles makeTemporaryFiles()
{
    const uint64_t sequence = g_shaderCompileCounter.fetch_add(1);
    const std::string stem = "xrecomp_shader_" +
        std::to_string(currentProcessId()) + "_" + std::to_string(sequence);
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path();

    TemporaryShaderFiles files;
    files.source = directory / (stem + ".hlsl");
    files.object = directory / (stem + ".bin");
    files.metal = directory / (stem + ".metal");
    files.metalEntryPoint = directory / (stem + ".metal.entry");
    files.diagnostics = directory / (stem + ".dxc.log");
    files.metalDiagnostics = directory / (stem + ".msl.log");
    return files;
}

uint64_t fnv1a64(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void hashLengthDelimited(uint64_t &hash, const void *data, size_t size)
{
    const uint64_t encodedSize = static_cast<uint64_t>(size);
    hash = fnv1a64(hash, &encodedSize, sizeof(encodedSize));
    hash = fnv1a64(hash, data, size);
}

void hashString(uint64_t &hash, const std::string &value)
{
    hashLengthDelimited(hash, value.data(), value.size());
}

std::string toolIdentity(const std::filesystem::path &path)
{
    if (path.empty())
        return "none";

    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    if (error)
        normalized = path;

    std::ostringstream identity;
    identity << normalized.generic_string();
    error.clear();
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (!error)
        identity << "|size=" << size;
    error.clear();
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error)
        identity << "|mtime="
                 << std::chrono::duration_cast<std::chrono::nanoseconds>(
                        modified.time_since_epoch()).count();
    return identity.str();
}

std::filesystem::path dxcLibraryForExecutable(const std::string &executable)
{
#if defined(_WIN32)
    std::filesystem::path path(executable);
    if (!path.has_parent_path())
        return {};
    std::error_code error;
    path = std::filesystem::absolute(path, error);
    if (error)
        path = std::filesystem::path(executable);
    return path.parent_path() / L"dxcompiler.dll";
#else
    (void)executable;
    return {};
#endif
}

std::vector<std::string> semanticDxcArguments(
    const ShaderCompileRequest &request)
{
    std::vector<std::string> arguments =
        buildDxcArguments(request, std::string(), std::string());
    const auto output = std::find(arguments.begin(), arguments.end(), "-Fo");
    arguments.erase(output, arguments.end());
    return arguments;
}

constexpr uint32_t kShaderCacheFormatVersion = 2;
constexpr uint64_t kMaxShaderCacheBytes = 64ull * 1024ull * 1024ull;
constexpr uint8_t kShaderCacheMagic[8] = {
    'X', 'G', 'P', 'U', 'S', 'H', 'D', 'R'
};

template <typename T>
void appendCacheValue(std::vector<uint8_t> &data, T value)
{
    const size_t offset = data.size();
    data.resize(offset + sizeof(value));
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

template <typename T>
bool readCacheValue(const std::vector<uint8_t> &data, size_t &offset, T &value)
{
    if (offset > data.size() || data.size() - offset < sizeof(value))
        return false;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    offset += sizeof(value);
    return true;
}

uint64_t shaderCachePayloadHash(const std::string &entryPoint,
                                const std::vector<uint8_t> &bytecode)
{
    uint64_t hash = 1469598103934665603ull;
    hashLengthDelimited(hash, entryPoint.data(), entryPoint.size());
    hashLengthDelimited(hash, bytecode.data(), bytecode.size());
    return hash;
}

bool shaderPayloadValid(ShaderTarget target, const std::string &entryPoint,
                        const std::vector<uint8_t> &bytecode)
{
    if (entryPoint.empty() || bytecode.empty())
        return false;
    if (target == ShaderTarget::DXIL)
        return bytecode.size() >= 4 &&
               std::memcmp(bytecode.data(), "DXBC", 4) == 0;
    if (target == ShaderTarget::SPIRV) {
        uint32_t magic = 0;
        if (bytecode.size() < sizeof(magic))
            return false;
        std::memcpy(&magic, bytecode.data(), sizeof(magic));
        return magic == 0x07230203u;
    }
    return true;
}

std::vector<uint8_t> encodeShaderCacheRecord(
    ShaderTarget target, const std::string &entryPoint,
    const std::vector<uint8_t> &bytecode)
{
    std::vector<uint8_t> record;
    record.insert(record.end(), std::begin(kShaderCacheMagic),
                  std::end(kShaderCacheMagic));
    appendCacheValue(record, kShaderCacheFormatVersion);
    appendCacheValue(record, static_cast<uint32_t>(target));
    appendCacheValue(record, static_cast<uint32_t>(entryPoint.size()));
    appendCacheValue(record, static_cast<uint64_t>(bytecode.size()));
    appendCacheValue(record, shaderCachePayloadHash(entryPoint, bytecode));
    record.insert(record.end(), entryPoint.begin(), entryPoint.end());
    record.insert(record.end(), bytecode.begin(), bytecode.end());
    return record;
}

bool decodeShaderCacheRecord(const std::filesystem::path &path,
                             ShaderTarget expectedTarget,
                             std::string &entryPoint,
                             std::vector<uint8_t> &bytecode)
{
    std::error_code error;
    const uintmax_t fileSize = std::filesystem::file_size(path, error);
    if (error || fileSize > kMaxShaderCacheBytes)
        return false;
    const std::vector<uint8_t> record = readBinaryFile(path);
    if (record.size() != fileSize ||
        record.size() < sizeof(kShaderCacheMagic) ||
        std::memcmp(record.data(), kShaderCacheMagic,
                    sizeof(kShaderCacheMagic)) != 0)
        return false;

    size_t offset = sizeof(kShaderCacheMagic);
    uint32_t version = 0;
    uint32_t target = 0;
    uint32_t entryPointSize = 0;
    uint64_t bytecodeSize = 0;
    uint64_t expectedHash = 0;
    if (!readCacheValue(record, offset, version) ||
        !readCacheValue(record, offset, target) ||
        !readCacheValue(record, offset, entryPointSize) ||
        !readCacheValue(record, offset, bytecodeSize) ||
        !readCacheValue(record, offset, expectedHash) ||
        version != kShaderCacheFormatVersion ||
        target != static_cast<uint32_t>(expectedTarget) ||
        entryPointSize == 0 || entryPointSize > 4096 ||
        bytecodeSize == 0 || bytecodeSize > kMaxShaderCacheBytes ||
        bytecodeSize > std::numeric_limits<size_t>::max() ||
        offset > record.size() ||
        entryPointSize > record.size() - offset)
        return false;

    const size_t payloadOffset = offset + entryPointSize;
    if (bytecodeSize != record.size() - payloadOffset)
        return false;
    entryPoint.assign(reinterpret_cast<const char *>(record.data() + offset),
                      entryPointSize);
    bytecode.assign(record.begin() + payloadOffset, record.end());
    return expectedHash == shaderCachePayloadHash(entryPoint, bytecode) &&
           shaderPayloadValid(expectedTarget, entryPoint, bytecode);
}

bool writeBinaryFileAtomically(const std::filesystem::path &path,
                               const std::vector<uint8_t> &data)
{
    std::filesystem::path temporary = path;
    temporary += "." + std::to_string(currentProcessId()) + "." +
        std::to_string(g_shaderCompileCounter.fetch_add(1)) + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(reinterpret_cast<const char *>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        stream.flush();
        if (!stream.good()) {
            stream.close();
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    bool replaced = false;
#if defined(_WIN32)
    replaced = MoveFileExW(temporary.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING |
                           MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    replaced = !error;
#endif
    if (!replaced) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    }
    return replaced;
}

/* Compiled shaders are stored as checksummed, atomically-published records.
 * Process spawn per compile costs ~35ms and stalls replay whenever a title
 * streams in new materials. */
std::filesystem::path shaderCachePath(uint64_t key)
{
    static const std::filesystem::path directory = []() {
        const char *overridePath = std::getenv("XRECOMP_SHADER_CACHE_DIR");
        std::filesystem::path dir;
        if (overridePath && overridePath[0]) {
            dir = overridePath;
        } else {
            std::filesystem::path base = currentExecutableDirectory();
            if (base.empty())
                base = std::filesystem::temp_directory_path();
            dir = base / "shader_cache";
        }
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        return dir;
    }();
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.xsc2",
                  static_cast<unsigned long long>(key));
    return directory / name;
}

bool shaderCacheEnabled()
{
    static const bool enabled = []() {
        const char *value = std::getenv("XEMU_D3D_SHADER_CACHE");
        if (value == nullptr || value[0] == '\0')
            value = std::getenv("XRECOMP_SHADER_CACHE");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

#if defined(_WIN32) && !defined(XEMU_D3D_HLE_NO_DXC_LIBRARY)
constexpr IID IID_XRECOMP_DxcBlob = {
    0x8ba5fb08, 0x5195, 0x40e2,
    {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};
constexpr IID IID_XRECOMP_DxcBlobUtf8 = {
    0x3da636c9, 0xba71, 0x4024,
    {0xa3, 0x01, 0x30, 0xcb, 0xf1, 0x25, 0x30, 0x5b}};
constexpr IID IID_XRECOMP_DxcResult = {
    0x58346cda, 0xdde7, 0x4497,
    {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};
constexpr IID IID_XRECOMP_DxcCompiler3 = {
    0x228b4687, 0x5a6a, 0x4730,
    {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};

/* In-process DXC via dxcompiler.dll: same compiler, no cmd.exe/dxc.exe spawn
 * or temp-file round trip. Falls back to the tool path if the DLL is absent. */
bool compileWithDxcLibrary(const std::filesystem::path &libraryPath,
                           const ShaderCompileRequest &request,
                           const std::vector<std::string> &arguments,
                           std::vector<uint8_t> &bytecode,
                           std::string &messages)
{
    struct LoadedDxc {
        std::wstring path;
        HMODULE module;
        DxcCreateInstanceProc createInstance;
    };
    static std::mutex loadedMutex;
    static std::vector<LoadedDxc> loadedLibraries;

    if (libraryPath.empty() || !std::filesystem::exists(libraryPath))
        return false;
    std::error_code pathError;
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(libraryPath, pathError);
    const std::wstring key =
        (pathError ? libraryPath : absolutePath).native();

    DxcCreateInstanceProc createInstance = nullptr;
    {
        std::lock_guard<std::mutex> lock(loadedMutex);
        for (const LoadedDxc &loaded : loadedLibraries) {
            if (loaded.path == key) {
                createInstance = loaded.createInstance;
                break;
            }
        }
        if (!createInstance) {
            HMODULE module = LoadLibraryExW(
                key.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (module) {
                createInstance = reinterpret_cast<DxcCreateInstanceProc>(
                    GetProcAddress(module, "DxcCreateInstance"));
                if (createInstance) {
                    loadedLibraries.push_back({key, module, createInstance});
                } else {
                    FreeLibrary(module);
                }
            }
        }
    }
    if (!createInstance)
        return false;

    IDxcCompiler3 *compiler = nullptr;
    if (FAILED(createInstance(CLSID_DxcCompiler, IID_XRECOMP_DxcCompiler3,
                              reinterpret_cast<void **>(&compiler))) ||
        !compiler)
        return false;

    std::vector<std::wstring> wide;
    for (const std::string &argument : arguments) {
        wide.emplace_back(argument.begin(), argument.end());
    }
    std::vector<const wchar_t *> pointers;
    for (const std::wstring &argument : wide)
        pointers.push_back(argument.c_str());

    DxcBuffer source = {};
    source.Ptr = request.source;
    source.Size = std::strlen(request.source);
    source.Encoding = DXC_CP_UTF8;

    IDxcResult *result = nullptr;
    HRESULT hr = compiler->Compile(&source, pointers.data(),
                                   static_cast<UINT32>(pointers.size()),
                                   nullptr, IID_XRECOMP_DxcResult,
                                   reinterpret_cast<void **>(&result));
    bool ok = false;
    if (SUCCEEDED(hr) && result) {
        IDxcBlobUtf8 *errors = nullptr;
        if (SUCCEEDED(result->GetOutput(
                DXC_OUT_ERRORS, IID_XRECOMP_DxcBlobUtf8,
                reinterpret_cast<void **>(&errors), nullptr)) && errors) {
            if (errors->GetStringLength())
                messages.assign(errors->GetStringPointer(),
                                errors->GetStringLength());
            errors->Release();
        }
        HRESULT status = E_FAIL;
        if (SUCCEEDED(result->GetStatus(&status)) && SUCCEEDED(status)) {
            IDxcBlob *object = nullptr;
            if (SUCCEEDED(result->GetOutput(
                    DXC_OUT_OBJECT, IID_XRECOMP_DxcBlob,
                    reinterpret_cast<void **>(&object), nullptr)) &&
                object) {
                const uint8_t *data =
                    static_cast<const uint8_t *>(object->GetBufferPointer());
                bytecode.assign(data, data + object->GetBufferSize());
                ok = !bytecode.empty();
                object->Release();
            }
        }
        result->Release();
    }
    compiler->Release();
    return ok;
}
#endif

} /* namespace */

std::vector<std::string> buildDxcArguments(
    const ShaderCompileRequest &request,
    const std::string &inputPath,
    const std::string &outputPath)
{
    std::vector<std::string> arguments = {
        "-E", request.entryPoint ? request.entryPoint : "main",
        "-T", request.profile ? request.profile : "ps_6_0",
        "-O3",
    };

    if (request.target != ShaderTarget::DXIL) {
        arguments.emplace_back("-spirv");
        arguments.emplace_back("-fspv-target-env=vulkan1.0");
        arguments.emplace_back("-fvk-use-dx-layout");
        arguments.emplace_back("-D");
        arguments.emplace_back("XGPU_SPIRV=1");
    }

    arguments.emplace_back("-Fo");
    arguments.emplace_back(outputPath);
    arguments.emplace_back(inputPath);
    return arguments;
}

ShaderCompileResult compileShader(const ShaderCompileRequest &request)
{
    ShaderCompileResult result;
    result.target = request.target;

    if (request.source == nullptr || request.source[0] == '\0' ||
        request.entryPoint == nullptr || request.entryPoint[0] == '\0' ||
        request.profile == nullptr || request.profile[0] == '\0') {
        result.diagnostics = "Shader source, entry point, and profile are required";
        return result;
    }
    result.entryPoint = request.entryPoint;

#if defined(_WIN32)
    constexpr const char *dxcName = "dxc.exe";
    constexpr const char *spirvCrossName = "plume_spirv_cross_msl.exe";
#else
    constexpr const char *dxcName = "dxc";
    constexpr const char *spirvCrossName = "plume_spirv_cross_msl";
#endif
    std::string embeddedDxcDiagnostics;
    const std::string dxc = resolveDxcTool(
        dxcName, XRECOMP_DXC_DEFAULT_PATH, dxcName,
        embeddedDxcDiagnostics);
    const std::filesystem::path dxcLibrary =
        dxcLibraryForExecutable(dxc);
    std::string spirvCross;
    if (request.target == ShaderTarget::METAL_SOURCE) {
        spirvCross = resolveTool(
            "XRECOMP_SPIRV_CROSS_MSL", spirvCrossName,
            XRECOMP_SPIRV_CROSS_MSL_DEFAULT_PATH, spirvCrossName);
    }
    const std::vector<std::string> semanticArguments =
        semanticDxcArguments(request);

    uint64_t cacheKey = 1469598103934665603ull;
    hashLengthDelimited(cacheKey, &kShaderCacheFormatVersion,
                        sizeof(kShaderCacheFormatVersion));
    hashString(cacheKey, shaderTargetName(request.target));
    hashLengthDelimited(cacheKey, request.entryPoint,
                        std::strlen(request.entryPoint));
    hashLengthDelimited(cacheKey, request.profile,
                        std::strlen(request.profile));
    hashLengthDelimited(cacheKey, request.source,
                        std::strlen(request.source));
    for (const std::string &argument : semanticArguments)
        hashString(cacheKey, argument);
    hashString(cacheKey, toolIdentity(dxc));
    hashString(cacheKey, toolIdentity(dxcLibrary));
    if (!spirvCross.empty())
        hashString(cacheKey, toolIdentity(spirvCross));

    const bool cacheEnabled = shaderCacheEnabled();
    const std::filesystem::path cachePath =
        cacheEnabled ? shaderCachePath(cacheKey) : std::filesystem::path();
    auto diagnosticsPrefix = [&](const char *cacheState) {
        std::ostringstream diagnostics;
        diagnostics << "target=" << shaderTargetName(request.target)
                    << " entry=" << request.entryPoint
                    << " profile=" << request.profile
                    << " tool=" << dxc
                    << " cache=" << cacheState;
        if (!embeddedDxcDiagnostics.empty())
            diagnostics << " dxc_bundle=\"" << embeddedDxcDiagnostics << '"';
        return diagnostics;
    };

    if (cacheEnabled) {
        std::string cachedEntryPoint;
        std::vector<uint8_t> cachedBytecode;
        if (decodeShaderCacheRecord(cachePath, request.target,
                                    cachedEntryPoint, cachedBytecode)) {
            result.entryPoint = std::move(cachedEntryPoint);
            result.bytecode = std::move(cachedBytecode);
            result.diagnostics = diagnosticsPrefix("hit").str();
            result.ok = true;
            return result;
        }
    }

#if defined(_WIN32) && !defined(XEMU_D3D_HLE_NO_DXC_LIBRARY)
    /* DXIL/SPIRV compile fully in-process when the selected DXC distribution
     * supplies its sibling library. Metal still needs the converter pass. */
    if (request.target != ShaderTarget::METAL_SOURCE) {
        std::string libraryMessages;
        if (compileWithDxcLibrary(dxcLibrary, request, semanticArguments,
                                  result.bytecode, libraryMessages)) {
            std::ostringstream diagnostics =
                diagnosticsPrefix(cacheEnabled ? "miss" : "disabled");
            diagnostics << " backend=dxcompiler.dll"
                        << " library=" << dxcLibrary.string();
            if (!shaderPayloadValid(request.target, result.entryPoint,
                                    result.bytecode)) {
                result.bytecode.clear();
                diagnostics << "\nDXC produced an invalid shader object";
                result.diagnostics = diagnostics.str();
                return result;
            }
            if (cacheEnabled) {
                const std::vector<uint8_t> record = encodeShaderCacheRecord(
                    request.target, result.entryPoint, result.bytecode);
                if (!writeBinaryFileAtomically(cachePath, record))
                    diagnostics << " cache_write=failed";
            }
            if (!libraryMessages.empty())
                diagnostics << '\n' << libraryMessages;
            result.diagnostics = diagnostics.str();
            result.ok = true;
            return result;
        }
        if (!libraryMessages.empty()) {
            std::ostringstream diagnostics =
                diagnosticsPrefix(cacheEnabled ? "miss" : "disabled");
            diagnostics << " backend=dxcompiler.dll"
                        << " library=" << dxcLibrary.string()
                        << '\n' << libraryMessages;
            result.diagnostics = diagnostics.str();
            return result;
        }
    }
#endif

    TemporaryShaderFiles files = makeTemporaryFiles();
    if (!writeSourceFile(files.source, request.source)) {
        std::ostringstream diagnostics =
            diagnosticsPrefix(cacheEnabled ? "miss" : "disabled");
        diagnostics << "\nUnable to write temporary HLSL source";
        result.diagnostics = diagnostics.str();
        return result;
    }

    const std::vector<std::string> arguments = buildDxcArguments(
        request, files.source.string(), files.object.string());
    const int dxcExit = runTool(dxc, arguments, files.diagnostics);
    const std::string dxcMessages = readTextFile(files.diagnostics);

    std::ostringstream diagnostics =
        diagnosticsPrefix(cacheEnabled ? "miss" : "disabled");
    diagnostics << " backend=external exit=" << dxcExit;
    if (!dxcMessages.empty())
        diagnostics << '\n' << dxcMessages;

    if (dxcExit != 0) {
        result.diagnostics = diagnostics.str();
        return result;
    }

    result.bytecode = readBinaryFile(files.object);
    if (result.bytecode.empty()) {
        diagnostics << "\nDXC produced no shader object";
        result.diagnostics = diagnostics.str();
        return result;
    }

    if (request.target == ShaderTarget::METAL_SOURCE) {
        const std::vector<std::string> metalArguments = {
            files.object.string(), files.metal.string(),
            files.metalEntryPoint.string()
        };
        const int metalExit = runTool(spirvCross, metalArguments,
                                      files.metalDiagnostics);
        const std::string metalMessages = readTextFile(files.metalDiagnostics);
        diagnostics << "\nmsl_tool=" << spirvCross << " exit=" << metalExit;
        if (!metalMessages.empty())
            diagnostics << '\n' << metalMessages;
        if (metalExit != 0) {
            result.bytecode.clear();
            result.diagnostics = diagnostics.str();
            return result;
        }

        result.bytecode = readBinaryFile(files.metal);
        if (result.bytecode.empty()) {
            diagnostics << "\nSPIRV-Cross produced no Metal source";
            result.diagnostics = diagnostics.str();
            return result;
        }
        result.entryPoint = readTextFile(files.metalEntryPoint);
        if (result.entryPoint.empty()) {
            result.bytecode.clear();
            diagnostics << "\nSPIRV-Cross produced no Metal entry point";
            result.diagnostics = diagnostics.str();
            return result;
        }
        diagnostics << "\nmsl_entry=" << result.entryPoint;
    }

    if (!shaderPayloadValid(request.target, result.entryPoint,
                            result.bytecode)) {
        result.bytecode.clear();
        diagnostics << "\nCompiler produced an invalid shader payload";
        result.diagnostics = diagnostics.str();
        return result;
    }
    if (cacheEnabled) {
        const std::vector<uint8_t> record = encodeShaderCacheRecord(
            request.target, result.entryPoint, result.bytecode);
        if (!writeBinaryFileAtomically(cachePath, record))
            diagnostics << " cache_write=failed";
    }

    result.ok = true;
    result.diagnostics = diagnostics.str();
    return result;
}

ShaderTarget shaderTargetForRenderFormat(::plume::RenderShaderFormat format)
{
    switch (format) {
    case ::plume::RenderShaderFormat::DXIL:
        return ShaderTarget::DXIL;
    case ::plume::RenderShaderFormat::SPIRV:
        return ShaderTarget::SPIRV;
    case ::plume::RenderShaderFormat::METAL:
    case ::plume::RenderShaderFormat::METAL_SOURCE:
        return ShaderTarget::METAL_SOURCE;
    default:
        return ShaderTarget::DXIL;
    }
}

::plume::RenderShaderFormat renderShaderFormatForTarget(ShaderTarget target)
{
    switch (target) {
    case ShaderTarget::DXIL:
        return ::plume::RenderShaderFormat::DXIL;
    case ShaderTarget::SPIRV:
        return ::plume::RenderShaderFormat::SPIRV;
    case ShaderTarget::METAL_SOURCE:
        return ::plume::RenderShaderFormat::METAL_SOURCE;
    }

    return ::plume::RenderShaderFormat::UNKNOWN;
}

const char *shaderTargetName(ShaderTarget target)
{
    switch (target) {
    case ShaderTarget::DXIL:
        return "dxil";
    case ShaderTarget::SPIRV:
        return "spirv";
    case ShaderTarget::METAL_SOURCE:
        return "metal-source";
    }

    return "unknown";
}

} /* namespace plume */
} /* namespace xgpu */
