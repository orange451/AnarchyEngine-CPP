#pragma once

#include <filesystem>
#include <string>

namespace ide {

// Where a place that has never been saved keeps what it adds to resources,
// such as imported textures and models and a Mesh's shapes, until its first
// Save moves them into the project.

// A new scratch resources folder: AnarchyEngine/untitled-<stamp>/resources
// under the system's temporary folder, which the system clears in time if the
// studio never does. Not created here; whatever writes there first makes it.
// Empty when the system has no temporary folder.
std::filesystem::path new_scratch_resources();

// Whether path has the shape new_scratch_resources gives, so a delete never
// reaches anything else.
bool is_scratch_resources(const std::filesystem::path& path);

// Moves every file under scratch into resources, keeping the folders between.
// A file resources already has is kept, and the scratch one dropped. Then
// deletes scratch's untitled folder. False, with error naming the first file
// that could not move, and with scratch left in place.
bool move_scratch_resources(const std::filesystem::path& scratch, const std::filesystem::path& resources,
                            std::string& error);

// Deletes scratch's untitled folder and everything in it. Does nothing to a
// path that is not a scratch resources folder.
void remove_scratch_resources(const std::filesystem::path& scratch);

}  // namespace ide
