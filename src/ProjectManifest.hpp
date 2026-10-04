#pragma once

#include "TargetConfig.hpp"
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DiscoProject {
constexpr std::size_t MaxManifestBytes = 64 * 1024;
constexpr std::size_t MaxProjectSources = 128;
constexpr std::size_t MaxProjectPathBytes = 4096;

struct Value {
    enum class Kind { String, Integer, Boolean, Strings };
    Kind kind = Kind::String;
    std::string text;
    std::int64_t integer = 0;
    bool boolean = false;
    std::vector<std::string> strings;
    std::size_t line = 1, column = 1;
};

// Owns decoded values. The bounded reader supports the documented manifest
// subset of TOML; unsupported TOML constructs are errors, never ignored text.
struct Manifest {
    std::string path;
    std::map<std::string, Value> values;
    static Manifest parse(const std::string& source, const std::string& path);
    static Manifest read(const std::string& path);
    void validate() const;
    const Value* find(const std::string& key) const;
    std::string string(const std::string& key, const std::string& fallback = "") const;
    bool boolean(const std::string& key, bool fallback = false) const;
    std::uint32_t number(const std::string& key, std::uint32_t fallback) const;
    TargetKind target() const;
};

enum class Consumer { Compiler, Linker };
bool optionTakesValue(const std::string& flag, Consumer consumer);
// Arguments include the program name. Manifest defaults are prepended, so CLI
// flags win independently of --config's position; no shell evaluation occurs.
std::unique_ptr<Manifest> commandManifest(const std::vector<std::string>& arguments, Consumer consumer);
std::vector<std::string> configurationArguments(const std::vector<std::string>& arguments, Consumer consumer,
                                                const Manifest* manifest = nullptr);
TargetKind parseTarget(const std::string& name);
std::string absolutePath(const std::string& path, const std::string& base = "");
std::string parentPath(const std::string& path);
bool samePath(const std::string& left, const std::string& right);
void createDirectories(const std::string& path);
void protectManifest(const Manifest& manifest, const std::vector<std::string>& paths);
} // namespace DiscoProject
