#pragma once
#include "AST.hpp"

// Returns an owned public declaration, with function bodies/storage definitions
// removed. Internal declarations are never exported; tokens retain their origin.
std::unique_ptr<Stmt> moduleInterface(const Stmt& declaration, bool interface_file);
