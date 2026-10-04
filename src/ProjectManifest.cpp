#include "ProjectManifest.hpp"
#include <algorithm>
#include <stdexcept>

namespace DiscoProject {
namespace {
[[noreturn]] void error(const Manifest& manifest, const std::string& key, const std::string& message) {
    const auto* value = manifest.find(key);
    throw std::runtime_error(manifest.path + ':' + std::to_string(value ? value->line : 1) + ':' +
        std::to_string(value ? value->column : 1) + ": manifest error: " + key + ": " + message);
}
bool validPath(const std::string& text) {
    return !text.empty() && text.size() <= MaxProjectPathBytes &&
        std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 32 || c == 127; });
}
std::string configPath(const std::vector<std::string>& arguments, Consumer consumer) {
    std::string path;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] != "--config") {
            if (optionTakesValue(arguments[index], consumer)) {
                if (++index == arguments.size()) throw std::runtime_error(arguments[index - 1] + " requires a value.");
            }
            continue;
        }
        if (!path.empty()) throw std::runtime_error("Only one --config manifest can be specified.");
        if (++index == arguments.size() || arguments[index].empty()) throw std::runtime_error("--config requires a filename.");
        path = absolutePath(arguments[index]);
    }
    return path;
}
} // namespace

bool optionTakesValue(const std::string& flag, Consumer consumer) {
    return flag == "--config" || flag == "-o" || flag == "--target" || flag == "--origin" ||
        flag == "--memory-mapping" || flag == "--execution-memory" || flag == "--ram-bank" ||
        flag == "--rom-bank" || flag == "--ram-origin" || flag == "--stack-pointer" ||
        flag == "--entry" || flag == "--output-dir" ||
        (flag == "--emit-asm" && consumer == Consumer::Linker);
}
TargetKind parseTarget(const std::string& name) {
    if (name == "gsu" || name == "superfx") return TargetKind::GSU;
    if (name == "spc700") return TargetKind::SPC700;
    throw std::runtime_error("Unsupported target '" + name + "'. Use 'gsu' or 'spc700'.");
}
const Value* Manifest::find(const std::string& key) const {
    const auto found = values.find(key); return found == values.end() ? nullptr : &found->second;
}
std::string Manifest::string(const std::string& key, const std::string& fallback) const {
    const auto* value = find(key); if (!value) return fallback;
    if (value->kind != Value::Kind::String) error(*this, key, "Expected string.");
    return value->text;
}
bool Manifest::boolean(const std::string& key, bool fallback) const {
    const auto* value = find(key); if (!value) return fallback;
    if (value->kind != Value::Kind::Boolean) error(*this, key, "Expected boolean.");
    return value->boolean;
}
std::uint32_t Manifest::number(const std::string& key, std::uint32_t fallback) const {
    const auto* value = find(key); if (!value) return fallback;
    if (value->kind != Value::Kind::Integer || value->integer < 0 || value->integer > 0xffffffffll)
        error(*this, key, "Expected nonnegative 32-bit integer.");
    return static_cast<std::uint32_t>(value->integer);
}
TargetKind Manifest::target() const { return parseTarget(string("project.target", "gsu")); }

void Manifest::validate() const {
    const std::map<std::string, std::uint32_t> limits = {
        {"target.gsu.origin", 0xffffff}, {"target.gsu.ram_bank", 1}, {"target.gsu.rom_bank", 0x5f},
        {"target.gsu.ram_origin", 0xffff}, {"target.gsu.stack_pointer", 0xfffe}, {"target.spc700.origin", 0xffff}};
    for (const auto& item : values) {
        const auto& key = item.first; const auto& value = item.second;
        const bool path = key == "output.directory" || key == "output.binary" || key == "output.assembly";
        if (key == "project.sources") {
            if (value.kind != Value::Kind::Strings) error(*this, key, "Expected array of quoted source paths.");
            for (const auto& name : value.strings) if (!validPath(name)) error(*this, key, "Invalid source path.");
        } else if (path || key == "project.name" || key == "project.target" || key == "runtime.entry") {
            const auto text = string(key);
            if (!validPath(text)) error(*this, key, "Empty string or invalid control character.");
            if (key == "project.name" && (text.size() > 64 || std::any_of(text.begin(), text.end(), [](char c) {
                return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
            }))) error(*this, key, "Name must contain 1..64 ASCII letters, digits, '_' or '-'.");
        } else if (key == "runtime.initialize" || key == "runtime.host_initialized_globals" || key == "target.gsu.initialize_runtime") {
            (void)boolean(key);
        } else if (key == "target.gsu.memory_mapping" || key == "target.gsu.execution_memory") {
            const auto text = string(key);
            if (key == "target.gsu.memory_mapping" ? text != "lorom" && text != "hirom" : text != "rom" && text != "ram")
                error(*this, key, "Unsupported mapping/execution memory.");
        } else {
            const auto found = limits.find(key);
            if (found == limits.end()) error(*this, key, "Unknown or unsupported manifest key.");
            if (number(key, 0) > found->second) error(*this, key, "Value is out of range.");
        }
    }
    try { (void)target(); } catch (const std::exception& exception) { error(*this, "project.target", exception.what()); }
    if (find("runtime.initialize") && find("target.gsu.initialize_runtime"))
        error(*this, "runtime.initialize", "Use only one initialization key, not both aliases.");
}

