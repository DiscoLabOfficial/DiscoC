#pragma once
#include <string>
#include <vector>

// Owns command arguments for the entire synchronous compilation. No AST/IR
// borrows escape this invocation; project builds reuse exactly this pipeline.
int runCompiler(std::vector<std::string> arguments);
