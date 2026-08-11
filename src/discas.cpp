#include "Assembler.hpp"
#include "CompilerError.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Failed to open assembly file: " + path);
    std::stringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: discas <file.s> -o <file.o>\n";
        return 1;
    }

    std::string input_path;
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "Error: -o requires a filename.\n";
                return 1;
            }
            output_path = argv[++i];
        } else if (!input_path.empty()) {
            std::cerr << "Error: only one input assembly file is supported.\n";
            return 1;
        } else {
            input_path = argument;
        }
    }
    if (input_path.empty()) {
        std::cerr << "Error: no input assembly file specified.\n";
        return 1;
    }
    if (output_path.empty()) {
        const auto dot = input_path.find_last_of('.');
        output_path = (dot == std::string::npos ? input_path : input_path.substr(0, dot)) + ".o";
    }

    try {
        Assembler assembler;
        ObjectFile object = assembler.assemble(readFile(input_path));
        object.write(output_path);
        std::cout << "Assembled: " << input_path << " -> " << output_path << "\n";
    } catch (const CompilerError& error) {
        std::cerr << input_path << ":" << error.getLine() << ":" << error.getCol()
                  << ": error: " << error.getMessage() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Assembler error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
