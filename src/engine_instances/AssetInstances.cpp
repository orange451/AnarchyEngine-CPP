#include "AssetInstances.hpp"
#include "AssetLoads.hpp"

#include "AudioWorld.hpp"
#include "Contract.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Skeleton.hpp"
#include "amesh.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace engine_core {

std::optional<std::string> resource_path_error(std::string_view path) {
    const std::string refused = "Path must be relative to the resources folder";
    if (path.empty()) {
        return std::nullopt;
    }
    if (path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) {
        return refused;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (path.substr(start, end - start) == "..") {
            return refused;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> write_resource_file(const std::filesystem::path& root, const std::string& path,
                                               const std::vector<std::byte>& bytes) {
    const std::filesystem::path file = root / std::filesystem::u8path(path);
    // Written beside the file and renamed over it, so a reader never sees half a file.
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::filesystem::path partial = file;
    partial += ".partial";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    // Closed here, not by the destructor, so a failed flush of the last
    // bytes (a full disk) is seen before the rename puts them over a good file.
    out.close();
    if (out.fail()) {
        std::filesystem::remove(partial, error);
        return "Could not write " + path;
    }
    std::filesystem::rename(partial, file, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return "Could not write " + path;
    }
    return std::nullopt;
}

std::optional<std::string> FileAsset::set_path(std::string path) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (std::optional<std::string> error = resource_path_error(path)) {
        return error;
    }
    if (path == path_) {
        return std::nullopt;
    }
    LuaSlot before;
    before.kind = LuaSlot::Kind::String;
    before.text = path_;
    path_ = std::move(path);
    LuaSlot after;
    after.kind = LuaSlot::Kind::String;
    after.text = path_;
    note_property_change("Path", before, after);
    return std::nullopt;
}

const char* Texture::class_name() const { return "Texture"; }

void Texture::set_flip_y(bool flip_y) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (flip_y == flip_y_) {
        return;
    }
    flip_y_ = flip_y;
    LuaSlot before;
    before.kind = LuaSlot::Kind::Bool;
    before.flag = !flip_y;
    LuaSlot after;
    after.kind = LuaSlot::Kind::Bool;
    after.flag = flip_y;
    note_property_change("FlipY", before, after);
}

namespace {

LuaSlot texture_streaming_slot(TextureStreaming value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &texture_streaming_enum();
    slot.number = static_cast<int>(value);
    return slot;
}

}  // namespace

std::optional<std::string> Texture::set_streaming(int value) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (enum_item_name(texture_streaming_enum(), value) == nullptr) {
        return std::string("Streaming must be an Enum.TextureStreaming");
    }
    const TextureStreaming next = static_cast<TextureStreaming>(value);
    if (next == streaming_) {
        return std::nullopt;
    }
    const TextureStreaming previous = streaming_;
    streaming_ = next;
    note_property_change("Streaming", texture_streaming_slot(previous), texture_streaming_slot(next));
    return std::nullopt;
}
const char* Mesh::class_name() const { return "Mesh"; }

bool Mesh::loaded(const std::filesystem::path& root) const { return asset_loaded(AssetKind::Mesh, root, path()); }

bool Texture::loaded(const std::filesystem::path& root) const {
    return asset_loaded(AssetKind::Texture, root, path());
}

