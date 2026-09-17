#include "runner.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <cwctype>
#include <iterator>
#include <stdexcept>

namespace
{
constexpr size_t kOutputLimit = 64 * 1024;

class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset()
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); }
        value_ = nullptr;
    }
private:
    HANDLE value_;
};

std::wstring linuxPath(const std::filesystem::path& path)
{
    auto text = std::filesystem::absolute(path).generic_wstring();
    if (text.size() < 3 || text[1] != L':')
    {
        throw std::runtime_error("The project must be on a local Windows drive accessible through /mnt in WSL.");
    }
    return L"/mnt/" + std::wstring(1, static_cast<wchar_t>(towlower(text[0]))) + text.substr(2);
}

// Windows CreateProcess argument quoting, including trailing backslashes.
std::wstring quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : value)
    {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') { result.append(slashes * 2 + 1, L'\\'); }
        else { result.append(slashes, L'\\'); }
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
}

std::wstring widen(const std::string& text)
{
    if (text.empty()) { return {}; }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) { return {}; }
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string readUtf8(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { throw std::runtime_error("Could not open " + path.string()); }
    if (std::filesystem::file_size(path) > 32768) { throw std::runtime_error("Keep snippets below 32 KB."); }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeUtf8(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(text.data(), static_cast<std::streamsize>(text.size())))
    {
        throw std::runtime_error("Could not save " + path.string());
    }
}

RunResult runSource(const std::filesystem::path& root, const std::string& source, RunMode mode)
{
    RunResult result;
    auto start = GetTickCount64();
    std::filesystem::path runDirectory;
    try
    {
        if (source.empty() || source.size() > 32768) { throw std::runtime_error("Enter between 1 byte and 32 KB of C source."); }
        auto engine = root / ".build" / "src" / "clauf";
        if (!std::filesystem::is_regular_file(engine)) { throw std::runtime_error("Clauf is missing. Build the engine using build.py first."); }
        runDirectory = root / ".native-runs" / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(start));
        std::filesystem::create_directories(runDirectory);
        auto sourcePath = runDirectory / "main.c";
        writeUtf8(sourcePath, source);

        wchar_t systemDirectory[MAX_PATH]{};
        GetSystemDirectoryW(systemDirectory, MAX_PATH);
        auto executable = std::filesystem::path(systemDirectory) / "wsl.exe";
        std::wstring command = quote(executable.wstring()) + L" --distribution Ubuntu --exec timeout --verbose -k 1s 4s prlimit --as=536870912 --cpu=3:4 --core=0 -- "
            + quote(linuxPath(engine)) + L" " + quote(linuxPath(sourcePath));
        if (mode == RunMode::eBytecode) { command += L" --compile-only --dump-bytecode"; }
        if (mode == RunMode::eAst) { command += L" --compile-only --dump-ast"; }

        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE rawRead = nullptr, rawWrite = nullptr;
        if (!CreatePipe(&rawRead, &rawWrite, &security, 0)) { throw std::runtime_error("Could not create the output pipe."); }
        Handle readPipe(rawRead), writePipe(rawWrite);
        SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input.get();
        startup.hStdOutput = startup.hStdError = writePipe.get();
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &processInfo))
        {
            throw std::runtime_error("Could not launch WSL (Windows error " + std::to_string(GetLastError()) + ").");
        }
        Handle process(processInfo.hProcess), thread(processInfo.hThread);
        writePipe.reset();
        std::array<char, 4096> buffer{};
        bool finished = false;
        while (true)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr)) { break; }
            if (available)
            {
                DWORD received = 0;
                if (!ReadFile(readPipe.get(), buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr)) { break; }
                size_t remaining = kOutputLimit - result.output.size();
                result.output.append(buffer.data(), std::min<size_t>(received, remaining));
                if (received > remaining) { result.truncated = true; }
            }
            else if (finished) { break; }
            else { Sleep(10); }
            if (GetTickCount64() - start > 12000)
            {
                result.timedOut = true;
                TerminateProcess(process.get(), 124);
                break;
            }
            finished = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
        }
        WaitForSingleObject(process.get(), 2000);
        GetExitCodeProcess(process.get(), &result.exitCode);
        // Programs may return timeout's reserved exit codes themselves. Require
        // a supervisor diagnostic as well; WSL may expose SIGXCPU directly as 24.
        const bool limitExit = result.exitCode == 124 || result.exitCode == 137 ||
            result.exitCode == 152 || result.exitCode == 24;
        const bool limitDiagnostic = result.output.find("timeout: sending signal") != std::string::npos ||
            result.output.find("timeout: the monitored command") != std::string::npos;
        result.timedOut = result.timedOut || (limitExit && limitDiagnostic);
        auto inputName = narrow(linuxPath(sourcePath));
        size_t position = 0;
        while ((position = result.output.find(inputName, position)) != std::string::npos)
        {
            result.output.replace(position, inputName.size(), "main.c");
            position += 6;
        }
    }
    catch (const std::exception& error) { result.error = error.what(); }
    if (!runDirectory.empty())
    {
        std::error_code ignored;
        std::filesystem::remove(runDirectory / "main.c", ignored);
        std::filesystem::remove(runDirectory, ignored);
    }
    result.elapsedMs = GetTickCount64() - start;
    return result;
}
