#include "LinkerDriver.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

int main(int argc, char* argv[]) {
    try {
        if (argc > 1024) throw std::runtime_error("Command-line argument count exceeds 1024.");
        std::vector<std::string> arguments;
        for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
        return runLinker(std::move(arguments));
    } catch (const std::exception& error) {
        std::cerr << "Linker Error: " << error.what() << '\n'; return 1;
    }
}