std::optional<std::string> Mesh::read_file(const std::filesystem::path& root, const std::string& path,
                                           anarchy::amesh::Data& out, bool allow_lods) const {
    out = anarchy::amesh::Data{};
    if (root.empty() || path.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path file = root / std::filesystem::u8path(path);
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    std::vector<std::byte> bytes(error ? 0 : static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (error || size > anarchy::amesh::kMaxFileSize ||
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return "Could not read " + path;
    }
    try {
        out = anarchy::amesh::read(bytes);
    } catch (const std::exception& failure) {
        return path + " is not an AMESH file, so it is left as it is: " + failure.what();
    }
    // New triangles would fall past the last LOD's range.
    if (!allow_lods && out.lods.size() >= 2) {
        return path + " has LODs, and shapes are added only to a mesh without them";
    }
    return std::nullopt;
}

std::optional<std::string> Mesh::edit_geometry(const std::function<void(anarchy::amesh::Data&)>& edit) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (simulation_running()) {
        // Built on this session's copy, or on the file the first time.
        anarchy::amesh::Data data;
        if (const SessionGeometry current = session_geometry(); current.data) {
            data = *current.data;
        } else if (std::optional<std::string> error = read_file(resources_root(), path(), data)) {
            return error;
        }
        edit(data);
        data.lods.clear();
        data.pieces.clear();
        if (data.vertices.size() > anarchy::amesh::kMaxVertices || data.indices.size() / 3 > anarchy::amesh::kMaxTriangles) {
            return std::string("The shapes are more than one mesh can hold");
        }
        // Unique across every Mesh, so a renderer can tell any two copies apart.
        static std::atomic<std::uint64_t> next_revision{1};
        session_.data = std::make_shared<const anarchy::amesh::Data>(std::move(data));
        session_.revision = next_revision.fetch_add(1);
        session_generation_ = world_generation();
        return std::nullopt;
    }

    const std::filesystem::path root = resources_root();
    if (root.empty()) {
        return std::string("Open or save a project first: a Mesh's shapes are written to its resources folder");
    }
    // The GUID keeps two Meshes with one Name from sharing a file.
    const std::string path =
        !this->path().empty() ? this->path() : "meshes/" + sanitize_file_name(name(id())) + "." + guid(id()) + ".amesh";
    anarchy::amesh::Data data;
    if (std::optional<std::string> error = read_file(root, path, data)) {
        return error;
    }
    edit(data);
    // One LOD over every triangle, whatever the edit added.
    data.lods.clear();
    data.pieces.clear();
    if (std::optional<std::string> error = write_file(root, path, data)) {
        return error;
    }
    if (this->path() != path) {
        return set_path(path);
    }
    return std::nullopt;
}

std::optional<std::string> Mesh::write_file(const std::filesystem::path& root, const std::string& path,
                                            const anarchy::amesh::Data& data) {
    std::vector<std::byte> bytes;
    try {
        bytes = anarchy::amesh::write(data);
    } catch (const std::exception& failure) {
        return std::string("The shapes do not fit in an AMESH file: ") + failure.what();
    }
    return write_resource_file(root, path, bytes);
}

Mesh::SessionGeometry Mesh::session_geometry() const {
    if (session_.data == nullptr || session_generation_ != world_generation() || !simulation_running()) {
        return SessionGeometry{};
    }
    return session_;
}

std::optional<std::string> Mesh::vertex_positions(std::vector<Vec3>& out, std::vector<std::uint32_t>* triangles) const {
    out.clear();
    if (triangles != nullptr) {
        triangles->clear();
    }
    anarchy::amesh::Data file;
    const anarchy::amesh::Data* data = nullptr;
    if (const SessionGeometry current = session_geometry(); current.data) {
        data = current.data.get();
    } else {
        if (path().empty()) {
            return std::string("the Mesh has no Path");
        }
        // A file with LODs still has its points.
        const std::optional<std::string> error = read_file(resources_root(), path(), file);
        if (error && file.vertices.empty()) {
            return error;
        }
        data = &file;
    }
    out.reserve(data->vertices.size());
    for (const anarchy::amesh::Vertex& vertex : data->vertices) {
        out.push_back(Vec3{vertex.p[0], vertex.p[1], vertex.p[2]});
    }
    if (out.empty()) {
        return path() + " has no vertices";
    }
    if (triangles != nullptr) {
        // The finest LOD only; the rest are the same surface again, coarser.
        std::size_t begin = 0;
        std::size_t count = data->indices.size() / 3;
        if (!data->lods.empty()) {
            begin = std::min<std::size_t>(data->lods.front().tri_begin, count);
            count = std::min<std::size_t>(data->lods.front().tri_count, count - begin);
        }
        triangles->reserve(count * 3);
        for (std::size_t index = begin * 3; index < (begin + count) * 3; ++index) {
            if (data->indices[index] >= out.size()) {
                triangles->clear();
                return path() + " has a triangle past its last vertex";
            }
            triangles->push_back(data->indices[index]);
        }
        if (triangles->empty()) {
            return path() + " has no triangles";
        }
    }
    return std::nullopt;
}

bool Mesh::bounds(Vec3& low, Vec3& high) const {
    const SessionGeometry session = session_geometry();
    std::filesystem::path file;
    std::filesystem::file_time_type stamp{};
    if (session.data == nullptr) {
        const std::filesystem::path root = resources_root();
        if (!root.empty() && !path().empty()) {
            file = root / std::filesystem::u8path(path());
            std::error_code error;
            stamp = std::filesystem::last_write_time(file, error);
            if (error) {
                stamp = {};
            }
        }
    }
    std::lock_guard<std::mutex> lock(bounds_mutex_);
    if (!bounds_measured_ || bounds_revision_ != session.revision || bounds_file_ != file || bounds_stamp_ != stamp) {
        bounds_measured_ = true;
        bounds_revision_ = session.revision;
        bounds_file_ = file;
        bounds_stamp_ = stamp;
        anarchy::amesh::Data read;
        const anarchy::amesh::Data* data = session.data.get();
        if (data == nullptr && !file.empty()) {
            // A file with LODs still has its points, as vertex_positions reads them.
            read_file(resources_root(), path(), read);
            data = &read;
        }
        bounds_found_ = data != nullptr && !data->vertices.empty();
        if (bounds_found_) {
            const float* first = data->vertices.front().p;
            bounds_low_ = Vec3{first[0], first[1], first[2]};
            bounds_high_ = bounds_low_;
            for (const anarchy::amesh::Vertex& vertex : data->vertices) {
                const float* p = vertex.p;
                bounds_low_ = {std::min(bounds_low_.x, p[0]), std::min(bounds_low_.y, p[1]),
                               std::min(bounds_low_.z, p[2])};
                bounds_high_ = {std::max(bounds_high_.x, p[0]), std::max(bounds_high_.y, p[1]),
                                std::max(bounds_high_.z, p[2])};
            }
        }
    }
    if (!bounds_found_) {
        return false;
    }
    low = bounds_low_;
    high = bounds_high_;
    return true;
}

namespace {

Vec3 middle(Vec3 low, Vec3 high) {
    return Vec3{(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
}

}  // namespace

Vec3 Mesh::origin_offset() const {
    Vec3 low{};
    Vec3 high{};
    return bounds(low, high) ? middle(low, high) : Vec3{};
}

std::string Mesh::file_stamp() const {
    const std::filesystem::path root = resources_root();
    if (root.empty() || path().empty()) {
        return {};
    }
    const std::filesystem::path file = root / std::filesystem::u8path(path());
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
    if (error) {
        return {};
    }
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error) {
        return {};
    }
    // Microseconds: MSVC's file clock counts from 1601, past long long in
    // nanoseconds, and libc++'s counts in __int128, which to_string does not take.
    const long long microseconds = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count());
    return path() + "|" + std::to_string(microseconds) + "|" + std::to_string(size);
}

bool Mesh::file_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece>& out) const {
    out.clear();
    if (session_geometry().data) {
        return false;
    }
    const std::string stamp = file_stamp();
    if (stamp.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    if (pieces_stamp_ != stamp) {
        pieces_stamp_ = stamp;
        anarchy::amesh::Data data;
        read_file(resources_root(), path(), data, true);
        pieces_recipe_ = data.piece_recipe;
        pieces_ = std::move(data.pieces);
    }
    if (pieces_.empty() || pieces_recipe_ != recipe) {
        return false;
    }
    out = pieces_;
    return true;
}

std::optional<std::string> Mesh::store_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece> pieces) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (simulation_running()) {
        return std::string("Pieces are stored only while the place is stopped");
    }
    if (file_stamp().empty()) {
        return std::string("The Mesh has no file");
    }
    const std::filesystem::path root = resources_root();
    anarchy::amesh::Data data;
    if (std::optional<std::string> error = read_file(root, path(), data, true)) {
        return error;
    }
    data.piece_recipe = recipe;
    data.pieces = std::move(pieces);
    return write_file(root, path(), data);
}

