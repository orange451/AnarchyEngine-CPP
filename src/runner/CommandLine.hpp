#pragma once

#include <string>
#include <vector>

namespace runner {

// The program's arguments in UTF-8, its name first, as AnarchyPlayer and
// AnarchyServer read them. On Windows argv is in the ANSI code page, which
// mangles any name outside it, such as a user folder with an accent, so they
// come from the wide command line instead; elsewhere argv is already UTF-8.
std::vector<std::string> utf8_arguments(int argc, char** argv);

}  // namespace runner
