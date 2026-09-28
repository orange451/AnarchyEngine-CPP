#pragma once

#include <memory>
#include <string>

namespace jadefx {
class ImageView;
}

namespace ide {

// One image view for this instance class. Rows share the decoded bitmap.
// Empty when that class has no icon file.
std::shared_ptr<jadefx::ImageView> icon_view(const std::string& class_name);

// An icon file under resources/icons, such as "Plus.png".
// Empty when the file is missing. The decoded bitmap is shared.
std::shared_ptr<jadefx::ImageView> icon_file(const std::string& filename);

// A 16px copy of that file that does not take clicks, for a menu row or a tab.
// Empty when the file is missing.
std::shared_ptr<jadefx::ImageView> icon_graphic(const std::string& filename);

}  // namespace ide