std::shared_ptr<const Skeleton> Mesh::skeleton() const {
    const SessionGeometry session = session_geometry();
    std::filesystem::path file;
    if (session.data == nullptr) {
        const std::filesystem::path root = resources_root();
        if (!root.empty() && !path().empty()) {
            file = root / std::filesystem::u8path(path());
        }
    }
    std::lock_guard<std::mutex> lock(skeleton_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const bool same_source = skeleton_read_ && skeleton_revision_ == session.revision && skeleton_file_ == file;
    if (same_source && (file.empty() || now - skeleton_checked_ < std::chrono::seconds(1))) {
        return skeleton_;
    }
    std::filesystem::file_time_type stamp{};
    if (!file.empty()) {
        std::error_code error;
        stamp = std::filesystem::last_write_time(file, error);
        if (error) {
            stamp = {};
        }
    }
    skeleton_checked_ = now;
    if (same_source && stamp == skeleton_stamp_) {
        return skeleton_;
    }
    skeleton_read_ = true;
    skeleton_revision_ = session.revision;
    skeleton_file_ = file;
    skeleton_stamp_ = stamp;
    if (session.data != nullptr) {
        skeleton_ = make_skeleton(session.data->bones);
    } else if (!file.empty()) {
        // A file with LODs still has its bones.
        anarchy::amesh::Data read;
        read_file(resources_root(), path(), read, true);
        skeleton_ = make_skeleton(read.bones);
    } else {
        skeleton_.reset();
    }
    return skeleton_;
}

void Mesh::on_reuse() {
    FileAsset::on_reuse();
    session_ = SessionGeometry{};
    session_generation_ = 0;
    std::lock_guard<std::mutex> lock(bounds_mutex_);
    bounds_measured_ = false;
    bounds_found_ = false;
    std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
    pieces_stamp_.clear();
    pieces_.clear();
    std::lock_guard<std::mutex> skeleton_lock(skeleton_mutex_);
    skeleton_read_ = false;
    skeleton_.reset();
}

const char* Sound::class_name() const { return "Sound"; }

double Sound::time_length(const std::filesystem::path& root) const {
    if (path().empty() || root.empty()) {
        return 0;
    }
    const std::filesystem::path file = root / std::filesystem::u8path(path());
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(length_mutex_);
    if (file != length_file_ || stamp != length_stamp_) {
        length_ = audio_file_seconds(file);
        length_file_ = file;
        length_stamp_ = stamp;
    }
    return length_;
}
const char* Material::class_name() const { return "Material"; }
const char* Model::class_name() const { return "Model"; }
const char* Prefab::class_name() const { return "Prefab"; }

void Prefab::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Edit, true});
    DataModel::context_actions(out);
}

