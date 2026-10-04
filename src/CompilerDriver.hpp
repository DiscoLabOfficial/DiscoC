#pragma once
#include <string>
#include <vector>

// Owns command arguments for the entire synchronous compilation. No AST/IR
// borrows escape this invocation; project builds reuse exactly this pipeline.
class ModuleLoader;
// Optional build-owned graph is borrowed only for this invocation. Its AST for
// the selected implementation is transferred once; no references escape.
int runCompiler(std::vector<std::string> arguments, ModuleLoader* modules = nullptr);
