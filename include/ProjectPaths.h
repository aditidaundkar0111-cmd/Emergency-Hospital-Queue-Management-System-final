#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ProjectPaths {
inline bool isRegularFileIfPresent(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) throw std::runtime_error("Could not inspect project root: " + error.message());
    if (!exists) return false;
    const bool regular = std::filesystem::is_regular_file(path, error);
    if (error) throw std::runtime_error("Could not inspect project root: " + error.message());
    return regular;
}

inline std::filesystem::path executablePath(const char* argv0) {
#if defined(_WIN32)
    (void)argv0;
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("Could not determine the application executable path.");
        if (length < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), length));
        if (buffer.size() >= 32768)
            throw std::runtime_error("Application executable path exceeds the Windows path limit.");
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    std::filesystem::path path(argv0 == nullptr ? "" : argv0);
    if (path.is_relative()) path = std::filesystem::current_path(error) / path;
    if (error) throw std::runtime_error("Could not determine the application working directory.");
    path = std::filesystem::canonical(path, error);
    if (error) throw std::runtime_error("Could not resolve the application executable path: " + error.message());
    return path;
#endif
}

inline std::filesystem::path projectRoot(const char* argv0) {
    const std::filesystem::path executable = executablePath(argv0);
    std::filesystem::path candidate = executable.parent_path();
    for (;;) {
        const bool hasBuildFile = isRegularFileIfPresent(candidate / "CMakeLists.txt");
        const bool hasDatabaseHeader = isRegularFileIfPresent(candidate / "include" / "DatabaseManager.h");
        if (hasBuildFile && hasDatabaseHeader) return candidate;
        const std::filesystem::path parent = candidate.parent_path();
        if (parent.empty() || parent == candidate)
            throw std::runtime_error("Could not locate the project root above the application executable. "
                                     "Run a project build from the project tree.");
        candidate = parent;
    }
}
}
