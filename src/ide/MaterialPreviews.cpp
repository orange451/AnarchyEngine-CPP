#include "MaterialPreviews.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ide {
namespace {

float unit(double value) { return static_cast<float>(std::clamp(value, 0.0, 1.0)); }

// The Path of the Texture material's reference at index holds; empty for none.
std::string texture_path(const engine_core::DataModel& world, const engine_core::Material& material,
                         std::size_t index) {
    const engine_core::LuaSlot slot = material.reference(index);
    const auto* texture = slot.kind == engine_core::LuaSlot::Kind::Instance
                              ? dynamic_cast<const engine_core::Texture*>(world.instance(slot.id))
                              : nullptr;
    return texture != nullptr ? texture->path() : std::string();
}

}  // namespace

bool MaterialLook::operator==(const MaterialLook& other) const {
    return engine_core::same_color(color, other.color) && engine_core::same_color(emissive, other.emissive) &&
           metalness == other.metalness && roughness == other.roughness && reflectivity == other.reflectivity &&
           transparency == other.transparency && diffuse_texture == other.diffuse_texture &&
           normal_texture == other.normal_texture && roughness_texture == other.roughness_texture &&
           metalness_texture == other.metalness_texture && emissive_texture == other.emissive_texture;
}

std::optional<MaterialLook> material_look(const engine_core::DataModel& world, engine_core::InstanceId id) {
    const auto* material = dynamic_cast<const engine_core::Material*>(world.instance(id));
    if (material == nullptr) {
        return std::nullopt;
    }
    using engine_core::Material;
    MaterialLook look;
    look.color = material->color();
    look.emissive = material->emissive();
    look.metalness = unit(material->metalness());
    look.roughness = unit(material->roughness());
    look.reflectivity = unit(material->reflectivity());
    look.transparency = unit(material->transparency());
    look.diffuse_texture = texture_path(world, *material, Material::kDiffuseTextureReference);
    look.normal_texture = texture_path(world, *material, Material::kNormalTextureReference);
    look.roughness_texture = texture_path(world, *material, Material::kRoughnessTextureReference);
    look.metalness_texture = texture_path(world, *material, Material::kMetalnessTextureReference);
    look.emissive_texture = texture_path(world, *material, Material::kEmissiveTextureReference);
    return look;
}

runner::ViewPixels cut_ball(const runner::ViewPixels& render, double radius, int size) {
    runner::ViewPixels out;
    if (render.empty() || size <= 0 ||
        render.rgba.size() < static_cast<std::size_t>(render.width) * render.height * 4) {
        return out;
    }
    out.width = size;
    out.height = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const double center_x = render.width * 0.5;
    const double center_y = render.height * 0.5;
    const double reach = radius * render.width * 0.5;
    // Each new pixel covers a box of old ones, [x0, x1) by [y0, y1).
    for (int y = 0; y < size; ++y) {
        const int y0 = y * render.height / size;
        const int y1 = std::max(y0 + 1, (y + 1) * render.height / size);
        for (int x = 0; x < size; ++x) {
            const int x0 = x * render.width / size;
            const int x1 = std::max(x0 + 1, (x + 1) * render.width / size);
            // How much of each old pixel the ball covers, smoothed over a pixel at its edge.
            double covered = 0;
            double color[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = render.rgba.data() + (static_cast<std::size_t>(sy) * render.width + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    const double away = std::hypot(sx + 0.5 - center_x, sy + 0.5 - center_y);
                    const double cover = std::clamp(reach - away + 0.5, 0.0, 1.0);
                    covered += cover;
                    for (int channel = 0; channel < 3; ++channel) {
                        color[channel] += row[channel] * cover;
                    }
                }
            }
            if (covered <= 0) {
                continue;
            }
            std::uint8_t* to = out.rgba.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                to[channel] = static_cast<std::uint8_t>(std::lround(std::min(255.0, color[channel] / covered)));
            }
            const double count = static_cast<double>((x1 - x0) * (y1 - y0));
            to[3] = static_cast<std::uint8_t>(std::lround(255.0 * covered / count));
        }
    }
    return out;
}

MaterialPreviews::MaterialPreviews(Draw draw, int per_pass) : draw_(std::move(draw)), per_pass_(per_pass) {}

std::shared_ptr<jadefx::Image> MaterialPreviews::get(engine_core::InstanceId material, const MaterialLook& look) {
    Entry& entry = entries_[material];
    if (!entry.drawn || *entry.drawn != look) {
        if (entry.queued && entry.wanted != look) {
            entry.waits = 0;
        }
        entry.wanted = look;
        if (!entry.queued) {
            entry.queued = true;
            queue_.push_back(material);
        }
    }
    return entry.image;
}

bool MaterialPreviews::draw_pending() {
    int drawn = 0;
    while (drawn < per_pass_ && !queue_.empty()) {
        const engine_core::InstanceId material = queue_.front();
        const auto found = entries_.find(material);
        if (found == entries_.end() || !found->second.queued) {
            queue_.pop_front();
            continue;
        }
        Entry& entry = found->second;
        std::shared_ptr<jadefx::Image> image;
        if (draw_ && !draw_(entry.wanted, image) && ++entry.waits < kMaxWaits) {
            // Not ready: it stays first in line, and the rest would wait on the same renderer.
            break;
        }
        queue_.pop_front();
        entry.queued = false;
        entry.waits = 0;
        entry.image = std::move(image);
        entry.drawn = entry.wanted;
        ++drawn;
    }
    return drawn > 0;
}

void MaterialPreviews::retain(const std::vector<engine_core::InstanceId>& in_use) {
    auto kept = [&in_use](engine_core::InstanceId material) {
        return std::find(in_use.begin(), in_use.end(), material) != in_use.end();
    };
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        entry = kept(entry->first) ? std::next(entry) : entries_.erase(entry);
    }
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&kept](engine_core::InstanceId material) {
                     return !kept(material);
                 }),
                 queue_.end());
}

}  // namespace ide
