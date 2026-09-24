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

}  // namespace ide
