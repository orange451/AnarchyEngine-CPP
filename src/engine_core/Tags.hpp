#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace engine_core {

// An instance's tags: names it carries, which scripts add with AddTag and
// CollectionService finds. Ordered, so GetTags and the saved JSON are sorted.
// DataModel.hpp names it too; the two must stay the same type.
using TagSet = std::set<std::string, std::less<>>;

// The longest tag.
inline constexpr std::size_t kMaxTag = 100;

// Why tag cannot be a tag, or nothing when it can: 1 to 100 characters, none
// of them a control character.
std::optional<std::string> tag_error(std::string_view tag);

// tags as the hidden Tags property holds them: a compact JSON array of
// strings, sorted. Empty for no tags.
std::string tags_to_json(const TagSet& tags);

// text read back into out, which it replaces. Empty text and null are no
// tags. An entry that is not a string tag_error accepts is left out, and named
// in error, one per line; the rest load. False, with out empty, only when text
// is not a JSON array.
bool tags_from_json(std::string_view text, TagSet& out, std::string& error);

}  // namespace engine_core