bool Prefab::bounds(Vec3& low, Vec3& high) const {
    bool found = false;
    for (InstanceId child = first_child(id()); child != 0; child = next_sibling(child)) {
        const auto* model = dynamic_cast<const Model*>(instance(child));
        if (model == nullptr) {
            continue;
        }
        const LuaSlot slot = model->reference(Model::kMeshReference);
        const auto* mesh =
            slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Mesh*>(instance(slot.id)) : nullptr;
        Vec3 mesh_low{};
        Vec3 mesh_high{};
        if (mesh == nullptr || !mesh->bounds(mesh_low, mesh_high)) {
            continue;
        }
        if (!found) {
            low = mesh_low;
            high = mesh_high;
            found = true;
            continue;
        }
        low = {std::min(low.x, mesh_low.x), std::min(low.y, mesh_low.y), std::min(low.z, mesh_low.z)};
        high = {std::max(high.x, mesh_high.x), std::max(high.y, mesh_high.y), std::max(high.z, mesh_high.z)};
    }
    return found;
}

Vec3 Prefab::origin_offset() const {
    Vec3 low{};
    Vec3 high{};
    return bounds(low, high) ? middle(low, high) : Vec3{};
}

namespace {

constexpr ReferenceSpec kMaterialRefs[] = {
    {"DiffuseTexture", "Texture"},
    {"NormalTexture", "Texture"},
    {"RoughnessTexture", "Texture"},
    {"MetalnessTexture", "Texture"},
    {"EmissiveTexture", "Texture"},
    {"HeightTexture", "Texture"},
};

constexpr ReferenceSpec kModelRefs[] = {
    {"Mesh", "Mesh"},
    {"Material", "Material"},
};

static_assert(std::size(kMaterialRefs) <= ReferenceAsset::kMaxReferences);
static_assert(std::size(kModelRefs) <= ReferenceAsset::kMaxReferences);

}  // namespace

const ReferenceSpec* Material::reference_specs(std::size_t& count) const {
    count = std::size(kMaterialRefs);
    return kMaterialRefs;
}

const ReferenceSpec* Model::reference_specs(std::size_t& count) const {
    count = std::size(kModelRefs);
    return kModelRefs;
}

