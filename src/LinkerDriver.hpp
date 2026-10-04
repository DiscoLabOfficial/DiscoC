#pragma once
#include <string>
#include <vector>

// Public CLI adapter to the canonical linker, reused in-process by project
// builds. Arguments include the program name; failures return nonzero.
int runLinker(std::vector<std::string> arguments);
