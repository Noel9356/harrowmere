#include "engine/kernel/filesystem_utilities.hpp"

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN and NOMINMAX are defined for the whole build through
// the root CMakeLists; windows.h must not be included without them.
#include <windows.h>
#elif defined(__APPLE__)
// macOS has no /proc: the executable path comes from the dyld API below.
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <vector>

namespace engine
{

std::filesystem::path executableDirectory()
{
    // The answer cannot change for a running process (the executable is
    // mapped at startup), so the query - a readlink syscall, a Win32
    // module-path walk or a dyld query - runs once and every later caller
    // reads the cached value. Startup resolves the executable directory
    // several times (configuration paths, module plugin directory, mod
    // package roots), and so does every configuration reload.
    static const std::filesystem::path cached = []() {
#if defined(_WIN32)
        wchar_t modulePath[MAX_PATH] = {};
        const DWORD written          = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (written == 0)
        {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(modulePath).parent_path();
#elif defined(__APPLE__)
        // _NSGetExecutablePath answers in two steps: called with a buffer
        // too small (nullptr, size 0) it reports the needed byte count
        // through `size` and fails; called again with that buffer it
        // succeeds. The path may be a symlink-resolvable relative on some
        // launchers, but the directory of it is still the install root the
        // runtime layout sits in - the same contract the other platforms'
        // queries honor.
        std::uint32_t size = 0;
        if (_NSGetExecutablePath(nullptr, &size) != 0 && size > 0)
        {
            std::vector<char> executablePath(size);
            if (_NSGetExecutablePath(executablePath.data(), &size) == 0)
            {
                return std::filesystem::path(executablePath.data()).parent_path();
            }
        }
        return std::filesystem::current_path();
#else
        char executablePath[PATH_MAX] = {};
        const ssize_t written         = ::readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1);
        if (written <= 0)
        {
            return std::filesystem::current_path();
        }
        executablePath[written] = '\0';
        return std::filesystem::path(executablePath).parent_path();
#endif
    }();
    return cached;
}

} // namespace engine
