#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// Width of one line of text, in the units max_width uses.
using MeasureText = std::function<double(const std::string&)>;

// Breaks text into lines no wider than max_width, at spaces. A word wider than
// max_width on its own is split between code points. Newlines in the text
// start a new line, and runs of spaces between words become one.
std::vector<std::string> WrapText(std::string_view text, double max_width, const MeasureText& measure);

}  // namespace ide
