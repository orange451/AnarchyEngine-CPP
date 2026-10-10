#pragma once

#include <string>
#include <vector>

namespace ide {

struct SystemFont {
    std::string family;
    std::string path;
};

const std::vector<SystemFont>& system_fonts();

void set_editor_font_choice(const std::string& family);

std::string editor_font_wanted();

std::string editor_font_family();

}  // namespace ide