LuaSlot ReferenceAsset::reference(std::size_t index) const {
    std::size_t count = 0;
    const ReferenceSpec* specs = reference_specs(count);
    if (index >= count || index >= kMaxReferences) {
        return LuaSlot();
    }
    return instance_reference_slot(refs_[index], specs[index].klass);
}

std::optional<std::string> ReferenceAsset::set_reference(std::size_t index, const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    std::size_t count = 0;
    const ReferenceSpec* specs = reference_specs(count);
    if (index >= count || index >= kMaxReferences) {
        contract_fail("no such reference");
    }
    const ReferenceSpec& spec = specs[index];
    return set_instance_reference(spec.property, spec.klass, refs_[index], value);
}

void ReferenceAsset::on_reuse() {
    for (InstanceRef& ref : refs_) {
        ref.set_guid(std::string());
    }
}

namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot color_slot(ColorRgb color) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

}  // namespace

std::optional<std::string> Material::set_number(const char* property, double& slot, double value) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> Material::set_reflectivity(double value) {
    return set_number("Reflectivity", reflectivity_, value);
}

std::optional<std::string> Material::set_transparency(double value) {
    return set_number("Transparency", transparency_, value);
}

std::optional<std::string> Material::set_metalness(double value) {
    return set_number("Metalness", metalness_, value);
}

std::optional<std::string> Material::set_roughness(double value) {
    return set_number("Roughness", roughness_, value);
}

std::optional<std::string> Material::set_ranged_number(const char* property, double& slot, double value,
                                                       bool (*valid)(double), const char* message) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (!std::isfinite(value) || !valid(value)) {
        return std::string(message);
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> Material::set_texture_scale(double value) {
    return set_ranged_number("TextureScale", texture_scale_, value, [](double v) { return v > 0.0; },
                             "TextureScale must be greater than 0");
}

std::optional<std::string> Material::set_blend_sharpness(double value) {
    return set_ranged_number("BlendSharpness", blend_sharpness_, value,
                             [](double v) { return v >= 0.0 && v <= 1.0; }, "BlendSharpness must be from 0 to 1");
}

std::optional<std::string> Material::set_height_strength(double value) {
    return set_ranged_number("HeightStrength", height_strength_, value, [](double v) { return v >= 0.0; },
                             "HeightStrength must not be negative");
}

std::optional<std::string> Material::set_color3(const char* property, ColorRgb& slot, ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string(property) + " must be finite";
    }
    // A Color3 has no alpha.
    color.a = 1.f;
    if (same_color(slot, color)) {
        return std::nullopt;
    }
    const ColorRgb previous = slot;
    slot = color;
    note_property_change(property, color_slot(previous), color_slot(color));
    return std::nullopt;
}

std::optional<std::string> Material::set_color(ColorRgb color) { return set_color3("Color", color_, color); }

std::optional<std::string> Material::set_emissive(ColorRgb color) { return set_color3("Emissive", emissive_, color); }

void Material::on_reuse() {
    ReferenceAsset::on_reuse();
    reflectivity_ = kDefaultReflectivity;
    transparency_ = kDefaultTransparency;
    metalness_ = kDefaultMetalness;
    roughness_ = kDefaultRoughness;
    color_ = kDefaultColor;
    emissive_ = kDefaultEmissive;
    texture_scale_ = kDefaultTextureScale;
    blend_sharpness_ = kDefaultBlendSharpness;
    height_strength_ = kDefaultHeightStrength;
}

