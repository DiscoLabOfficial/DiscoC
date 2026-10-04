#include "ProjectManifest.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#if __cplusplus >= 201703L
#include <filesystem>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace DiscoProject {
namespace {
#if defined(_WIN32) && __cplusplus < 201703L
// Windows CRT stat() has no usable inode identity. Query metadata through
// read-only handles instead; RAII closes both before any output is opened.
bool sameNativeFile(const std::string& left, const std::string& right) {
    struct Handle {
        HANDLE value;
        explicit Handle(const std::string& path) : value(CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr)) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    const Handle a(left), b(right);
    BY_HANDLE_FILE_INFORMATION a_info{}, b_info{};
    return a.value != INVALID_HANDLE_VALUE && b.value != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a.value, &a_info) && GetFileInformationByHandle(b.value, &b_info) &&
        a_info.dwVolumeSerialNumber == b_info.dwVolumeSerialNumber &&
        a_info.nFileIndexHigh == b_info.nFileIndexHigh && a_info.nFileIndexLow == b_info.nFileIndexLow;
}
#endif
std::string workingDirectory() {
    std::array<char, MaxProjectPathBytes + 1> buffer{};
#ifdef _WIN32
    if (!_getcwd(buffer.data(), static_cast<int>(buffer.size()))) throw std::runtime_error("Cannot resolve working directory.");
#else
    if (!getcwd(buffer.data(), buffer.size())) throw std::runtime_error("Cannot resolve working directory.");
#endif
    std::string path = buffer.data();
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}
} // namespace
std::string absolutePath(const std::string& input, const std::string& base) {
    if (input.empty() || input.size() > MaxProjectPathBytes || input.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid project path.");
    auto path = input;
    std::replace(path.begin(), path.end(), '\\', '/');
    bool drive = path.size() >= 2 && path[1] == ':';
    if (drive && (path.size() < 3 || path[2] != '/')) throw std::runtime_error("Drive-relative project paths are unsupported.");
#ifdef _WIN32
    if (path.front() == '/' && path.compare(0, 2, "//") != 0) {
        const auto current = workingDirectory();
        if (current.size() < 2 || current[1] != ':') throw std::runtime_error("Root-relative paths require a current drive.");
        path = current.substr(0, 2) + path;
        drive = true;
    }
#endif
    if (path.front() != '/' && !drive) {
        const auto directory = base.empty() ? workingDirectory() : absolutePath(base);
        path = directory + '/' + path; std::replace(path.begin(), path.end(), '\\', '/');
    }
    std::string prefix;
    std::size_t beginning = 0;
    if (path.size() >= 3 && path[1] == ':') { prefix = path.substr(0, 3); beginning = 3; }
#ifdef _WIN32
    else if (path.compare(0, 2, "//") == 0) {
        const auto server = path.find('/', 2), share = server == std::string::npos ? server : path.find('/', server + 1);
        if (server == std::string::npos || server == 2 || server + 1 == path.size() ||
            (share != std::string::npos && share == server + 1))
            throw std::runtime_error("Invalid UNC project path.");
        prefix = path.substr(0, share == std::string::npos ? path.size() : share) + '/'; beginning = prefix.size();
    }
#endif
    else { prefix = "/"; beginning = 1; }
    std::vector<std::string> parts;
    std::istringstream stream(path.substr(std::min(beginning, path.size())));
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else parts.push_back(part);
    }
    auto result = prefix;
    for (const auto& component : parts) { if (result.back() != '/') result += '/'; result += component; }
    if (result.size() > MaxProjectPathBytes) throw std::runtime_error("Project path exceeds 4096 bytes.");
    return result;
}
std::string parentPath(const std::string& path) {
    const auto absolute = absolutePath(path);
    const auto slash = absolute.find_last_of('/');
    return absolute.substr(0, slash + 1);
}
bool sameExistingFile(const std::string& left, const std::string& right) {
#if __cplusplus >= 201703L
    std::error_code error;
    return std::filesystem::equivalent(left, right, error) && !error;
#elif defined(_WIN32)
    return sameNativeFile(left, right);
#else
    struct stat a_stat{}, b_stat{};
    return stat(left.c_str(), &a_stat) == 0 && stat(right.c_str(), &b_stat) == 0 && a_stat.st_ino != 0 &&
        a_stat.st_ino == b_stat.st_ino && a_stat.st_dev == b_stat.st_dev;
#endif
}
bool samePath(const std::string& left, const std::string& right) {
    if (sameExistingFile(left, right)) return true;
#if __cplusplus >= 201703L
    // Preserve existing symlink components until canonicalization, especially
    // before '..'; lexical normalization alone can miss an output alias.
    auto a = std::filesystem::weakly_canonical(std::filesystem::absolute(left)).generic_string();
    auto b = std::filesystem::weakly_canonical(std::filesystem::absolute(right)).generic_string();
#else
    auto a = absolutePath(left), b = absolutePath(right);
#endif
#if defined(_WIN32) || defined(__DJGPP__)
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return a == b;
}
void createDirectories(const std::string& path) {
#if __cplusplus >= 201703L
    std::filesystem::create_directories(path);
#else
    const auto absolute = absolutePath(path);
    std::size_t root = absolute.size() >= 3 && absolute[1] == ':' ? 3 : 1;
#ifdef _WIN32
    if (absolute.compare(0, 2, "//") == 0) root = absolute.find('/', absolute.find('/', 2) + 1) + 1;
#endif
    for (std::size_t index = root; index <= absolute.size(); ++index) {
        if (index != absolute.size() && absolute[index] != '/') continue;
        const auto directory = absolute.substr(0, index);
        if (directory.back() == ':' || directory == "/") continue;
        struct stat info{};
        if (stat(directory.c_str(), &info) == 0) {
            if ((info.st_mode & S_IFMT) != S_IFDIR) throw std::runtime_error("Output directory is not a directory: " + directory);
            continue;
        }
#ifdef _WIN32
        const auto status = _mkdir(directory.c_str());
#else
        const auto status = mkdir(directory.c_str(), 0777);
#endif
        if (status != 0 && (errno != EEXIST || stat(directory.c_str(), &info) != 0 || (info.st_mode & S_IFMT) != S_IFDIR))
            throw std::runtime_error("Cannot create output directory: " + directory);
    }
#endif
}
} // namespace DiscoProject
