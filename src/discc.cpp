#include "CompilerDriver.hpp"
#include "ProjectBuild.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

int main(int argc, char* argv[]) {
    try {
        if (argc > 1024) throw std::runtime_error("Command-line argument count exceeds 1024.");
        std::vector<std::string> arguments;
        for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
        if (argc > 1 && (arguments[1] == "build" || arguments[1] == "--project"))
            return buildProject(arguments);
        return runCompiler(std::move(arguments));
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'; return 1;
    }
}
