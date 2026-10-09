#include "ProjectManifest.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace DiscoProject;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void reject(const std::string& source, const std::string& diagnostic) {
    try { (void)Manifest::parse(source, "test.toml"); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos,
                "Wrong manifest diagnostic: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Invalid manifest accepted: " + source.substr(0, 100));
}
std::string lastValue(const std::vector<std::string>& arguments, const std::string& flag, Consumer consumer) {
    std::string result;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == flag) result = arguments.at(++index);
        else if (optionTakesValue(arguments[index], consumer)) ++index;
    }
    return result;
}
bool hasFlag(const std::vector<std::string>& arguments, const std::string& flag, Consumer consumer) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == flag) return true;
        if (optionTakesValue(arguments[index], consumer)) ++index;
    }
    return false;
}
void parsing() {
    const auto manifest = Manifest::parse(
        "# Hardware project\r\n[project]\r\nname = 'multi-file'\r\n"
        "target = \"superfx\"\r\nsources = [\r\n \"a.dc\", # first\r\n 'b.dc',\r\n]\r\n"
        "[target.superfx]\r\norigin = 0x70_0900\r\nram_bank = 0b1\r\nstack_pointer = 0o20000\r\n"
        "[runtime]\r\ninitialize = true\r\n[output]\r\nassembly = \"final\\u0020\\U0001F600.s\"\r\n",
        absolutePath("test.toml"));
    require(manifest.target() == TargetKind::GSU, "SuperFX target alias");
    require(manifest.number("target.gsu.origin", 0) == 0x700900, "Hex address decoding");
    require(manifest.number("target.gsu.ram_bank", 0) == 1, "Binary integer decoding");
    require(manifest.number("target.gsu.stack_pointer", 0) == 8192, "Octal integer decoding");
    require(manifest.boolean("runtime.initialize"), "Boolean decoding");
    require(manifest.find("project.sources")->strings == std::vector<std::string>{"a.dc", "b.dc"}, "Multiline sources");
    require(manifest.string("output.assembly") == "final \xf0\x9f\x98\x80.s", "Unicode escapes");
    require(manifest.find("target.gsu.origin")->line == 10, "Value source location");
    require(Manifest::parse("", "empty.toml").target() == TargetKind::GSU, "Empty linker config defaults");
    require(Manifest::parse("[target.gsu]\norigin = +32768", "signed.toml").number("target.gsu.origin", 0) == 32768,
            "Signed decimal decoding");
    require(Manifest::parse("[output]\nbinary = 'quote\"\\file.bin'", "literal.toml").string("output.binary") == "quote\"\\file.bin",
            "Literal strings must not decode escapes");
    const auto escapes = Manifest::parse("[output]\nbinary = \"quote\\\"slash\\\\.bin\"", "escape.toml");
    require(escapes.string("output.binary") == "quote\"slash\\.bin", "Basic strings must decode escapes");
    std::string sources = "[project]\nsources = [";
    for (std::size_t index = 0; index < MaxProjectSources; ++index) sources += "'a.dc',";
    require(Manifest::parse(sources + ']', "limit.toml").find("project.sources")->strings.size() == MaxProjectSources,
            "Source limit boundary");
    reject(sources + "'b.dc']", "Source count exceeds 128");
    std::string imports = "[compiler]\nimport_paths = [";
    for (std::size_t index = 0; index < 64; ++index) imports += "'lib',";
    require(Manifest::parse(imports + ']', "imports.toml").find("compiler.import_paths")->strings.size() == 64,
            "Import path limit boundary");
    reject(imports + "'more']", "Import path count exceeds 64");
}
void invalidInput() {
    reject("[unknown]", "Unknown manifest table");
    reject("[project]\nunknown = 1", "Unknown or unsupported manifest key");
    reject("[project]\nname = 'a'\nname = 'b'", "Duplicate key");
    reject("[project]\n[project]", "Duplicate table");
    reject("[target.superfx]\n[target.gsu]", "Duplicate table");
    reject("[runtime]\ninitialize = true\n[target.gsu]\ninitialize_runtime = false", "not both aliases");
    reject("[project]\ntarget = 'z80'", "Unsupported target");
    reject("[project]\nname = '../bad'", "Name must contain");
    reject("[project]\nsources = 'a.dc'", "Expected array");
    reject("[project]\nsources = [1]", "array of quoted strings");
    reject("[project]\nsources = ['a.dc' 'b.dc']", "Expected ','");
    reject("[project]\nsources = [", "array of quoted strings");
    reject("[project]\nsources = ['']", "Invalid source path");
    reject("[project]\nsources = ['a.dc', ['b.dc']]", "array of quoted strings");
    reject("[compiler]\nimport_paths = 'lib'", "Expected array");
    reject("[compiler]\nimport_paths = [1]", "array of quoted strings");
    reject("[compiler]\nimport_paths = ['']", "Invalid import path");
    reject("[compiler]\nunknown = true", "unsupported manifest key");
    reject("[compiler]\noptimize = 1", "Expected boolean");
    reject("[compiler]\noptimize = 'true'", "Expected boolean");
    reject("[runtime]\ninitialize = 'true'", "Expected boolean");
    reject("[runtime]\ninitialize = True", "integer or boolean");
    reject("[target.gsu]\norigin = '0x8000'", "32-bit integer");
    for (const auto& number : {"0x", "0X8000", "0x_8000", "0x8000_", "0x80__00", "08", "0_8", "0b2", "+0x8000", "1.0", "1979-05-27"})
        reject("[target.gsu]\norigin = " + std::string(number), "manifest error");
    reject("[target.gsu]\norigin = 9223372036854775808", "signed 64-bit range");
    reject("[target.gsu]\norigin = -9223372036854775809", "signed 64-bit range");
    reject("[target.gsu]\norigin = -9223372036854775808", "nonnegative 32-bit integer");
    reject("[target.gsu]\norigin = 0x1000000", "out of range");
    reject("[target.gsu]\nram_bank = 2", "out of range");
    reject("[target.gsu]\nrom_bank = 0x60", "out of range");
    reject("[target.gsu]\nstack_pointer = 0xffff", "out of range");
    reject("[target.spc700]\norigin = 0x10000", "out of range");
    reject("[target.spc700]\nstack_pointer = 0xef", "unsupported manifest key");
    reject("[target.gsu]\nmemory_mapping = 'linear'", "Unsupported mapping");
    reject("[target.gsu]\nexecution_memory = 'sram'", "Unsupported mapping");
    reject("[[sources]]", "Array-of-table syntax");
    reject("[project]\nname.x = 'a'", "dotted assignment keys");
    reject("[project]\n\"name\" = 'a'", "Expected bare key");
    reject("[output]\nbinary = {name='a'}", "integer or boolean");
    reject("[output]\nbinary = 'a' trailing", "Expected newline");
    reject("[output]\nbinary = \"\"\"multi\"\"\"", "Expected newline");
    reject("[output]\nbinary = 'unterminated", "Unterminated string");
    reject("[output]\nbinary = 'two\nlines'", "Multiline strings");
    reject("[output]\nbinary = \"\\z\"", "Invalid string escape");
    reject("[output]\nbinary = \"\\uZZZZ\"", "Invalid Unicode escape");
    reject("[output]\nbinary = \"\\uD800\"", "Invalid Unicode scalar");
    reject("[output]\nbinary = \"\\U00110000\"", "Invalid Unicode scalar");
    reject("[output]\nbinary = \"\\u0000\"", "invalid control character");
    reject("[output]\nbinary = \"\\n\"", "invalid control character");
    reject("[output]\nbinary = '" + std::string(4097, 'a') + "'", "String exceeds 4096");
    reject(std::string(MaxManifestBytes + 1, '#'), "Manifest exceeds 64 KiB");
    reject("# comment\n" + std::string(1, '\0'), "test.toml:2:1");
    reject("# comment\n" + std::string(1, static_cast<char>(0xff)), "test.toml:2:1");
    reject("#\rnot-newline", "Carriage return");
    reject("# " + std::string("\xc0\x80", 2), "Invalid UTF-8");
    reject("# " + std::string("\xed\xa0\x80", 3), "Invalid UTF-8");
    reject("# " + std::string("\xf4\x90\x80\x80", 4), "Invalid UTF-8");
    // Every prefix exercises truncation and parser progress; a successful
    // prefix is allowed only when it is itself a complete valid manifest.
    const std::string sample = "[project]\nsources = [\"one.dc\", \"two.dc\"]\n[target.gsu]\norigin = 0x700900\n";
    for (std::size_t size = 0; size <= sample.size(); ++size) {
        try { (void)Manifest::parse(sample.substr(0, size), "prefix.toml"); }
        catch (const std::runtime_error&) {}
    }
}
void precedence() {
    const auto manifest = Manifest::parse(
        "[project]\ntarget = 'superfx'\nname = 'demo'\n[target.gsu]\norigin = 0x700900\n"
        "stack_pointer = 0x2000\ninitialize_runtime = true\n[target.spc700]\norigin = 0x0200\n"
        "[runtime]\nentry = 'start'\n[output]\ndirectory = 'build'\nassembly = 'final.s'\n",
        absolutePath("test.toml"));
    for (const auto& args : {std::vector<std::string>{"discld", "--origin", "0x701000", "--config", "test.toml"},
                            std::vector<std::string>{"discld", "--config", "test.toml", "--origin", "0x701000"}}) {
        const auto expanded = configurationArguments(args, Consumer::Linker, &manifest);
        require(lastValue(expanded, "--origin", Consumer::Linker) == "0x701000", "CLI wins over manifest in either order");
        require(!hasFlag(expanded, "--config", Consumer::Linker), "Config pair must be removed");
        require(lastValue(expanded, "-o", Consumer::Linker) == absolutePath("build/demo.bin"), "Manifest output is manifest-relative");
    }
    const auto no_init = configurationArguments({"discld", "--config", "test.toml", "--no-init-runtime"}, Consumer::Linker, &manifest);
    require(!hasFlag(no_init, "--init-runtime", Consumer::Linker) && !hasFlag(no_init, "--stack-pointer", Consumer::Linker) &&
            !hasFlag(no_init, "--entry", Consumer::Linker), "Disabling startup omits its manifest-only settings");
    const auto selected = configurationArguments({"discc", "--target", "spc700"}, Consumer::Compiler, &manifest);
    require(lastValue(selected, "--target", Consumer::Compiler) == "spc700" && !hasFlag(selected, "--origin", Consumer::Compiler),
            "SPC frontend does not receive GSU options");
    const auto odd_names = configurationArguments({"discld", "-o", "--target", "--entry", "--no-init-runtime"}, Consumer::Linker, &manifest);
    require(lastValue(odd_names, "--target", Consumer::Linker) == "gsu" && hasFlag(odd_names, "--stack-pointer", Consumer::Linker),
            "Flag-looking values must not change defaults");
    const auto odd_config = configurationArguments({"discld", "--config", "--target"}, Consumer::Linker, &manifest);
    require(lastValue(odd_config, "--target", Consumer::Linker) == "gsu", "Manifest filename is not a target flag");
    const auto cli_output = configurationArguments({"discld", "-o", "local.bin"}, Consumer::Linker, &manifest);
    require(lastValue(cli_output, "-o", Consumer::Linker) == "local.bin", "CLI paths retain working-directory semantics");
    require(samePath(absolutePath("a/../test.toml"), absolutePath("test.toml")), "Lexical alias protection");
    require(parentPath(absolutePath("dir/file.toml")) == absolutePath("dir") + '/', "Manifest parent resolution");
    const auto imports = Manifest::parse("[compiler]\nimport_paths = ['src', 'lib path']", absolutePath("dir/imports.toml"));
    const auto configured = configurationArguments({"discc", "--check"}, Consumer::Compiler, &imports);
    require(configured == std::vector<std::string>{"discc", "--target", "gsu", "--import-path", absolutePath("dir/src"),
            "--import-path", absolutePath("dir/lib path"), "--check"}, "Manifest import directories must retain order and manifest-relative resolution");
    for (const auto& flags : {std::vector<std::string>{"--import-path", "override"},
                             std::vector<std::string>{"-I", "override"}, std::vector<std::string>{"-Ioverride"}}) {
        std::vector<std::string> arguments{"discc"}; arguments.insert(arguments.end(), flags.begin(), flags.end());
        const auto explicit_paths = configurationArguments(arguments, Consumer::Compiler, &imports);
        require(std::find(explicit_paths.begin(), explicit_paths.end(), absolutePath("dir/lib path")) == explicit_paths.end(),
                "Explicit CLI import directories must replace manifest directories");
    }
    const auto flag_value = configurationArguments({"discc", "-o", "-Ioverride"}, Consumer::Compiler, &imports);
    require(hasFlag(flag_value, "--import-path", Consumer::Compiler), "Flag-looking output name changed import path precedence");
    const auto link_imports = configurationArguments({"discld"}, Consumer::Linker, &imports);
    require(!hasFlag(link_imports, "--import-path", Consumer::Linker), "Compiler import directories leaked to linker arguments");
    const auto optimization = Manifest::parse("[compiler]\noptimize = true", absolutePath("optimization.toml"));
    const auto optimized = configurationArguments({"discc", "-O0"}, Consumer::Compiler, &optimization);
    require(hasFlag(optimized, "-O1", Consumer::Compiler) && optimized.back() == "-O0",
            "Explicit O0 must override a manifest O1 default");
    require(!hasFlag(configurationArguments({"discld"}, Consumer::Linker, &optimization), "-O1", Consumer::Linker),
            "Optimization must not leak to linker/runtime configuration");
    const auto baseline = Manifest::parse("[compiler]\noptimize = false", absolutePath("baseline.toml"));
    require(hasFlag(configurationArguments({"discc"}, Consumer::Compiler, &baseline), "-O0", Consumer::Compiler),
            "False optimization selects the baseline");
    for (const auto level : {0, 1, 2}) {
        const auto level_manifest = Manifest::parse("[compiler]\noptimization_level = " + std::to_string(level), "test.toml");
        require(hasFlag(configurationArguments({"discc"}, Consumer::Compiler, &level_manifest), "-O" + std::to_string(level), Consumer::Compiler),
                "Integer optimization level was not forwarded");
        const auto overridden = configurationArguments({"discc", "-O0"}, Consumer::Compiler, &level_manifest);
        require(overridden.back() == "-O0", "CLI must win over manifest O2");
    }
    reject("[compiler]\noptimization_level = -1", "nonnegative");
    reject("[compiler]\noptimization_level = 3", "0, 1 or 2");
    reject("[compiler]\noptimization_level = true", "integer");
    reject("[compiler]\noptimization_level = \"2\"", "integer");
    reject("[compiler]\noptimization_level = \"z\"", "integer");
    reject("[compiler]\noptimization_level = \"size\"", "integer");
    const auto size_manifest = Manifest::parse("[compiler]\noptimization_level = 's'", "size.toml");
    const auto size_arguments = configurationArguments({"discc", "-O2"}, Consumer::Compiler, &size_manifest);
    require(hasFlag(size_arguments, "-Os", Consumer::Compiler) && size_arguments.back() == "-O2", "CLI did not override manifest Os");
    reject("[compiler]\noptimize = true\noptimization_level = 2", "not both");
}
} // namespace
int main() {
    try { parsing(); invalidInput(); precedence(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Project manifest checks passed\n";
    return 0;
}
