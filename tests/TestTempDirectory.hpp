#pragma once
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

// Each process owns a newly created directory, so independent build/test runs
// cannot remove another run's fixtures (including on Windows).
class TestTempDirectory {
public:
    explicit TestTempDirectory(const std::string& name) {
        const auto parent = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 32; ++attempt) {
            const auto candidate = parent / ("discoc-" + name + "-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("Cannot create a private test directory.");
    }
    TestTempDirectory(const TestTempDirectory&) = delete;
    TestTempDirectory& operator=(const TestTempDirectory&) = delete;
    ~TestTempDirectory() noexcept {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    const std::filesystem::path& path() const { return path_; }
    // Successful runs surface cleanup failures; unwinding still cleans up
    // without replacing the original assertion/diagnostic failure.
    void cleanup() {
        std::filesystem::remove_all(path_);
        path_.clear();
    }
private:
    std::filesystem::path path_;
};