std::unique_ptr<Manifest> commandManifest(const std::vector<std::string>& arguments, Consumer consumer) {
    const auto path = configPath(arguments, consumer);
    return path.empty() ? nullptr : std::make_unique<Manifest>(Manifest::read(path));
}
std::vector<std::string> configurationArguments(const std::vector<std::string>& arguments, Consumer consumer,
                                                const Manifest* loaded) {
    if (arguments.empty() || arguments.size() > 1024) throw std::runtime_error("Invalid command-line argument count.");
    auto owner = loaded ? nullptr : commandManifest(arguments, consumer);
    if (!loaded && !owner) return arguments;
    const auto& manifest = loaded ? *loaded : *owner;
    const auto& path = manifest.path;
    auto target = manifest.target();
    bool initialize = manifest.boolean("runtime.initialize", manifest.boolean("target.gsu.initialize_runtime"));
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--target") {
            if (++index == arguments.size()) throw std::runtime_error("--target requires a value.");
            target = parseTarget(arguments[index]);
        } else if (arguments[index] == "--init-runtime") initialize = true;
        else if (arguments[index] == "--no-init-runtime") initialize = false;
        else if (optionTakesValue(arguments[index], consumer)) {
            if (++index == arguments.size()) throw std::runtime_error(arguments[index - 1] + " requires a value.");
        }
    }
    const std::string prefix = target == TargetKind::GSU ? "target.gsu." : "target.spc700.";
    std::vector<std::string> result{arguments.front(), "--target", target == TargetKind::GSU ? "gsu" : "spc700"};
    const auto option = [&](const std::string& key, const std::string& flag) {
        if (const auto* value = manifest.find(key)) {
            result.push_back(flag);
            result.push_back(value->kind == Value::Kind::String ? value->text : std::to_string(manifest.number(key, 0)));
        }
    };
    if (target == TargetKind::GSU) {
        option(prefix + "origin", "--origin");
        option(prefix + "memory_mapping", "--memory-mapping");
        option(prefix + "execution_memory", "--execution-memory");
    } else if (manifest.find(prefix + "origin")) {
        // Origin belongs to the future SPC linker. Shared frontend inspection
        // may use the project, but must not send GSU placement flags to it.
        if (consumer == Consumer::Linker) option(prefix + "origin", "--origin");
    }
    if (consumer == Consumer::Linker) {
        if (initialize) result.push_back("--init-runtime");
        if (manifest.boolean("runtime.host_initialized_globals")) result.push_back("--host-initialized-globals");
        if (target == TargetKind::GSU) {
            option(prefix + "ram_bank", "--ram-bank"); option(prefix + "ram_origin", "--ram-origin");
            option(prefix + "rom_bank", "--rom-bank");
            if (initialize) { option(prefix + "stack_pointer", "--stack-pointer"); option("runtime.entry", "--entry"); }
        }
        const auto directory = absolutePath(manifest.string("output.directory", "build"), parentPath(path));
        result.push_back("-o"); result.push_back(absolutePath(manifest.string("output.binary", manifest.string("project.name", "new") + ".bin"), directory));
        if (manifest.find("output.assembly")) {
            result.push_back("--emit-asm"); result.push_back(absolutePath(manifest.string("output.assembly"), directory));
        }
    }
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--config") { ++index; continue; }
        result.push_back(arguments[index]);
        if (optionTakesValue(arguments[index], consumer)) {
            if (++index == arguments.size()) throw std::runtime_error(arguments[index - 1] + " requires a value.");
            result.push_back(arguments[index]);
        }
    }
    return result;
}
void protectManifest(const Manifest& manifest, const std::vector<std::string>& paths) {
    for (const auto& output : paths) {
        if (samePath(output, manifest.path)) throw std::runtime_error("Output path must not overwrite the manifest.");
        if (const auto* sources = manifest.find("project.sources"))
            for (const auto& source : sources->strings)
                if (samePath(output, absolutePath(source, parentPath(manifest.path))))
                    throw std::runtime_error("Output path must not overwrite a project source.");
    }
}
} // namespace DiscoProject