namespace {

bool read_path(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* asset = dynamic_cast<const FileAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = asset->path();
    return true;
}

bool read_flip_y(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* texture = dynamic_cast<const Texture*>(&object);
    if (texture == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Bool;
    out.flag = texture->flip_y();
    return true;
}

bool write_flip_y(DataModel&, DataModel& object, LuaSlot& in) {
    auto* texture = dynamic_cast<Texture*>(&object);
    if (texture == nullptr) {
        return false;
    }
    texture->set_flip_y(in.flag);
    return true;
}

bool read_streaming(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* texture = dynamic_cast<const Texture*>(&object);
    if (texture == nullptr) {
        return false;
    }
    out = texture_streaming_slot(texture->streaming());
    return true;
}

bool write_streaming(DataModel&, DataModel& object, LuaSlot& in) {
    auto* texture = dynamic_cast<Texture*>(&object);
    if (texture == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &texture_streaming_enum()) {
        in.error = "Streaming must be an Enum.TextureStreaming";
        return false;
    }
    std::optional<std::string> why = texture->set_streaming(static_cast<int>(in.number));
    if (why) {
        in.error = std::move(*why);
        return false;
    }
    return true;
}

// Loaded, of a Texture, Mesh, or Sound: read from what loaded it, never written or saved.
bool read_loaded(DataModel& game, DataModel& object, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Bool;
    if (const auto* texture = dynamic_cast<const Texture*>(&object)) {
        out.flag = texture->loaded(game.resources_root());
    } else if (const auto* mesh = dynamic_cast<const Mesh*>(&object)) {
        out.flag = mesh->loaded(game.resources_root());
    } else if (const auto* sound = dynamic_cast<const Sound*>(&object)) {
        out.flag = sound->loaded(game.resources_root());
    } else {
        return false;
    }
    return true;
}

bool read_time_length(DataModel& game,DataModel& object, LuaSlot& out) {
    const auto* sound = dynamic_cast<const Sound*>(&object);
    if (sound == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Number;
    out.number = sound->time_length(game.resources_root());
    return true;
}

// OriginOffset, of a Mesh or a Prefab.
bool read_origin_offset(DataModel&, DataModel& object, LuaSlot& out) {
    Vec3 offset{};
    if (const auto* mesh = dynamic_cast<const Mesh*>(&object)) {
        offset = mesh->origin_offset();
    } else if (const auto* prefab = dynamic_cast<const Prefab*>(&object)) {
        offset = prefab->origin_offset();
    } else {
        return false;
    }
    out.kind = LuaSlot::Kind::Vec3;
    out.vec = offset;
    return true;
}

bool write_path(DataModel&, DataModel& object, LuaSlot& in) {
    auto* asset = dynamic_cast<FileAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = asset->set_path(in.text)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <std::size_t Index>
bool read_reference(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* asset = dynamic_cast<const ReferenceAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    out = asset->reference(Index);
    return true;
}

template <std::size_t Index>
bool write_reference(DataModel&, DataModel& object, LuaSlot& in) {
    auto* asset = dynamic_cast<ReferenceAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = asset->set_reference(Index, in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Material* material_of(DataModel& object) { return dynamic_cast<Material*>(&object); }

template <double (Material::*Get)() const>
bool read_material_number(DataModel&, DataModel& object, LuaSlot& out) {
    const Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    out = number_slot((material->*Get)());
    return true;
}

template <std::optional<std::string> (Material::*Set)(double)>
bool write_material_number(DataModel&, DataModel& object, LuaSlot& in) {
    Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = (material->*Set)(in.number)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <ColorRgb (Material::*Get)() const>
bool read_material_color(DataModel&, DataModel& object, LuaSlot& out) {
    const Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    out = color_slot((material->*Get)());
    return true;
}

template <std::optional<std::string> (Material::*Set)(ColorRgb)>
bool write_material_color(DataModel&, DataModel& object, LuaSlot& in) {
    Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = (material->*Set)(in.color)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

std::string color_json(ColorRgb color) {
    const float channels[3] = {color.r, color.g, color.b};
    return write_json(json_floats(channels, 3));
}

ANARCHY_LUA_REGISTER(register_asset_instances_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string reflectivity = write_json(JsonValue::number(Material::kDefaultReflectivity));
    static const std::string transparency = write_json(JsonValue::number(Material::kDefaultTransparency));
    static const std::string metalness = write_json(JsonValue::number(Material::kDefaultMetalness));
    static const std::string roughness = write_json(JsonValue::number(Material::kDefaultRoughness));
    static const std::string color = color_json(Material::kDefaultColor);
    static const std::string emissive = color_json(Material::kDefaultEmissive);
    static const std::string texture_scale = write_json(JsonValue::number(Material::kDefaultTextureScale));
    static const std::string blend_sharpness = write_json(JsonValue::number(Material::kDefaultBlendSharpness));
    static const std::string height_strength = write_json(JsonValue::number(Material::kDefaultHeightStrength));
    const LuaField file_fields[] = {
        lua_saved_property("Path", "string", read_path, write_path, "\"\""),
    };
    register_lua_class("FileAsset", "Instance", nullptr, 0);
    const LuaField texture_fields[] = {
        file_fields[0],
        lua_saved_property("FlipY", "boolean", read_flip_y, write_flip_y, "false"),
        lua_saved_enum("Streaming", texture_streaming_enum(), read_streaming, write_streaming, "\"Automatic\""),
        lua_property("Loaded", "boolean", false, read_loaded, nullptr),
    };
    register_lua_class("Texture", "FileAsset", texture_fields, 4);
    // OriginOffset is measured from the geometry, never written or saved.
    const LuaField mesh_fields[] = {
        file_fields[0],
        lua_property("OriginOffset", "Vector3", false, read_origin_offset, nullptr),
        lua_property("Loaded", "boolean", false, read_loaded, nullptr),
    };
    register_lua_class("Mesh", "FileAsset", mesh_fields, 3);
    // TimeLength is read from the file, never written or saved.
    const LuaField sound_fields[] = {
        file_fields[0],
        lua_property("TimeLength", "number", false, read_time_length, nullptr),
        lua_property("Loaded", "boolean", false, read_loaded, nullptr),
    };
    register_lua_class("Sound", "FileAsset", sound_fields, 3);
    register_lua_class("ReferenceAsset", "Instance", nullptr, 0);
    const LuaField material_fields[] = {
        lua_group("Surface"),
        lua_saved_property("DiffuseTexture", "Texture?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("NormalTexture", "Texture?", read_reference<1>, write_reference<1>, "null"),
        lua_saved_property("RoughnessTexture", "Texture?", read_reference<2>, write_reference<2>, "null"),
        lua_saved_property("MetalnessTexture", "Texture?", read_reference<3>, write_reference<3>, "null"),
        lua_group("Modifier"),
        lua_saved_property("Color", "Color3", read_material_color<&Material::color>,
                           write_material_color<&Material::set_color>, color.c_str()),
        lua_slider(lua_saved_property("Transparency", "number", read_material_number<&Material::transparency>,
                                      write_material_number<&Material::set_transparency>, transparency.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Roughness", "number", read_material_number<&Material::roughness>,
                                      write_material_number<&Material::set_roughness>, roughness.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Metalness", "number", read_material_number<&Material::metalness>,
                                      write_material_number<&Material::set_metalness>, metalness.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Reflectivity", "number", read_material_number<&Material::reflectivity>,
                                      write_material_number<&Material::set_reflectivity>, reflectivity.c_str()),
                   0.0, 1.0),
        lua_saved_property("EmissiveTexture", "Texture?", read_reference<4>, write_reference<4>, "null"),
        lua_saved_property("Emissive", "Color3", read_material_color<&Material::emissive>,
                           write_material_color<&Material::set_emissive>, emissive.c_str()),
        lua_group("Terrain"),
        lua_slider(lua_saved_property("TextureScale", "number", read_material_number<&Material::texture_scale>,
                                      write_material_number<&Material::set_texture_scale>, texture_scale.c_str()),
                   Material::kMinTextureScaleSlider, 16.0),
        lua_slider(lua_saved_property("BlendSharpness", "number", read_material_number<&Material::blend_sharpness>,
                                      write_material_number<&Material::set_blend_sharpness>,
                                      blend_sharpness.c_str()),
                   0.0, 1.0),
        lua_saved_property("HeightTexture", "Texture?", read_reference<5>, write_reference<5>, "null"),
        lua_slider(lua_saved_property("HeightStrength", "number", read_material_number<&Material::height_strength>,
                                      write_material_number<&Material::set_height_strength>, height_strength.c_str()),
                   0.0, 1.0),
    };
    register_lua_class("Material", "ReferenceAsset", material_fields,
                       static_cast<int>(sizeof(material_fields) / sizeof(material_fields[0])));
    const LuaField model_fields[] = {
        lua_saved_property("Mesh", "Mesh?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("Material", "Material?", read_reference<1>, write_reference<1>, "null"),
    };
    register_lua_class("Model", "ReferenceAsset", model_fields, 2);
    const LuaField prefab_fields[] = {
        lua_property("OriginOffset", "Vector3", false, read_origin_offset, nullptr),
    };
    register_lua_class("Prefab", "Instance", prefab_fields, 1);
}

}  // namespace

}  // namespace engine_core
