#pragma once

#include <memory>
#include <string>

namespace jadefx {
class Image;
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

// How wide and tall the icon that follows the pointer while instances are dragged
// is. Its center sits on the pointer.
inline constexpr double kDragIconSize = 24;

// A kDragIconSize view of an instance's icon image for that, with the id
// "instance-drag-icon". Empty without an image.
std::shared_ptr<jadefx::ImageView> drag_icon(std::shared_ptr<jadefx::Image> image);

}  // namespace ide
