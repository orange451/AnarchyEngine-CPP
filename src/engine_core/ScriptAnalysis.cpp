#include "ScriptAnalysis.hpp"

#include "AnalysisPool.hpp"
#include "AnalysisWorld.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"
#include "StackThread.hpp"
#include "TableSnapshot.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include <atomic>

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/Cancellation.h"
#include "Luau/Common.h"
#include "Luau/Config.h"
#include "Luau/Error.h"
#include "Luau/FileResolver.h"
#include "Luau/AstQuery.h"
#include "Luau/Autocomplete.h"
#include "Luau/Frontend.h"
#include "Luau/Linter.h"
#include "Luau/Module.h"
#include "Luau/ParseOptions.h"
#include "Luau/ConstraintSolver.h"
#include "Luau/Parser.h"
#include "Luau/ParseResult.h"
#include "Luau/Scope.h"
#include "Luau/Type.h"
#include "Luau/TypeArena.h"
#include "Luau/TypePack.h"
#include "Luau/TypeUtils.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine_core {
namespace {

constexpr std::chrono::milliseconds kDebounce{75};

using analysis::NodeSnap;
using analysis::TreeDiff;
using analysis::WorldSnap;
using analysis::capture_world;
using analysis::diff_worlds;
using analysis::same_tree;
using analysis::world_from_nodes;

// A script waiting for the place checker.
struct Pending {
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point ready_at{};
    // The capture count when it was queued. The place checker takes it only
    // once a tree captured after that is adopted, so its batch sees the change.
    std::uint64_t after = 0;
};

struct Finished {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    std::string name;
    std::string source;
    std::vector<Diagnostic> diagnostics;
    std::vector<InstanceId> requires;
    std::vector<InstanceId> reached;
    // The script left the place: pump() drops what it published instead.
    bool dropped = false;
};

const char* severity_name(Severity severity) {
    switch (severity) {
    case Severity::Error:
        return "Error";
    case Severity::Warning:
        return "Warning";
    case Severity::Information:
        return "Information";
    case Severity::Hint:
        return "Hint";
    }
    return "Error";
}

TextRange range_from(const Luau::Location& location) {
    TextRange range;
    if (location.begin.line != UINT_MAX) {
        range.start.line = location.begin.line;
        range.start.character = location.begin.column;
    }
    if (location.end.line != UINT_MAX) {
        range.end.line = location.end.line;
        range.end.character = location.end.column;
    } else {
        range.end = range.start;
    }
    return range;
}

}  // namespace

std::string one_line(std::string text, std::size_t max_bytes) {
    for (char& unit : text) {
        if (unit == '\n' || unit == '\r' || unit == '\t') {
            unit = ' ';
        }
    }
    if (max_bytes != std::string::npos && text.size() > max_bytes) {
        text.resize(fit_utf8(text, max_bytes >= 3 ? max_bytes - 3 : 0));
        text += "...";
    }
    return text;
}

namespace {

Diagnostic make_diagnostic(InstanceId script, TextRange range, Severity severity, std::string code, std::string message) {
    Diagnostic diagnostic;
    diagnostic.script = script;
    diagnostic.range = range;
    diagnostic.severity = severity;
    diagnostic.code = std::move(code);
    diagnostic.message = one_line(std::move(message));
    return diagnostic;
}

std::string string_literal(const Luau::AstArray<char>& value) {
    return std::string(value.data, value.size);
}

std::optional<InstanceId> resolve_expr(const WorldSnap& world, InstanceId self, Luau::AstExpr* expr) {
    if (expr == nullptr) {
        return std::nullopt;
    }
    if (auto* group = expr->as<Luau::AstExprGroup>()) {
        return resolve_expr(world, self, group->expr);
    }
    if (auto* global = expr->as<Luau::AstExprGlobal>()) {
        if (global->name == "script") {
            return self;
        }
        if (global->name == "game") {
            return world.root;
        }
        if (global->name == "workspace") {
            return world.workspace();
        }
        return std::nullopt;
    }
    if (auto* call = expr->as<Luau::AstExprCall>()) {
        auto* index = call->func->as<Luau::AstExprIndexName>();
        if (index == nullptr || !lua_method_resolves_child(index->index.value) || call->args.size < 1) {
            return std::nullopt;
        }
        auto* literal = call->args.data[0]->as<Luau::AstExprConstantString>();
        if (literal == nullptr || !literal->isQuoted()) {
            return std::nullopt;
        }
        const std::optional<InstanceId> base = resolve_expr(world, self, index->expr);
        if (!base) {
            return std::nullopt;
        }
        return world.child_named(*base, string_literal(literal->value));
    }
    if (auto* index = expr->as<Luau::AstExprIndexName>()) {
        const std::optional<InstanceId> base = resolve_expr(world, self, index->expr);
        if (!base) {
            return std::nullopt;
        }
        if (index->index == "Parent") {
            const NodeSnap* node = world.find(*base);
            if (node == nullptr || node->parent == DataModel::kNoParent) {
                return std::nullopt;
            }
            return node->parent;
        }
        return world.child_named(*base, index->index.value);
    }
    if (auto* literal = expr->as<Luau::AstExprConstantString>()) {
        if (!literal->isQuoted()) {
            return std::nullopt;
        }
        std::optional<InstanceId> prefer;
        if (const NodeSnap* node = world.find(self)) {
            if (node->parent != DataModel::kNoParent) {
                prefer = node->parent;
            }
        }
        return world.module_named(string_literal(literal->value), prefer);
    }
    return std::nullopt;
}

struct RequireWalk : Luau::AstVisitor {
    const WorldSnap* world = nullptr;
    InstanceId self = 0;
    std::vector<InstanceId> required;
    // Each `require("Name")`: it looks the name up across the whole place.
    std::vector<std::string> by_name;

    bool visit(Luau::AstExprCall* call) override {
        auto* global = call->func != nullptr ? call->func->as<Luau::AstExprGlobal>() : nullptr;
        if (global != nullptr && global->name == "require" && call->args.size > 0 && world != nullptr) {
            Luau::AstExpr* arg = call->args.data[0];
            while (auto* group = arg->as<Luau::AstExprGroup>()) {
                arg = group->expr;
            }
            if (auto* literal = arg->as<Luau::AstExprConstantString>()) {
                std::string name = string_literal(literal->value);
                if (literal->isQuoted() && std::find(by_name.begin(), by_name.end(), name) == by_name.end()) {
                    by_name.push_back(std::move(name));
                }
            }
            const std::optional<InstanceId> target = resolve_expr(*world, self, call->args.data[0]);
            if (target && *target != self) {
                const NodeSnap* node = world->find(*target);
                if (node != nullptr && node->module &&
                    std::find(required.begin(), required.end(), *target) == required.end()) {
                    required.push_back(*target);
                }
            }
        }
        return true;
    }
};

// The modules the script requires, and in `by_name` the names it requires by name.
std::vector<InstanceId> find_requires(const WorldSnap& world, InstanceId self, Luau::AstStat* root,
                                      std::vector<std::string>* by_name = nullptr) {
    if (root == nullptr) {
        return {};
    }
    RequireWalk walk;
    walk.world = &world;
    walk.self = self;
    root->visit(&walk);
    if (by_name != nullptr) {
        *by_name = std::move(walk.by_name);
    }
    return walk.required;
}

// The instance an extern type in PlaceTypes stands for. Cloning a type keeps
// this pointer, so a module's exported copy still names the same instance.
struct InstanceTag final : Luau::ClassUserData {
    InstanceId id = 0;

    explicit InstanceTag(InstanceId id) : id(id) {}
};

// One extern type per instance in a snapshot, so a script is checked against
// the place it runs in. Each extends its instance's class. Its children are
// read-only fields named like them, the first of a name in sibling order, as
// `game.Door` reads at run time; a class member of the same name wins. Parent
// reads as the parent's type and still takes any Instance on write. `game` and
// `script` are bound to these in every module the frontend checks.
struct PlaceTypes {
    std::shared_ptr<const WorldSnap> world;
    Luau::TypeArena arena;
    std::unordered_map<InstanceId, Luau::TypeId> types;

    std::optional<Luau::TypeId> find(InstanceId id) const {
        const auto found = types.find(id);
        if (found == types.end()) {
            return std::nullopt;
        }
        return found->second;
    }
};

std::optional<Luau::TypeId> class_type(const Luau::Scope& scope, const std::string& name) {
    const std::optional<Luau::TypeFun> type_fun = scope.lookupType(name);
    if (!type_fun) {
        return std::nullopt;
    }
    const Luau::TypeId type = Luau::follow(type_fun->type);
    if (Luau::get<Luau::ExternType>(type) == nullptr) {
        return std::nullopt;
    }
    return type;
}

// A new extern type for the instance, extending its class, tagged with its id.
void add_instance_type(PlaceTypes& place, const Luau::Scope& globals, const NodeSnap& node) {
    std::optional<Luau::TypeId> base = class_type(globals, node.class_name);
    if (!base) {
        base = class_type(globals, "DataModel");
    }
    if (!base) {
        return;
    }
    const Luau::ExternType* base_class = Luau::get<Luau::ExternType>(*base);
    place.types[node.id] = place.arena.addType(Luau::ExternType{base_class->name, {}, *base, std::nullopt, {},
                                                                std::make_shared<InstanceTag>(node.id), "@anarchy",
                                                                std::nullopt});
}

// The instance's children and Parent as its type's fields, from place.world.
// The fields are rewritten, never the type: cached modules hold it.
void fill_instance_props(PlaceTypes& place, InstanceId id) {
    const NodeSnap* node = place.world->find(id);
    const std::optional<Luau::TypeId> own = place.find(id);
    if (node == nullptr || !own) {
        return;
    }
    Luau::ExternType* type = Luau::getMutable<Luau::ExternType>(*own);
    const Luau::ExternType* base_class = Luau::get<Luau::ExternType>(*type->parent);
    type->props.clear();
    for (InstanceId child : node->children) {
        const NodeSnap* child_node = place.world->find(child);
        const std::optional<Luau::TypeId> child_type = place.find(child);
        if (child_node == nullptr || !child_type) {
            continue;
        }
        const std::string& name = child_node->name;
        if (name.empty() || type->props.count(name) != 0 || Luau::lookupExternTypeProp(base_class, name) != nullptr) {
            continue;
        }
        type->props[name] = Luau::Property::readonly(*child_type);
    }
    const std::optional<Luau::TypeId> parent =
        node->parent != DataModel::kNoParent ? place.find(node->parent) : std::nullopt;
    if (!parent) {
        return;
    }
    const Luau::Property* declared = Luau::lookupExternTypeProp(base_class, "Parent");
    if (declared != nullptr && declared->writeTy) {
        type->props["Parent"] = Luau::Property::rw(*parent, *declared->writeTy);
    } else {
        type->props["Parent"] = Luau::Property::readonly(*parent);
    }
}

std::unique_ptr<PlaceTypes> build_place_types(const Luau::Scope& globals, std::shared_ptr<const WorldSnap> world) {
    auto place = std::make_unique<PlaceTypes>();
    place->world = std::move(world);
    for (const NodeSnap& node : place->world->nodes) {
        add_instance_type(*place, globals, node);
    }
    for (const NodeSnap& node : place->world->nodes) {
        fill_instance_props(*place, node.id);
    }
    return place;
}

// Brings the place's types to `world` between batches. New instances get
// types; instances the diff names get their fields rewritten; a destroyed one
// keeps its type with no fields, since cached modules may still hold it. An
// instance back in the place, after an undo or a detached folder reattached,
// takes up the type it had.
void update_place_types(PlaceTypes& place, const Luau::Scope& globals, std::shared_ptr<const WorldSnap> world,
                        const TreeDiff& diff) {
    const std::shared_ptr<const WorldSnap> before = std::move(place.world);
    place.world = std::move(world);
    std::vector<InstanceId> refill(diff.parents.begin(), diff.parents.end());
    refill.insert(refill.end(), diff.moved.begin(), diff.moved.end());
    for (const NodeSnap& node : place.world->nodes) {
        if (place.types.count(node.id) == 0) {
            add_instance_type(place, globals, node);
            refill.push_back(node.id);
        } else if (before == nullptr || before->find(node.id) == nullptr) {
            refill.push_back(node.id);
        }
    }
    for (InstanceId id : refill) {
        if (place.world->find(id) != nullptr) {
            fill_instance_props(place, id);
        } else if (const std::optional<Luau::TypeId> type = place.find(id)) {
            Luau::getMutable<Luau::ExternType>(*type)->props.clear();
        }
    }
}

std::optional<InstanceId> tagged_instance(Luau::TypeId type) {
    const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(Luau::follow(type));
    if (extern_type == nullptr) {
        return std::nullopt;
    }
    const auto* tag = dynamic_cast<const InstanceTag*>(extern_type->userData.get());
    if (tag == nullptr) {
        return std::nullopt;
    }
    return tag->id;
}

// The registered class an extern type is, walking up its bases. Empty for
// anything the registry does not know.
std::string registered_class(const Luau::ExternType* extern_type) {
    for (int depth = 0; extern_type != nullptr && depth < 64; ++depth) {
        if (extern_type->name == "vector") {
            return "Vector3";
        }
        if (lua_class_known(extern_type->name.c_str())) {
            return extern_type->name;
        }
        if (!extern_type->parent) {
            break;
        }
        extern_type = Luau::get<Luau::ExternType>(Luau::follow(*extern_type->parent));
    }
    return {};
}

// The registered class a type is, walking a per-instance type up to it.
std::string registered_class(Luau::TypeId type) {
    return registered_class(Luau::get<Luau::ExternType>(Luau::follow(type)));
}

bool missing_render_member(const Luau::TypeError& error) {
    if (const Luau::UnknownProperty* property = Luau::get<Luau::UnknownProperty>(error)) {
        return property->key == "PreRender";
    }
    if (const Luau::UnknownPropButFoundLikeProp* property = Luau::get<Luau::UnknownPropButFoundLikeProp>(error)) {
        return property->key == "PreRender";
    }
    return false;
}

struct WorkerEnv;

// FindFirstChild's declared return is Instance?. When the name is a string
// literal and that child is in the place, the result is that child, not
// optional: the script is checked against the tree in the explorer, the same
// way `game.Door` is. A name that is not there keeps the declared Instance?.
// WaitForChild and any other lookup the API flags resolves_child work the same.
struct FindChildMagic final : Luau::MagicFunction {
    WorkerEnv* env = nullptr;

    explicit FindChildMagic(WorkerEnv* env) : env(env) {}

    std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(Luau::TypeChecker&,
                                                                          const std::shared_ptr<Luau::Scope>&,
                                                                          const Luau::AstExprCall&,
                                                                          Luau::WithPredicate<Luau::TypePackId>) override {
        return std::nullopt;
    }

    bool infer(const Luau::MagicFunctionCallContext& context) override;
    bool infer_child(const Luau::MagicFunctionCallContext& context);
};

struct NarrowMagic final : Luau::MagicFunction {
    enum class Kind { Service, Creatable };

    Kind kind = Kind::Service;

    explicit NarrowMagic(Kind kind) : kind(kind) {}

    std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(Luau::TypeChecker&,
                                                                          const std::shared_ptr<Luau::Scope>&,
                                                                          const Luau::AstExprCall&,
                                                                          Luau::WithPredicate<Luau::TypePackId>) override {
        return std::nullopt;
    }

    bool infer(const Luau::MagicFunctionCallContext& context) override;
    bool infer_narrow(const Luau::MagicFunctionCallContext& context);
};

void stamp_vector(Luau::Frontend& frontend);
void attach_api(WorkerEnv& env);

constexpr std::string_view kModulePrefix = "script-";

std::string module_name_of(InstanceId id) {
    return std::string(kModulePrefix) + std::to_string(id);
}

// What the report calls a script: its Name, or its class when it has none.
const std::string& shown_name(const NodeSnap& node) { return node.name.empty() ? node.class_name : node.name; }

std::optional<InstanceId> instance_of_module(std::string_view name) {
    if (name.substr(0, kModulePrefix.size()) != kModulePrefix) {
        return std::nullopt;
    }
    InstanceId id = 0;
    const std::string_view digits = name.substr(kModulePrefix.size());
    if (digits.empty()) {
        return std::nullopt;
    }
    for (char digit : digits) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        id = id * 10 + static_cast<InstanceId>(digit - '0');
    }
    return id;
}

// Every instance is a module name, "script-<id>", so a require path can walk
// through folders. Only a ModuleScript has source a require can load.
struct SourceFileResolver : Luau::FileResolver {
    const std::string* module_name = nullptr;
    const std::string* source = nullptr;
    const std::string* display = nullptr;
    Luau::SourceCode::Type type = Luau::SourceCode::Script;
    const WorldSnap* world = nullptr;
    // While the place checker runs a batch: what Luau checks for each script in
    // it, by module name, with a header --!nonstrict made --!strict.
    const std::unordered_map<std::string, std::string>* batch_sources = nullptr;

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
        if (batch_sources != nullptr) {
            const auto found = batch_sources->find(name);
            if (found != batch_sources->end()) {
                const std::optional<InstanceId> id = instance_of_module(name);
                const NodeSnap* node = id && world != nullptr ? world->find(*id) : nullptr;
                const bool module = node != nullptr && node->module;
                return Luau::SourceCode{found->second, module ? Luau::SourceCode::Module : Luau::SourceCode::Script};
            }
        }
        if (module_name != nullptr && source != nullptr && name == *module_name) {
            return Luau::SourceCode{*source, type};
        }
        const std::optional<InstanceId> id = instance_of_module(name);
        if (world == nullptr || !id) {
            return std::nullopt;
        }
        const NodeSnap* node = world->find(*id);
        if (node == nullptr || !node->lua) {
            return std::nullopt;
        }
        return Luau::SourceCode{node->source, node->module ? Luau::SourceCode::Module : Luau::SourceCode::Script};
    }

    // The same paths resolve_expr follows, one step at a time. RequireTracer
    // hands each step the step before it as context.
    std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* expr,
                                                  const Luau::TypeCheckLimits&) override {
        if (world == nullptr || context == nullptr || expr == nullptr) {
            return std::nullopt;
        }
        const std::optional<InstanceId> base = instance_of_module(context->name);
        if (!base) {
            return std::nullopt;
        }
        std::optional<InstanceId> found;
        if (auto* global = expr->as<Luau::AstExprGlobal>()) {
            if (global->name == "script") {
                found = base;
            } else if (global->name == "game") {
                found = world->root;
            } else if (global->name == "workspace") {
                found = world->workspace();
            }
        } else if (auto* call = expr->as<Luau::AstExprCall>()) {
            auto* index = call->func->as<Luau::AstExprIndexName>();
            if (index != nullptr && lua_method_resolves_child(index->index.value) && call->args.size >= 1) {
                auto* literal = call->args.data[0]->as<Luau::AstExprConstantString>();
                if (literal != nullptr && literal->isQuoted()) {
                    found = world->child_named(*base, string_literal(literal->value));
                }
            }
        } else if (auto* index = expr->as<Luau::AstExprIndexName>()) {
            if (index->index == "Parent") {
                const NodeSnap* node = world->find(*base);
                if (node != nullptr && node->parent != DataModel::kNoParent) {
                    found = node->parent;
                }
            } else {
                found = world->child_named(*base, index->index.value);
            }
        } else if (auto* literal = expr->as<Luau::AstExprConstantString>()) {
            if (literal->isQuoted()) {
                found = resolve_expr(*world, *base, literal);
            }
        }
        if (!found) {
            return std::nullopt;
        }
        return Luau::ModuleInfo{module_name_of(*found)};
    }

    std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override {
        if (display != nullptr && !display->empty() && module_name != nullptr && name == *module_name) {
            return *display;
        }
        if (const std::optional<InstanceId> id = instance_of_module(name); id && world != nullptr) {
            if (const NodeSnap* node = world->find(*id)) {
                return shown_name(*node);
            }
        }
        return name;
    }
};

struct SourceConfigResolver : Luau::ConfigResolver {
    Luau::Config config;

    const Luau::Config& getConfig(const Luau::ModuleName&, const Luau::TypeCheckLimits&) const override { return config; }
};

std::string definition_failure(const Luau::LoadDefinitionFileResult& loaded) {
    std::ostringstream out;
    out << "script analysis definitions failed to load";
    for (const Luau::ParseError& error : loaded.parseResult.errors) {
        out << '\n' << error.getMessage();
    }
    if (loaded.module) {
        for (const Luau::TypeError& error : loaded.module->errors) {
            out << '\n' << Luau::toString(error);
        }
    }
    return out.str();
}

struct WorkerEnv {
    SourceFileResolver files;
    SourceConfigResolver configs;
    std::unique_ptr<Luau::Frontend> frontend;
    std::string init_error;
    // The place copied for the check that is running. The magic reads it.
    const WorldSnap* world = nullptr;
    // The snapshot the frontend's cached modules were checked against.
    std::shared_ptr<const WorldSnap> checked_world;
    // The editor checker builds it from checked_world and marks every script
    // dirty when it is replaced, so no cached module still uses the old one's
    // types. The place checker keeps one and updates it in place (sync_place).
    std::unique_ptr<PlaceTypes> place;
    std::shared_ptr<Luau::MagicFunction> find_child;
    std::shared_ptr<Luau::MagicFunction> service_result;
    std::shared_ptr<Luau::MagicFunction> creatable_result;
    // Lint runs before the type check, so it has no types. This empty module says so:
    // the lints that want types find none, as with no module at all. Luau's
    // TableLiteral lint reads the module on every table type, so a null one crashes it.
    std::unique_ptr<Luau::Module> untyped;
    // The registry revision the definitions were built from.
    std::uint64_t revision = 0;
    // Each global library function by its type, so an alias of one is known.
    std::unordered_map<Luau::TypeId, std::pair<std::string, std::string>> global_functions;

    void init() {
        untyped = std::make_unique<Luau::Module>(std::make_shared<Luau::TypeArena>());
        // As the frontend below, so a table type's read and write fields lint the same way.
        untyped->checkedInNewSolver = true;
        configs.config.mode = Luau::Mode::Nonstrict;
        configs.config.parseOptions.captureComments = true;
        configs.config.enabledLint.setDefaults();
        frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::New, &files, &configs, Luau::FrontendOptions{});
        frontend->iceHandler.onInternalError = [](const char*) {};
        // `game` is this place's root and `script` is the instance the module
        // belongs to, children and all, instead of their bare classes.
        frontend->prepareModuleScope = [this](const Luau::ModuleName& name, const Luau::ScopePtr& scope, bool) {
            try {
                if (place != nullptr) {
                    if (const std::optional<Luau::TypeId> root = place->find(place->world->root)) {
                        scope->bindings[Luau::AstName("game")] = Luau::Binding{*root};
                    }
                    if (const std::optional<InstanceId> workspace = place->world->workspace()) {
                        if (const std::optional<Luau::TypeId> type = place->find(*workspace)) {
                            scope->bindings[Luau::AstName("workspace")] = Luau::Binding{*type};
                        }
                    }
                    if (const std::optional<InstanceId> owner = instance_of_module(name)) {
                        if (const std::optional<Luau::TypeId> self = place->find(*owner)) {
                            scope->bindings[Luau::AstName("script")] = Luau::Binding{*self};
                            return;
                        }
                    }
                }
                // A buffer the place does not hold yet is the kind of script it says it is.
                if (files.module_name != nullptr && name == *files.module_name && frontend != nullptr) {
                    const char* kind = files.type == Luau::SourceCode::Module ? "ModuleScript" : "Script";
                    if (const std::optional<Luau::TypeId> type = class_type(*frontend->globals.globalScope, kind)) {
                        scope->bindings[Luau::AstName("script")] = Luau::Binding{*type};
                    }
                }
            } catch (...) {
            }
        };
        revision = lua_registry_revision();
        try {
            Luau::unfreeze(frontend->globals.globalTypes);
            Luau::registerBuiltinGlobals(*frontend, frontend->globals);
            const std::string definitions = lua_analysis_definitions();
            const Luau::LoadDefinitionFileResult loaded = frontend->loadDefinitionFile(
                frontend->globals, frontend->globals.globalScope, definitions, "@anarchy",
                /*captureComments*/ false, /*typeCheckForAutocomplete*/ false);
            if (!loaded.success) {
                init_error = definition_failure(loaded);
            } else {
                stamp_vector(*frontend);
                attach_api(*this);
            }
            Luau::freeze(frontend->globals.globalTypes);
        } catch (const std::exception& error) {
            init_error = error.what();
        } catch (...) {
            init_error = "script analysis definitions failed to load";
        }
    }
};

void set_magic(Luau::Property& prop, const std::shared_ptr<Luau::MagicFunction>& magic) {
    if (!magic || !prop.readTy) {
        return;
    }
    Luau::FunctionType* function = Luau::getMutable<Luau::FunctionType>(Luau::follow(*prop.readTy));
    if (function == nullptr) {
        return;
    }
    function->magic = magic;
}

Luau::TypeId vector_member_type(std::string type, Luau::TypeId vector_type, Luau::NotNull<Luau::BuiltinTypes> builtins,
                                Luau::TypeArena& arena) {
    bool optional = false;
    if (!type.empty() && type.back() == '?') {
        optional = true;
        type.pop_back();
    }
    Luau::TypeId resolved = builtins->anyType;
    if (type == "number") {
        resolved = builtins->numberType;
    } else if (type == "boolean") {
        resolved = builtins->booleanType;
    } else if (type == "string") {
        resolved = builtins->stringType;
    } else if (type == "thread") {
        resolved = builtins->threadType;
    } else if (type == "nil") {
        resolved = builtins->nilType;
        optional = false;
    } else if (type == "Vector3" || type == "vector") {
        resolved = vector_type;
    }
    if (optional) {
        resolved = Luau::makeOption(builtins, arena, resolved);
    }
    return resolved;
}

Luau::TypeId vector_method(Luau::TypeArena& arena, Luau::TypeId self, const std::vector<Luau::TypeId>& params, Luau::TypeId result) {
    switch (params.size()) {
    case 0:
        return Luau::makeFunction(arena, self, {}, {result});
    case 1:
        return Luau::makeFunction(arena, self, {params[0]}, {result});
    case 2:
        return Luau::makeFunction(arena, self, {params[0], params[1]}, {result});
    case 3:
        return Luau::makeFunction(arena, self, {params[0], params[1], params[2]}, {result});
    default:
        break;
    }
    std::vector<Luau::TypeId> arguments;
    arguments.reserve(params.size() + 1);
    arguments.push_back(self);
    arguments.insert(arguments.end(), params.begin(), params.end());
    Luau::TypePackId argument_pack = arena.addTypePack(std::move(arguments));
    Luau::TypePackId result_pack = arena.addTypePack({result});
    Luau::FunctionType function{{}, {}, argument_pack, result_pack, {}, true};
    function.argNames.emplace_back(Luau::FunctionArgument{"self", {}});
    for (std::size_t index = 0; index < params.size(); ++index) {
        function.argNames.emplace_back(std::nullopt);
    }
    return arena.addType(std::move(function));
}

// Vector3 is Luau's builtin vector. Its arithmetic stays on that type. The
// class registry contributes the X/Y/Z properties and the methods.
void stamp_vector(Luau::Frontend& frontend) {
    if (frontend.globals.globalScope == nullptr) {
        return;
    }
    const auto found = frontend.globals.globalScope->exportedTypeBindings.find("vector");
    if (found == frontend.globals.globalScope->exportedTypeBindings.end()) {
        return;
    }
    const Luau::TypeId vector_type = Luau::follow(found->second.type);
    Luau::ExternType* vector = Luau::getMutable<Luau::ExternType>(vector_type);
    if (vector == nullptr) {
        return;
    }
    Luau::TypeArena& arena = frontend.globals.globalTypes;
    Luau::NotNull<Luau::BuiltinTypes> builtins = frontend.globals.builtinTypes;
    std::vector<LuaField> fields;
    lua_class_own_members("Vector3", fields);
    for (const LuaField& field : fields) {
        if (field.blocked || field.name == nullptr) {
            continue;
        }
        const std::string result_name = field.type_name != nullptr ? field.type_name : "";
        const Luau::TypeId result = vector_member_type(result_name.empty() ? "nil" : result_name, vector_type, builtins, arena);
        if (!field.method) {
            vector->props[field.name] = field.writable ? Luau::Property::rw(result) : Luau::Property::readonly(result);
            continue;
        }
        const LuaDoc doc = lua_symbol_doc("Vector3", field.name);
        std::vector<Luau::TypeId> params;
        if (doc.found) {
            for (const LuaDocParam& param : doc.params) {
                params.push_back(vector_member_type(param.type_name, vector_type, builtins, arena));
            }
        }
        vector->props[field.name] = Luau::Property::readonly(vector_method(arena, vector_type, params, result));
    }
}

void attach_api(WorkerEnv& env) {
    if (env.frontend == nullptr || env.frontend->globals.globalScope == nullptr) {
        return;
    }
    for (const auto& [symbol, binding] : env.frontend->globals.globalScope->bindings) {
        const Luau::TableType* table = Luau::get<Luau::TableType>(Luau::follow(binding.typeId));
        if (table == nullptr || symbol.global.value == nullptr) {
            continue;
        }
        for (const auto& [name, prop] : table->props) {
            if (prop.readTy && Luau::get<Luau::FunctionType>(Luau::follow(*prop.readTy)) != nullptr) {
                env.global_functions[Luau::follow(*prop.readTy)] = {symbol.global.value, name};
            }
        }
    }
    env.find_child = std::make_shared<FindChildMagic>(&env);
    env.service_result = std::make_shared<NarrowMagic>(NarrowMagic::Kind::Service);
    env.creatable_result = std::make_shared<NarrowMagic>(NarrowMagic::Kind::Creatable);

    for (auto& binding : env.frontend->globals.globalScope->exportedTypeBindings) {
        Luau::ExternType* type = Luau::getMutable<Luau::ExternType>(Luau::follow(binding.second.type));
        if (type == nullptr) {
            continue;
        }
        for (auto& entry : type->props) {
            const LuaField* field = lua_class_find(type->name.c_str(), entry.first);
            if (field == nullptr) {
                continue;
            }
            if (field->resolves_child) {
                set_magic(entry.second, env.find_child);
            } else if (field->service_arg) {
                set_magic(entry.second, env.service_result);
            } else if (field->class_from_arg) {
                set_magic(entry.second, env.creatable_result);
            }
        }
    }

    std::vector<std::string> classes;
    lua_class_names(classes);
    for (const std::string& name : classes) {
        Luau::Binding* binding = Luau::tryGetGlobalBindingRef(env.frontend->globals, name);
        if (binding == nullptr) {
            continue;
        }
        Luau::TableType* table = Luau::getMutable<Luau::TableType>(Luau::follow(binding->typeId));
        if (table == nullptr) {
            continue;
        }
        for (auto& entry : table->props) {
            if (!lua_function_result(name, entry.first).class_from_arg) {
                continue;
            }
            set_magic(entry.second, env.creatable_result);
        }
    }
}

// On a pool thread an exception escaping a Luau task leaves checkQueuedModules
// waiting for it forever, so a hook that fails keeps the declared type instead.
bool FindChildMagic::infer(const Luau::MagicFunctionCallContext& context) {
    try {
        return infer_child(context);
    } catch (...) {
        return false;
    }
}

bool FindChildMagic::infer_child(const Luau::MagicFunctionCallContext& context) {
    if (env == nullptr || env->world == nullptr) {
        return false;
    }
    // Modules are checked on several threads, a required one in the same pass,
    // so `script` is whichever module this call is in. The constraint names it
    // only under LuauCyclicRequireTypeInference; otherwise the solver's module does.
    const Luau::ModuleName* module_name = context.constraint->moduleName.get();
    if (module_name == nullptr && context.solver->module != nullptr) {
        module_name = &context.solver->module->name;
    }
    if (module_name == nullptr) {
        return false;
    }
    const std::optional<InstanceId> owner = instance_of_module(*module_name);
    if (!owner) {
        return false;
    }
    const InstanceId self = *owner;
    // The receiver's type names its instance when it came from game, script, a
    // dotted child, or an earlier lookup, even through a local. Otherwise the
    // call's own path is followed.
    std::optional<InstanceId> child;
    const Luau::AstExprCall* call = context.callSite.get();
    const auto* literal = call->args.size >= 1 ? call->args.data[0]->as<Luau::AstExprConstantString>() : nullptr;
    if (call->self && literal != nullptr && literal->isQuoted()) {
        const auto [head, tail] = Luau::flatten(context.arguments);
        if (!head.empty()) {
            if (const std::optional<InstanceId> receiver = tagged_instance(head[0])) {
                child = env->world->child_named(*receiver, string_literal(literal->value));
            }
        }
    }
    if (!child) {
        child = resolve_expr(*env->world, self, const_cast<Luau::AstExprCall*>(call));
    }
    if (!child) {
        return false;
    }
    std::optional<Luau::TypeId> result = env->place != nullptr ? env->place->find(*child) : std::nullopt;
    if (!result) {
        const NodeSnap* node = env->world->find(*child);
        if (node == nullptr || node->class_name.empty()) {
            return false;
        }
        result = class_type(*context.solver->rootScope, node->class_name);
    }
    if (!result) {
        return false;
    }
    Luau::TypeArena* arena = context.solver->arena.get();
    Luau::asMutable(context.result)->ty.emplace<Luau::BoundTypePack>(arena->addTypePack({*result}));
    return true;
}

bool NarrowMagic::infer(const Luau::MagicFunctionCallContext& context) {
    try {
        return infer_narrow(context);
    } catch (...) {
        return false;
    }
}

bool NarrowMagic::infer_narrow(const Luau::MagicFunctionCallContext& context) {
    const Luau::AstExprCall* call = context.callSite.get();
    if (call == nullptr || call->args.size < 1) {
        return false;
    }
    const auto* literal = call->args.data[0]->as<Luau::AstExprConstantString>();
    if (literal == nullptr || !literal->isQuoted()) {
        return false;
    }
    const std::string name = string_literal(literal->value);
    if (kind == Kind::Service) {
        if (!lua_service_known(name.c_str())) {
            return false;
        }
    } else if (!lua_creatable_known(name.c_str())) {
        return false;
    }
    const std::optional<Luau::TypeFun> type_fun = context.solver->rootScope->lookupType(name);
    if (!type_fun) {
        return false;
    }
    const Luau::TypeId class_ty = Luau::follow(type_fun->type);
    if (Luau::get<Luau::ExternType>(class_ty) == nullptr) {
        return false;
    }
    Luau::TypeArena* arena = context.solver->arena.get();
    Luau::asMutable(context.result)->ty.emplace<Luau::BoundTypePack>(arena->addTypePack({class_ty}));
    return true;
}

// The source the type check reads. The `--!nonstrict` that set the mode is
// rewritten so Luau does not switch to its smaller nonstrict pass, which skips
// unknown properties such as PreRender. It is the first header mode comment,
// on whatever line.
std::string checked_source(const std::string& source, const Luau::ParseResult& parsed, Luau::Mode mode) {
    std::string check_source = source;
    if (mode != Luau::Mode::Nonstrict) {
        return check_source;
    }
    for (const Luau::HotComment& comment : parsed.hotcomments) {
        if (!comment.header ||
            (comment.content != "nocheck" && comment.content != "nonstrict" && comment.content != "strict")) {
            continue;
        }
        std::size_t line_start = 0;
        for (unsigned line = 0; line < comment.location.begin.line && line_start != std::string::npos; ++line) {
            line_start = check_source.find('\n', line_start);
            line_start = line_start == std::string::npos ? line_start : line_start + 1;
        }
        if (comment.content == "nonstrict" && line_start != std::string::npos) {
            const std::size_t line_end = check_source.find('\n', line_start);
            const std::size_t at = check_source.find("--!nonstrict", line_start);
            if (at != std::string::npos && (line_end == std::string::npos || at < line_end)) {
                check_source.replace(at, std::char_traits<char>::length("--!nonstrict"), "--!strict");
            }
        }
        break;
    }
    return check_source;
}

// Required modules stay cached in the frontend. A new snapshot can change
// their source or what their paths reach, so recheck them. A script the
// snapshot lacks is gone, and so is its cached module.
void sync_world(WorkerEnv& env, const std::shared_ptr<const WorldSnap>& world, bool keep_on_empty) {
    // A place with no root, as when an analysis pass could not read it, says
    // nothing about the tree. The cached modules stay for the next real
    // snapshot. A completion asked with no place means none.
    if (env.checked_world == world || (keep_on_empty && world->nodes.empty())) {
        return;
    }
    // The same tree keeps every cached module and the place's types. Only a
    // script whose source changed, and what requires it, is checked again.
    if (env.checked_world != nullptr && env.place != nullptr && same_tree(*env.checked_world, *world)) {
        std::unordered_map<InstanceId, const NodeSnap*> before;
        before.reserve(env.checked_world->nodes.size());
        for (const NodeSnap& node : env.checked_world->nodes) {
            before.emplace(node.id, &node);
        }
        for (const NodeSnap& node : world->nodes) {
            if (node.lua && before.at(node.id)->source != node.source) {
                env.frontend->markDirty(module_name_of(node.id));
            }
        }
        env.checked_world = world;
        return;
    }
    std::unordered_set<std::string> live;
    for (const NodeSnap& node : world->nodes) {
        if (node.lua) {
            live.insert(module_name_of(node.id));
            env.frontend->markDirty(module_name_of(node.id));
        }
    }
    std::vector<Luau::ModuleName> gone;
    for (const auto& cached : env.frontend->sourceNodes) {
        if (live.count(cached.first) == 0) {
            gone.push_back(cached.first);
        }
    }
    env.frontend->clearModules(gone);
    env.checked_world = world;
    env.place = build_place_types(*env.frontend->globals.globalScope, world);
}

// A Luau request: the completion at `offset` (none when npos) and the type at
// each of `offsets`. The worker answers it ahead of queued checks.
struct CompleteRequest {
    std::vector<std::size_t> offsets;
    // Requests in one lane replace each other while queued. Empty never does.
    std::string lane;
    std::shared_ptr<const WorldSnap> world;
    InstanceId script = 0;
    std::string source;
    std::size_t offset = 0;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::shared_ptr<LuauAnswer> answer = std::make_shared<LuauAnswer>();
    // Shutdown cancels the check a request is running.
    std::shared_ptr<Luau::FrontendCancellationToken> cancel = std::make_shared<Luau::FrontendCancellationToken>();
};

// Publishes a request's answer to both the waiting and the polling side.
void finish_request(CompleteRequest& request) {
    request.answer->ready.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> done(request.mu);
        request.done = true;
    }
    request.cv.notify_all();
}

// Line and byte column of a byte offset, as Luau counts them.
Luau::Position position_of(const std::string& source, std::size_t offset) {
    unsigned line = 0;
    std::size_t line_start = 0;
    const std::size_t end = offset < source.size() ? offset : source.size();
    for (std::size_t at = 0; at < end; ++at) {
        if (source[at] == '\n') {
            ++line;
            line_start = at + 1;
        }
    }
    return Luau::Position{line, static_cast<unsigned>(end - line_start)};
}

const char* kind_name(Luau::AutocompleteEntryKind kind) {
    switch (kind) {
    case Luau::AutocompleteEntryKind::Property:
        return "property";
    case Luau::AutocompleteEntryKind::Binding:
        return "binding";
    case Luau::AutocompleteEntryKind::Keyword:
        return "keyword";
    case Luau::AutocompleteEntryKind::String:
        return "string";
    case Luau::AutocompleteEntryKind::Type:
        return "type";
    case Luau::AutocompleteEntryKind::Module:
        return "module";
    case Luau::AutocompleteEntryKind::GeneratedFunction:
        return "function";
    case Luau::AutocompleteEntryKind::RequirePath:
        return "require path";
    case Luau::AutocompleteEntryKind::HotComment:
        return "hot comment";
    }
    return "unknown";
}

const char* context_name(Luau::AutocompleteContext context) {
    switch (context) {
    case Luau::AutocompleteContext::Expression:
        return "expression";
    case Luau::AutocompleteContext::Statement:
        return "statement";
    case Luau::AutocompleteContext::Property:
        return "property";
    case Luau::AutocompleteContext::Type:
        return "type";
    case Luau::AutocompleteContext::Keyword:
        return "keyword";
    case Luau::AutocompleteContext::String:
        return "string";
    case Luau::AutocompleteContext::HotComment:
        return "hot comment";
    case Luau::AutocompleteContext::Unknown:
        break;
    }
    return "unknown";
}

// A function type as a completion row shows it: its parameters after the name,
// and what it returns. `with_self` drops the first parameter, as a ':' call does.
// A type as a completion row or hover writes it. A literal alone is its kind:
// "hi" reads string and true reads boolean, as the value's type, not its
// value. Inside a union it stays, so "sit" | "roll" says what it allows.
std::string shown_type(Luau::TypeId type) {
    const Luau::TypeId followed = Luau::follow(type);
    if (const auto* singleton = Luau::get<Luau::SingletonType>(followed)) {
        return Luau::get<Luau::BooleanSingleton>(singleton) != nullptr ? "boolean" : "string";
    }
    return Luau::toString(followed);
}

// What a method's self is, as a title shows it: its class, else the table
// the method was written on.
std::string self_type(Luau::TypeId type) {
    const std::string class_name = registered_class(Luau::follow(type));
    return class_name.empty() ? std::string("table") : class_name;
}

// The source of a module the worker can read: the buffer being answered, or a
// script in the place.
const std::string* module_text(const WorkerEnv& env, const std::string& module) {
    if (env.files.module_name != nullptr && env.files.source != nullptr && module == *env.files.module_name) {
        return env.files.source;
    }
    const std::optional<InstanceId> id = instance_of_module(module);
    if (env.files.world == nullptr || !id) {
        return nullptr;
    }
    const NodeSnap* node = env.files.world->find(*id);
    return node != nullptr && node->lua ? &node->source : nullptr;
}

// The text of a position's line in a source.
std::string_view line_of(const std::string& text, unsigned line) {
    std::size_t start = 0;
    for (unsigned at = 0; at < line && start != std::string::npos; ++at) {
        start = text.find('\n', start);
        start = start == std::string::npos ? start : start + 1;
    }
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find('\n', start);
    return std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The name a definition wrote after `function`: `module:Test` in
// `function module:Test()`. Empty for `function(` itself.
std::string defined_name(const WorkerEnv& env, const Luau::FunctionType& fn) {
    if (!fn.definition || !fn.definition->definitionModuleName) {
        return {};
    }
    const std::string* text = module_text(env, *fn.definition->definitionModuleName);
    if (text == nullptr) {
        return {};
    }
    const Luau::Location& where = fn.definition->definitionLocation;
    const std::string_view line = line_of(*text, where.begin.line);
    const std::size_t column = std::min<std::size_t>(where.begin.column, line.size());
    const std::size_t keyword = line.substr(0, column + 8).rfind("function");
    if (keyword == std::string_view::npos) {
        return {};
    }
    const std::size_t open = line.find('(', keyword);
    if (open == std::string_view::npos) {
        return {};
    }
    std::string name(line.substr(keyword + 8, open - keyword - 8));
    name.erase(std::remove_if(name.begin(), name.end(), [](char unit) { return unit == ' ' || unit == '\t'; }), name.end());
    return name;
}

// The source a location spans, on one line: runs of blank space become one.
std::string text_at(const std::string& text, const Luau::Location& where) {
    const auto offset = [&](const Luau::Position& position) {
        std::size_t at = 0;
        for (unsigned line = 0; line < position.line && at != std::string::npos; ++line) {
            at = text.find('\n', at);
            at = at == std::string::npos ? at : at + 1;
        }
        return at == std::string::npos ? text.size() : std::min(text.size(), at + position.column);
    };
    const std::size_t begin = offset(where.begin);
    const std::size_t end = std::max(begin, offset(where.end));
    std::string out;
    bool blank = false;
    for (std::size_t at = begin; at < end; ++at) {
        const char unit = text[at];
        if (unit == ' ' || unit == '\t' || unit == '\r' || unit == '\n') {
            blank = !out.empty();
            continue;
        }
        if (blank) {
            out += ' ';
            blank = false;
        }
        out += unit;
    }
    return out;
}

// The function node a definition is, in a module checked with its types kept.
struct FindFunction : Luau::AstVisitor {
    Luau::Location where;
    Luau::AstExprFunction* found = nullptr;

    bool visit(Luau::AstExprFunction* node) override {
        if (node->location == where) {
            found = node;
            return false;
        }
        return found == nullptr;
    }
};

// The returns of one function, not of the functions inside it.
struct FindReturns : Luau::AstVisitor {
    Luau::AstExprFunction* self = nullptr;
    std::vector<Luau::AstStatReturn*> returns;

    bool visit(Luau::AstExprFunction* node) override { return node == self; }
    bool visit(Luau::AstStatReturn* node) override {
        returns.push_back(node);
        return true;
    }
};

// What a function's return statements say, as the resolver judged them:
// `none` when no return gives a value, `disagree` when two give different
// counts or types, and `first` the types the first return gives. Comparing
// types needs a module checked with its types kept.
void judge_returns(const WorkerEnv& env, const Luau::FunctionType& fn, bool& none, bool& disagree,
                   std::vector<std::string>* first_types = nullptr) {
    none = false;
    disagree = false;
    if (!fn.definition || !fn.definition->definitionModuleName || env.frontend == nullptr) {
        return;
    }
    const std::string& module_name = *fn.definition->definitionModuleName;
    const Luau::SourceModule* source = env.frontend->getSourceModule(module_name);
    if (source == nullptr || source->root == nullptr) {
        return;
    }
    FindFunction find;
    find.where = fn.definition->definitionLocation;
    source->root->visit(&find);
    if (find.found == nullptr) {
        return;
    }
    FindReturns returns;
    returns.self = find.found;
    find.found->body->visit(&returns);
    none = std::all_of(returns.returns.begin(), returns.returns.end(),
                       [](const Luau::AstStatReturn* statement) { return statement->list.size == 0; });
    const Luau::ModulePtr module = env.frontend->moduleResolver.getModule(module_name);
    if (module == nullptr || module->astTypes.empty()) {
        return;
    }
    std::optional<std::vector<std::string>> first;
    for (Luau::AstStatReturn* statement : returns.returns) {
        std::vector<std::string> shown;
        for (Luau::AstExpr* expr : statement->list) {
            const Luau::TypeId* type = module->astTypes.find(expr);
            shown.push_back(type != nullptr ? shown_type(*type) : std::string("?"));
        }
        if (!first) {
            first = std::move(shown);
            if (first_types != nullptr) {
                *first_types = *first;
            }
        } else if (*first != shown) {
            disagree = true;
            return;
        }
    }
}

// Whether a function was written in another script than the one being
// answered. A required module's function reads as its first return says.
bool defined_elsewhere(const WorkerEnv& env, const Luau::FunctionType& fn) {
    return fn.definition && fn.definition->definitionModuleName && env.files.module_name != nullptr &&
           *fn.definition->definitionModuleName != *env.files.module_name;
}

std::string pack_text(const std::vector<std::string>& types) {
    if (types.size() == 1) {
        return types[0];
    }
    std::string out = "(";
    for (std::size_t index = 0; index < types.size(); ++index) {
        out += (index == 0 ? "" : ", ") + types[index];
    }
    return out + ")";
}

// The definition of a function in its module's source, and that source, when
// the worker can read both.
struct Definition {
    Luau::AstExprFunction* node = nullptr;
    const std::string* text = nullptr;
};

Definition find_definition(const WorkerEnv& env, const Luau::FunctionType& fn) {
    Definition out;
    if (!fn.definition || !fn.definition->definitionModuleName || env.frontend == nullptr) {
        return out;
    }
    const std::string& module_name = *fn.definition->definitionModuleName;
    const Luau::SourceModule* source = env.frontend->getSourceModule(module_name);
    const std::string* text = module_text(env, module_name);
    if (source == nullptr || source->root == nullptr || text == nullptr) {
        return out;
    }
    FindFunction find;
    find.where = fn.definition->definitionLocation;
    source->root->visit(&find);
    out.node = find.found;
    out.text = find.found != nullptr ? text : nullptr;
    return out;
}

// A function type as rows and hovers use it. Every parameter is listed, self
// too for a method; `params` leaves self out when the function is called
// with ':', as `with_self` says. A parameter or return the definition
// annotated reads as written there, as `target: Vector2D.Vector2D`, rather
// than as the type it expands to.
void describe_function(const WorkerEnv* env, const Luau::FunctionType& fn, bool with_self, LuauSuggestion& out) {
    const auto [args, args_tail] = Luau::flatten(fn.argTypes);
    const Definition written = env != nullptr ? find_definition(*env, fn) : Definition{};
    const auto annotation = [&](std::size_t index) -> std::string {
        if (written.node == nullptr) {
            return {};
        }
        const std::size_t own = fn.hasSelf ? index - 1 : index;
        if ((fn.hasSelf && index == 0) || own >= written.node->args.size) {
            return {};
        }
        const Luau::AstLocal* arg = written.node->args.data[own];
        return arg->annotation != nullptr ? text_at(*written.text, arg->annotation->location) : std::string();
    };
    std::string params = "(";
    bool first = true;
    for (std::size_t index = 0; index < args.size(); ++index) {
        std::string name;
        if (index < fn.argNames.size() && fn.argNames[index] && !fn.argNames[index]->name.empty()) {
            name = fn.argNames[index]->name;
        }
        const bool self = index == 0 && fn.hasSelf;
        if (self && name.empty()) {
            name = "self";
        }
        const std::string noted = annotation(index);
        const std::string type = !noted.empty() ? noted : self ? self_type(args[index]) : shown_type(args[index]);
        out.param_list.emplace_back(name, type);
        if (index == 0 && (with_self || (fn.hasSelf && with_self))) {
            continue;
        }
        params += first ? "" : ", ";
        first = false;
        params += name.empty() ? type : name + ": " + type;
    }
    // Luau gives a function it inferred a hidden `...` tail. Only a written one shows.
    if (args_tail) {
        const auto* variadic = Luau::get<Luau::VariadicTypePack>(Luau::follow(*args_tail));
        if (variadic == nullptr || !variadic->hidden) {
            params += first ? "..." : ", ...";
            out.variadic = true;
        }
    }
    out.params = params + ")";
    if (!args.empty()) {
        const Luau::TypeId receiver = Luau::follow(args[0]);
        const bool vague = Luau::get<Luau::AnyType>(receiver) != nullptr ||
                           Luau::get<Luau::UnknownType>(receiver) != nullptr ||
                           Luau::get<Luau::GenericType>(receiver) != nullptr ||
                           Luau::get<Luau::FreeType>(receiver) != nullptr || Luau::get<Luau::ErrorType>(receiver) != nullptr;
        const bool named_self = !fn.argNames.empty() && fn.argNames[0] && fn.argNames[0]->name == "self";
        out.takes_receiver = fn.hasSelf || (!vague && (named_self || !annotation(0).empty()));
    }
    const auto [rets, rets_tail] = Luau::flatten(fn.retTypes);
    if (rets.size() == 1 && !rets_tail) {
        out.returns = shown_type(rets[0]);
    } else if (!rets.empty() || rets_tail) {
        std::string pack = "(";
        for (std::size_t index = 0; index < rets.size(); ++index) {
            pack += (index == 0 ? "" : ", ") + shown_type(rets[index]);
        }
        if (rets_tail) {
            pack += rets.empty() ? "..." : ", ...";
        }
        out.returns = pack + ")";
    }
    out.function = true;
    out.method = fn.hasSelf;
    if (env != nullptr) {
        out.defined_as = defined_name(*env, fn);
        std::vector<std::string> first_return;
        judge_returns(*env, fn, out.returns_none, out.returns_disagree, &first_return);
        // A required module's function that returns different things reads
        // as its first return, as the resolver read it.
        if (out.returns_disagree && defined_elsewhere(*env, fn) && !first_return.empty()) {
            out.returns = pack_text(first_return);
            out.returns_disagree = false;
        }
        // A written return is what the function promises.
        if (written.node != nullptr && written.node->returnAnnotation != nullptr) {
            out.returns = text_at(*written.text, written.node->returnAnnotation->location);
            out.returns_disagree = false;
            out.returns_none = out.returns == "()";
        }
    }
}

// Marks every module `module_name` requires, however deep, whose check kept
// no expression types. True when there was one.
bool forget_untyped_requires(WorkerEnv& env, const std::string& module_name) {
    std::vector<std::string> stack{module_name};
    std::unordered_set<std::string> seen{module_name};
    bool marked = false;
    while (!stack.empty()) {
        const std::string name = std::move(stack.back());
        stack.pop_back();
        const auto node = env.frontend->sourceNodes.find(name);
        if (node == env.frontend->sourceNodes.end() || node->second == nullptr) {
            continue;
        }
        for (const std::string& required : node->second->requireSet) {
            if (!seen.insert(required).second) {
                continue;
            }
            stack.push_back(required);
            const Luau::ModulePtr module = env.frontend->moduleResolver.getModule(required);
            if (module != nullptr && module->astTypes.empty()) {
                env.frontend->markDirty(required);
                marked = true;
            }
        }
    }
    return marked;
}

// Checks the request's buffer with full type graphs kept and calls `answer`
// with the module name. Then marks the module dirty, so the next check reads
// the place's own source. Returns an error, or empty.
template <typename Answer>
std::string with_checked_buffer(WorkerEnv& env, const CompleteRequest& request, Answer&& answer) {
    if (!env.init_error.empty() || env.frontend == nullptr || env.frontend->globals.globalScope == nullptr) {
        return env.init_error.empty() ? "script analysis is unavailable" : env.init_error;
    }
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions parse_options;
    parse_options.captureComments = true;
    const Luau::ParseResult parsed =
        Luau::Parser::parse(request.source.c_str(), request.source.size(), names, allocator, parse_options);
    const Luau::Mode mode = Luau::parseMode(parsed.hotcomments).value_or(Luau::Mode::Nonstrict);
    const std::string check_source = checked_source(request.source, parsed, mode);
    const std::string module_name = module_name_of(request.script);
    const NodeSnap* self = request.world->find(request.script);
    const std::string display = self != nullptr && !self->name.empty() ? self->name : std::string("script");
    std::string error;
    try {
        sync_world(env, request.world, false);
        env.files.module_name = &module_name;
        env.files.world = request.world.get();
        env.files.source = &check_source;
        env.files.display = &display;
        env.files.type = self != nullptr && self->module ? Luau::SourceCode::Module : Luau::SourceCode::Script;
        env.configs.config.mode = Luau::Mode::Strict;
        env.world = request.world.get();
        env.frontend->markDirty(module_name);
        Luau::FrontendOptions options;
        options.runLintChecks = false;
        options.retainFullTypeGraphs = true;
        options.cancellationToken = request.cancel;
        env.frontend->check(module_name, options);
        // A module the analyzer checked keeps no types for its expressions, and
        // what a required module replaces or returns is read from them. Those
        // are checked again, with their types kept, until they next change.
        if (!request.cancel->requested() && forget_untyped_requires(env, module_name)) {
            env.frontend->check(module_name, options);
        }
        if (request.cancel->requested()) {
            error = "cancelled";
        } else {
            answer(module_name);
        }
    } catch (const std::exception& failure) {
        error = request.cancel->requested() ? "cancelled" : failure.what();
    } catch (...) {
        error = request.cancel->requested() ? "cancelled" : "the type check failed";
    }
    env.world = nullptr;
    env.files.world = nullptr;
    env.files.module_name = nullptr;
    env.files.source = nullptr;
    env.files.display = nullptr;
    env.frontend->markDirty(module_name);
    return error;
}

LuauCompletion completion_at(WorkerEnv& env, const std::string& module_name, const std::string& text,
                             std::size_t offset);

// The type of a local where it is bound: the binding in whichever scope of the
// module declared it. The scope at the declaration itself does not have it yet.
std::optional<Luau::TypeId> local_type(const Luau::Module& module, Luau::AstLocal* local) {
    const Luau::Symbol symbol(local);
    for (const auto& [location, scope] : module.scopes) {
        if (scope == nullptr) {
            continue;
        }
        const auto binding = scope->bindings.find(symbol);
        if (binding != scope->bindings.end()) {
            return binding->second.typeId;
        }
    }
    return std::nullopt;
}

// The object expression of a member, as the owner a docs lookup and a
// FindFirstChild receiver need: a library global's name, or the registered
// class and instance of its type.
void describe_object(const Luau::Module& module, Luau::AstExpr* object, std::string& owner, bool& instance_known,
                     InstanceId& instance) {
    if (auto* global = object->as<Luau::AstExprGlobal>()) {
        owner = global->name.value;
    }
    if (object->is<Luau::AstExprConstantString>()) {
        owner = "string";
    }
    if (const Luau::TypeId* type = module.astTypes.find(object)) {
        const Luau::TypeId followed = Luau::follow(*type);
        const auto* primitive = Luau::get<Luau::PrimitiveType>(followed);
        if ((primitive != nullptr && primitive->type == Luau::PrimitiveType::String) ||
            (Luau::get<Luau::SingletonType>(followed) != nullptr && Luau::toString(followed).rfind('"', 0) == 0)) {
            owner = "string";
        }
        const std::string class_name = registered_class(*type);
        if (!class_name.empty()) {
            owner = class_name;
        }
        if (const std::optional<InstanceId> tagged = tagged_instance(*type)) {
            instance_known = true;
            instance = *tagged;
        }
    }
}

// The `local` statement that declares a local.
struct FindDeclaration : Luau::AstVisitor {
    const Luau::AstLocal* local = nullptr;
    Luau::AstStatLocal* found = nullptr;

    bool visit(Luau::AstStatLocal* node) override {
        for (Luau::AstLocal* var : node->vars) {
            if (var == local) {
                found = node;
            }
        }
        return found == nullptr;
    }
};

// The table field a statement writes, as `module.new` in `module.new = f`
// or `function module.new()`.
bool written_field(Luau::AstExpr* target, const Luau::AstLocal*& table, std::string& field) {
    auto* index = target->as<Luau::AstExprIndexName>();
    auto* object = index != nullptr ? index->expr->as<Luau::AstExprLocal>() : nullptr;
    if (object == nullptr) {
        return false;
    }
    table = object->local;
    field = index->index.value;
    return true;
}

// What a module leaves in a field it first filled with this function, when a
// later top-level write replaces it: `module.new = function() ... end` after
// `function module.new() ... end`. The runtime keeps the last write; Luau
// types the field by the first.
std::optional<Luau::TypeId> replaced_by(const WorkerEnv& env, const Luau::FunctionType& fn) {
    if (!fn.definition || !fn.definition->definitionModuleName || env.frontend == nullptr) {
        return std::nullopt;
    }
    const std::string& module_name = *fn.definition->definitionModuleName;
    const Luau::SourceModule* source = env.frontend->getSourceModule(module_name);
    const Luau::ModulePtr module = env.frontend->moduleResolver.getModule(module_name);
    if (source == nullptr || source->root == nullptr || module == nullptr || module->astTypes.empty()) {
        return std::nullopt;
    }
    const Luau::Location& where = fn.definition->definitionLocation;
    const Luau::AstLocal* table = nullptr;
    std::string field;
    Luau::AstExpr* last = nullptr;
    const auto wrote = [&](Luau::AstExpr* target, Luau::AstExpr* value) {
        const Luau::AstLocal* written_table = nullptr;
        std::string written;
        if (!written_field(target, written_table, written)) {
            return;
        }
        if (table == nullptr && value->is<Luau::AstExprFunction>() && value->location == where) {
            table = written_table;
            field = written;
            last = value;
        } else if (table != nullptr && written_table == table && written == field) {
            last = value;
        }
    };
    for (Luau::AstStat* stat : source->root->body) {
        if (auto* defined = stat->as<Luau::AstStatFunction>()) {
            wrote(defined->name, defined->func);
        } else if (auto* assign = stat->as<Luau::AstStatAssign>()) {
            for (std::size_t index = 0; index < assign->vars.size && index < assign->values.size; ++index) {
                wrote(assign->vars.data[index], assign->values.data[index]);
            }
        }
    }
    if (last == nullptr || last->location == where) {
        return std::nullopt;
    }
    const Luau::TypeId* type = module->astTypes.find(last);
    return type != nullptr ? std::optional<Luau::TypeId>(*type) : std::nullopt;
}

// A function type, or what its module replaced it with.
const Luau::FunctionType* kept_function(const WorkerEnv& env, Luau::TypeId& type) {
    const auto* fn = Luau::get<Luau::FunctionType>(Luau::follow(type));
    if (fn == nullptr) {
        return nullptr;
    }
    if (const std::optional<Luau::TypeId> replaced = replaced_by(env, *fn)) {
        if (const auto* kept = Luau::get<Luau::FunctionType>(Luau::follow(*replaced))) {
            type = *replaced;
            return kept;
        }
    }
    return fn;
}

// The call a local takes its value from, and which of the call's values it
// takes: 1 for y in `local x, y = f()`. A call in parentheses gives one value
// and is not followed.
Luau::AstExprCall* value_call(const Luau::SourceModule& source, Luau::ExprOrLocal& found, std::size_t& value) {
    const Luau::AstLocal* local = found.getLocal();
    if (local == nullptr && found.getExpr() != nullptr) {
        if (auto* named = found.getExpr()->as<Luau::AstExprLocal>()) {
            local = named->local;
        }
    }
    if (local == nullptr || source.root == nullptr) {
        return nullptr;
    }
    FindDeclaration find;
    find.local = local;
    source.root->visit(&find);
    if (find.found == nullptr || find.found->values.size == 0) {
        return nullptr;
    }
    std::size_t index = 0;
    while (index < find.found->vars.size && find.found->vars.data[index] != local) {
        ++index;
    }
    const std::size_t last = find.found->values.size - 1;
    value = index < last ? 0 : index - last;
    return find.found->values.data[std::min(index, last)]->as<Luau::AstExprCall>();
}

// What a local holds when the call it takes its value from reads otherwise
// than Luau types it: the replaced function's return, or, when the returns
// disagree, nothing for a function written here and the first return's type
// for a required one. Nothing to say when Luau's type stands.
struct CallValue {
    std::optional<Luau::TypeId> type;
    std::optional<std::string> shown;
};

std::optional<CallValue> call_value(const WorkerEnv& env, const Luau::SourceModule& source,
                                    const Luau::Module& module, Luau::ExprOrLocal& found) {
    std::size_t value = 0;
    Luau::AstExprCall* call = value_call(source, found, value);
    const Luau::TypeId* callee = call != nullptr ? module.astTypes.find(call->func) : nullptr;
    if (callee == nullptr) {
        return std::nullopt;
    }
    Luau::TypeId kept = *callee;
    const Luau::FunctionType* fn = kept_function(env, kept);
    if (fn == nullptr) {
        return std::nullopt;
    }
    CallValue out;
    if (kept != *callee) {
        const auto [rets, tail] = Luau::flatten(fn->retTypes);
        if (value < rets.size()) {
            out.type = rets[value];
        } else {
            out.shown = std::string();
        }
        return out;
    }
    bool none = false;
    bool disagree = false;
    std::vector<std::string> first;
    judge_returns(env, *fn, none, disagree, &first);
    if (!disagree) {
        return std::nullopt;
    }
    out.shown = defined_elsewhere(env, *fn) && value < first.size() ? first[value] : std::string();
    return out;
}

// The `type` statement that declares `name` in a module, as it is written.
std::string type_declaration(const WorkerEnv& env, const std::string& module_name, const std::string& name) {
    const Luau::SourceModule* source = env.frontend->getSourceModule(module_name);
    const std::string* text = module_text(env, module_name);
    if (source == nullptr || source->root == nullptr || text == nullptr) {
        return {};
    }
    for (Luau::AstStat* stat : source->root->body) {
        auto* alias = stat->as<Luau::AstStatTypeAlias>();
        if (alias != nullptr && name == alias->name.value) {
            return text_at(*text, alias->location);
        }
    }
    return {};
}

// A type name at the position: `Diet`, or `Trick` in `Dog.Trick`, or the
// module alias `Dog` before the dot. Kind "type" or "module".
bool type_name_at(const WorkerEnv& env, const Luau::SourceModule& source, const Luau::Module& module,
                  const std::string& module_name, const Luau::Position& at, LuauTypeAt& out) {
    Luau::AstTypeReference* reference = nullptr;
    for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(source, at, true)) {
        if (auto* found = node->as<Luau::AstTypeReference>()) {
            reference = found;
        }
    }
    if (reference == nullptr) {
        return false;
    }
    const Luau::ScopePtr scope = Luau::findScopeAtPosition(module, at);
    if (reference->prefix && reference->prefixLocation && reference->prefixLocation->containsClosed(at)) {
        out.found = true;
        out.name = reference->prefix->value;
        out.kind = "module";
        return true;
    }
    if (!reference->nameLocation.containsClosed(at)) {
        return false;
    }
    out.name = reference->name.value;
    out.kind = "type";
    out.found = true;
    std::optional<Luau::TypeFun> named;
    std::string declared_in = module_name;
    if (reference->prefix) {
        const std::string alias = reference->prefix->value;
        for (Luau::ScopePtr step = scope; step != nullptr && declared_in == module_name; step = step->parent) {
            const auto imported = step->importedModules.find(alias);
            if (imported != step->importedModules.end()) {
                declared_in = imported->second;
            }
        }
        named = scope != nullptr ? scope->lookupImportedType(alias, out.name) : std::nullopt;
        out.object_name = alias;
    } else if (scope != nullptr) {
        named = scope->lookupType(out.name);
    }
    out.described.type = type_declaration(env, declared_in, out.name);
    if (named) {
        out.class_name = registered_class(named->type);
        if (out.described.type.empty() && out.class_name.empty()) {
            out.described.type = shown_type(named->type);
        }
    }
    return true;
}

LuauTypeAt type_at_offset(const WorkerEnv& env, const Luau::SourceModule& source, const Luau::Module& module,
                          const std::string& text, std::size_t offset) {
    LuauTypeAt out;
    out.ran = true;
    const Luau::Position at = position_of(text, offset);
    if (env.files.module_name != nullptr && type_name_at(env, source, module, *env.files.module_name, at, out)) {
        return out;
    }
    Luau::ExprOrLocal found = Luau::findExprOrLocalAtPosition(source, at);
    std::optional<Luau::TypeId> type;
    bool with_self = false;
    // A key where a table is written: `{ Gold = 1 }` has Gold's value's type.
    for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(source, at)) {
        auto* table = node->as<Luau::AstExprTable>();
        if (table == nullptr) {
            continue;
        }
        for (const Luau::AstExprTable::Item& item : table->items) {
            auto* key = item.key != nullptr ? item.key->as<Luau::AstExprConstantString>() : nullptr;
            if (key == nullptr || !key->location.containsClosed(at)) {
                continue;
            }
            if (const Luau::TypeId* value = module.astTypes.find(item.value)) {
                out.name = std::string(key->value.data, key->value.size);
                out.kind = "field";
                type = *value;
            }
        }
    }
    if (type) {
        found = Luau::ExprOrLocal{};
    }
    // Whether a local is a parameter of a function around the position.
    const auto parameter = [&](Luau::AstLocal* local) {
        for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(source, at)) {
            if (auto* fn = node->as<Luau::AstExprFunction>()) {
                if (fn->self == local) {
                    return true;
                }
                for (Luau::AstLocal* arg : fn->args) {
                    if (arg == local) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    if (Luau::AstLocal* local = found.getLocal()) {
        out.name = local->name.value;
        out.kind = parameter(local) ? "parameter" : "local";
        type = local_type(module, local);
    } else if (Luau::AstExpr* expr = found.getExpr()) {
        if (const Luau::TypeId* known = module.astTypes.find(expr)) {
            type = *known;
        }
        if (auto* named = expr->as<Luau::AstExprLocal>()) {
            out.name = named->local->name.value;
            out.kind = parameter(named->local) ? "parameter" : "local";
            // A local being assigned has no expression type; it has its own.
            if (!type) {
                type = local_type(module, named->local);
            }
        } else if (auto* global = expr->as<Luau::AstExprGlobal>()) {
            out.name = global->name.value;
            out.kind = "global";
        } else if (auto* index = expr->as<Luau::AstExprIndexName>()) {
            out.name = index->index.value;
            out.kind = "member";
            with_self = index->op == ':';
            describe_object(module, index->expr, out.described.owner, out.object_instance_known, out.object_instance);
            if (auto* object_local = index->expr->as<Luau::AstExprLocal>()) {
                out.object_name = object_local->local->name.value;
            } else if (auto* object_global = index->expr->as<Luau::AstExprGlobal>()) {
                out.object_name = object_global->name.value;
            }
        } else {
            out.kind = "expression";
        }
    }
    if (!type) {
        return out;
    }
    out.found = true;
    out.described.name = out.name;
    if (out.kind == "local") {
        if (const std::optional<CallValue> held = call_value(env, source, module, found)) {
            if (!held->type) {
                out.described.type = *held->shown;
                return out;
            }
            type = *held->type;
        }
    }
    Luau::TypeId kept = *type;
    kept_function(env, kept);
    type = kept;
    out.described.type = shown_type(*type);
    // A table Luau named after the local that holds it prints as that name,
    // which says nothing. Its shape does.
    if (out.described.type == out.name) {
        Luau::ToStringOptions shape;
        shape.ignoreSyntheticName = true;
        out.described.type = Luau::toString(*type, shape);
    }
    out.class_name = registered_class(*type);
    if (const auto* own = Luau::get<Luau::ExternType>(Luau::follow(*type))) {
        out.raw_class = own->name;
    }
    const auto global_function = env.global_functions.find(Luau::follow(*type));
    if (global_function != env.global_functions.end()) {
        out.function_owner = global_function->second.first;
        out.function_name = global_function->second.second;
    }
    if (const std::optional<InstanceId> tagged = tagged_instance(*type)) {
        out.instance_known = true;
        out.instance = *tagged;
    }
    if (const auto* fn = Luau::get<Luau::FunctionType>(Luau::follow(*type))) {
        describe_function(&env, *fn, with_self, out.described);
    }
    return out;
}

// The globals a chunk defines: `function name()` and `name = value`.
struct DefinedGlobals : Luau::AstVisitor {
    std::unordered_set<std::string> names;

    bool visit(Luau::AstStatFunction* node) override {
        if (auto* global = node->name->as<Luau::AstExprGlobal>()) {
            names.insert(global->name.value);
        }
        return true;
    }

    bool visit(Luau::AstStatAssign* node) override {
        for (Luau::AstExpr* target : node->vars) {
            if (auto* global = target->as<Luau::AstExprGlobal>()) {
                names.insert(global->name.value);
            }
        }
        return true;
    }
};

// The completion at the caret, from a buffer already checked.
LuauCompletion completion_at(WorkerEnv& env, const std::string& module_name, const std::string& text,
                             std::size_t offset) {
    LuauCompletion out;
    const Luau::AutocompleteResult result = Luau::autocomplete(
        *env.frontend, module_name, position_of(text, offset),
        [](std::string, std::optional<const Luau::ExternType*>, std::optional<std::string>) {
            return std::optional<Luau::AutocompleteEntryMap>();
        });
    out.context = context_name(result.context);
    const Luau::ModulePtr module = env.frontend->moduleResolver.getModule(module_name);
    const Luau::Position at = position_of(text, offset);
    // After '.' or ':', the expression before it.
    for (auto node = result.ancestry.rbegin(); node != result.ancestry.rend() && module != nullptr; ++node) {
        if (auto* index = (*node)->as<Luau::AstExprIndexName>()) {
            std::string named;
            describe_object(*module, index->expr, named, out.receiver_instance_known, out.receiver_instance);
            if (const Luau::TypeId* type = module->astTypes.find(index->expr)) {
                out.receiver_class = registered_class(*type);
            }
            if (const Luau::TypeId* type = module->astTypes.find(index->expr)) {
                if (const auto* primitive = Luau::get<Luau::PrimitiveType>(Luau::follow(*type))) {
                    out.receiver_type = primitive->type == Luau::PrimitiveType::String ? "string" : "";
                } else if (Luau::get<Luau::SingletonType>(Luau::follow(*type)) != nullptr) {
                    out.receiver_type = Luau::toString(*type).rfind('"', 0) == 0 ? "string" : "";
                }
            }
            if (index->expr->is<Luau::AstExprGlobal>()) {
                out.receiver_global = index->expr->as<Luau::AstExprGlobal>()->name.value;
            }
            break;
        }
    }
    DefinedGlobals defined;
    if (const Luau::SourceModule* source = env.frontend->getSourceModule(module_name); source != nullptr && source->root) {
        source->root->visit(&defined);
    }
    // The locals in scope at the caret, and where each was declared. A local is
    // not in scope inside its own initializer.
    std::unordered_map<std::string, std::uint32_t> locals;
    std::unordered_map<std::string, std::string> written;
    std::unordered_set<const Luau::AstLocal*> declaring;
    for (Luau::AstNode* node : result.ancestry) {
        if (auto* stat = node->as<Luau::AstStatLocal>()) {
            for (Luau::AstLocal* var : stat->vars) {
                declaring.insert(var);
            }
        }
    }
    if (module != nullptr) {
        for (Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, at); scope != nullptr; scope = scope->parent) {
            for (const auto& [symbol, binding] : scope->bindings) {
                if (symbol.local == nullptr || locals.count(symbol.local->name.value) != 0 ||
                    declaring.count(symbol.local) != 0) {
                    continue;
                }
                const Luau::Location& where = symbol.local->location;
                if (where.begin > at) {
                    continue;
                }
                locals.emplace(symbol.local->name.value, where.begin.line * 65536u + where.begin.column);
                if (symbol.local->annotation != nullptr) {
                    written.emplace(symbol.local->name.value, text_at(text, symbol.local->annotation->location));
                }
            }
        }
    }
    // The module whose `type` statements name the types offered: another
    // module's after `Module.`, else this one.
    std::string types_module = module_name;
    for (Luau::AstNode* node : result.ancestry) {
        auto* reference = node->as<Luau::AstTypeReference>();
        if (reference == nullptr || !reference->prefix || module == nullptr) {
            continue;
        }
        const std::string alias = reference->prefix->value;
        for (Luau::ScopePtr step = Luau::findScopeAtPosition(*module, at); step != nullptr; step = step->parent) {
            const auto imported = step->importedModules.find(alias);
            if (imported != step->importedModules.end()) {
                types_module = imported->second;
                break;
            }
        }
    }
    std::vector<Luau::AstLocal*> repeat_locals;
    for (Luau::AstNode* node : result.ancestry) {
        auto* loop = node->as<Luau::AstStatRepeat>();
        if (loop == nullptr || loop->condition == nullptr || !loop->condition->location.containsClosed(at)) {
            continue;
        }
        for (Luau::AstStat* stat : loop->body->body) {
            if (auto* declared = stat->as<Luau::AstStatLocal>()) {
                for (Luau::AstLocal* var : declared->vars) {
                    repeat_locals.push_back(var);
                    locals[var->name.value] = var->location.begin.line * 65536u + var->location.begin.column;
                }
            } else if (auto* function = stat->as<Luau::AstStatLocalFunction>()) {
                repeat_locals.push_back(function->name);
                locals[function->name->name.value] =
                    function->name->location.begin.line * 65536u + function->name->location.begin.column;
            }
        }
    }
    for (const auto& [name, entry] : result.entryMap) {
        LuauSuggestion item;
        item.name = name;
        item.kind = kind_name(entry.kind);
        if (entry.type) {
            Luau::TypeId kept = *entry.type;
            const Luau::FunctionType* fn = kept_function(env, kept);
            item.type = shown_type(kept);
            if (fn != nullptr) {
                describe_function(&env, *fn, entry.indexedWithSelf && fn->hasSelf, item);
            }
        }
        item.call = entry.parens != Luau::ParenthesesRecommendation::None;
        item.wrong_index = entry.wrongIndexType;
        item.type_correct = entry.typeCorrect != Luau::TypeCorrectKind::None;
        if (entry.containingExternType && *entry.containingExternType != nullptr) {
            item.owner = registered_class(*entry.containingExternType);
        } else if (!out.receiver_global.empty()) {
            item.owner = out.receiver_global;
        }
        if (entry.type) {
            item.class_name = registered_class(*entry.type);
        }
        if (entry.kind == Luau::AutocompleteEntryKind::GeneratedFunction && entry.insertText) {
            item.insert = *entry.insertText;
        }
        if (entry.kind == Luau::AutocompleteEntryKind::Type) {
            item.declaration = type_declaration(env, types_module, name);
            if (item.declaration.rfind("export ", 0) == 0) {
                item.declaration.erase(0, 7);
            }
        }
        if (entry.kind == Luau::AutocompleteEntryKind::Binding) {
            // C++17 cannot capture a structured binding, so name it again.
            const bool own_initializer =
                std::any_of(declaring.begin(), declaring.end(), [&entry_name = name](const Luau::AstLocal* var) {
                    return entry_name == var->name.value;
                });
            if (own_initializer && locals.find(name) == locals.end() && defined.names.count(name) == 0) {
                continue;
            }
            const auto local = locals.find(name);
            if (local != locals.end()) {
                item.local = true;
                item.declared = local->second;
                const auto annotation = written.find(name);
                if (annotation != written.end()) {
                    item.written_type = annotation->second;
                }
            } else {
                item.defined_here = defined.names.count(name) != 0;
            }
        }
        out.items.push_back(std::move(item));
    }
    for (Luau::AstLocal* var : repeat_locals) {
        const std::string name = var->name.value;
        const bool listed = std::any_of(out.items.begin(), out.items.end(),
                                        [&](const LuauSuggestion& item) { return item.name == name && item.local; });
        if (listed || module == nullptr) {
            continue;
        }
        out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                       [&](const LuauSuggestion& item) { return item.name == name; }),
                        out.items.end());
        LuauSuggestion item;
        item.name = name;
        item.kind = "binding";
        item.local = true;
        item.declared = locals[name];
        if (const std::optional<Luau::TypeId> type = local_type(*module, var)) {
            item.type = shown_type(*type);
            item.class_name = registered_class(*type);
            if (const auto* fn = Luau::get<Luau::FunctionType>(Luau::follow(*type))) {
                describe_function(&env, *fn, false, item);
            }
        }
        out.items.push_back(std::move(item));
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const LuauSuggestion& a, const LuauSuggestion& b) { return a.name < b.name; });
    out.ran = true;
    return out;
}

LuauFacts facts_job(WorkerEnv& env, const CompleteRequest& request) {
    LuauFacts out;
    out.error = with_checked_buffer(env, request, [&](const std::string& module_name) {
        const Luau::SourceModule* source = env.frontend->getSourceModule(module_name);
        const Luau::ModulePtr module = env.frontend->moduleResolver.getModule(module_name);
        if (request.offset != std::string::npos) {
            out.completion = completion_at(env, module_name, request.source, request.offset);
        }
        for (std::size_t offset : request.offsets) {
            if (source != nullptr && module != nullptr) {
                out.types.push_back(type_at_offset(env, *source, *module, request.source, offset));
            } else {
                out.types.emplace_back();
            }
        }
        out.ran = true;
    });
    return out;
}

// One script in a batch: stages 1 and 2 of its check, and what the type check
// needs. prepare_script reads only the snapshot and the frozen globals, so a
// batch prepares its scripts in parallel.
struct CheckInput {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    std::string name;
    std::string source;
    std::string module_name;
    std::string check_source;
    Luau::Mode mode = Luau::Mode::Nonstrict;
    // False after a syntax error, under --!nocheck, or when analysis cannot run.
    bool type_check = false;
    // Preparing it threw, so it says it could not be checked.
    bool failed = false;
    std::vector<Diagnostic> diagnostics;
    std::vector<InstanceId> requires;
    // The names it requires by name, as `require("Util")`.
    std::vector<std::string> by_name;
};

void prepare_script(const WorkerEnv& env, const WorldSnap& world, CheckInput& input) {
    const NodeSnap* self = world.find(input.id);
    if (self == nullptr || !self->lua) {
        return;
    }
    input.name = shown_name(*self);
    input.source = self->source;
    input.module_name = module_name_of(input.id);

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions parse_options;
    parse_options.captureComments = true;
    const Luau::ParseResult parsed =
        Luau::Parser::parse(self->source.c_str(), self->source.size(), names, allocator, parse_options);

    // Stage 1. A parse failure stops the pipeline. The compiler is unchanged.
    if (!parsed.errors.empty()) {
        for (const Luau::ParseError& error : parsed.errors) {
            input.diagnostics.push_back(make_diagnostic(input.id, range_from(error.getLocation()), Severity::Error,
                                                        "Syntax", error.getMessage()));
        }
        if (parsed.root != nullptr) {
            input.requires = find_requires(world, input.id, parsed.root);
        }
        return;
    }

    // A script without a `--!` mode comment on its first lines is checked nonstrict.
    input.mode = Luau::parseMode(parsed.hotcomments).value_or(Luau::Mode::Nonstrict);
    if (parsed.root != nullptr) {
        input.requires = find_requires(world, input.id, parsed.root, &input.by_name);
    }
    if (input.mode == Luau::Mode::NoCheck) {
        return;
    }
    if (!env.init_error.empty() || env.frontend == nullptr || env.frontend->globals.globalScope == nullptr) {
        const std::string message = env.init_error.empty() ? "script analysis is unavailable" : env.init_error;
        input.diagnostics.push_back(make_diagnostic(input.id, TextRange{}, Severity::Error, "Analysis", message));
        return;
    }

    // Stage 2. Builtin lints. Unknown globals are left to the type checker.
    Luau::LintOptions lint_options;
    lint_options.setDefaults();
    lint_options.warningMask &= ~Luau::LintWarning::parseMask(parsed.hotcomments);
    lint_options.disableWarning(Luau::LintWarning::Code_UnknownGlobal);
    if (input.mode == Luau::Mode::Strict) {
        lint_options.disableWarning(Luau::LintWarning::Code_ImplicitReturn);
    }
    const std::vector<Luau::LintWarning> lints = Luau::lint(parsed.root, names, env.frontend->globals.globalScope,
                                                            env.untyped.get(), parsed.hotcomments, lint_options);
    for (const Luau::LintWarning& warning : lints) {
        Severity severity = Severity::Warning;
        if (warning.code == Luau::LintWarning::Code_LocalUnused ||
            warning.code == Luau::LintWarning::Code_FunctionUnused ||
            warning.code == Luau::LintWarning::Code_ImportUnused) {
            severity = Severity::Hint;
        }
        const char* name = Luau::LintWarning::getName(warning.code);
        input.diagnostics.push_back(make_diagnostic(input.id, range_from(warning.location), severity,
                                                    std::string("Lint/") + (name != nullptr ? name : "Unknown"),
                                                    warning.text));
    }
    input.check_source = checked_source(self->source, parsed, input.mode);
    input.type_check = true;
}

// Stage 3's diagnostics: the module's own type errors. The full checker runs for
// strict and nonstrict alike; nonstrict then reads type errors as warnings.
void add_type_errors(const Luau::Module& module, const CheckInput& input, Luau::FileResolver& files,
                     std::vector<Diagnostic>& out) {
    Luau::TypeErrorToStringOptions stringify;
    stringify.fileResolver = &files;
    for (const Luau::TypeError& error : module.errors) {
        // A required module reports its own problems when it is analyzed.
        if (error.moduleName != input.module_name) {
            continue;
        }
        if (Luau::get<Luau::SyntaxError>(error) != nullptr) {
            out.push_back(make_diagnostic(input.id, range_from(error.location), Severity::Error, "Syntax",
                                          Luau::toString(error, stringify)));
            continue;
        }
        if (const Luau::UnknownSymbol* symbol = Luau::get<Luau::UnknownSymbol>(error)) {
            if (symbol->context == Luau::UnknownSymbol::Binding) {
                out.push_back(make_diagnostic(input.id, range_from(error.location), Severity::Warning,
                                              "Lint/UnknownGlobal", Luau::toString(error, stringify)));
                continue;
            }
        }
        Severity severity = input.mode == Luau::Mode::Strict ? Severity::Error : Severity::Warning;
        if (missing_render_member(error)) {
            severity = Severity::Warning;
        }
        out.push_back(
            make_diagnostic(input.id, range_from(error.location), severity, "Type", Luau::toString(error, stringify)));
    }
}

// Every instance one of the module's expressions is typed as, directly or as an
// option of a union or intersection, as Door? is. Locals, parameters, script,
// and FindFirstChild all end up here, because this reads what Luau inferred.
// A call written last in an argument list or a return is typed as a pack, so
// the packs' values count too.
std::vector<InstanceId> reached_instances(const Luau::Module& module) {
    std::vector<InstanceId> out;
    std::unordered_set<InstanceId> seen;
    const auto take = [&](Luau::TypeId type) {
        if (const std::optional<InstanceId> id = tagged_instance(type)) {
            if (seen.insert(*id).second) {
                out.push_back(*id);
            }
        }
    };
    const auto take_all = [&](Luau::TypeId type) {
        type = Luau::follow(type);
        take(type);
        if (const Luau::UnionType* options = Luau::get<Luau::UnionType>(type)) {
            for (Luau::TypeId option : options->options) {
                take(option);
            }
        } else if (const Luau::IntersectionType* parts = Luau::get<Luau::IntersectionType>(type)) {
            for (Luau::TypeId part : parts->parts) {
                take(part);
            }
        }
    };
    for (const auto& entry : module.astTypes) {
        take_all(entry.second);
    }
    for (const auto& entry : module.astTypePacks) {
        for (Luau::TypeId type : Luau::flatten(entry.second).first) {
            take_all(type);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// `name` and every module that requires it, however deep, as far as the
// frontend knows. markDirty's own list stops at modules already dirty, so it
// cannot say this.
void add_dependents(const Luau::Frontend& frontend, const std::string& name, std::unordered_set<std::string>& out) {
    std::vector<std::string> stack{name};
    while (!stack.empty()) {
        std::string next = std::move(stack.back());
        stack.pop_back();
        if (!out.insert(next).second) {
            continue;
        }
        const auto node = frontend.sourceNodes.find(next);
        if (node == frontend.sourceNodes.end() || node->second == nullptr) {
            continue;
        }
        for (const std::string& dependent : node->second->dependents) {
            stack.push_back(dependent);
        }
    }
}

// The place checker's own state, kept between batches on its thread.
struct PlaceChecker {
    std::unique_ptr<WorkerEnv> env;
    // Each script's reached set from its last check. Written from pool threads.
    std::mutex reached_mu;
    std::unordered_map<InstanceId, std::vector<InstanceId>> reached;
    // Scripts whose last check failed, so it said nothing of what it reached.
    // Any tree change checks them again. Guarded by reached_mu.
    std::unordered_set<InstanceId> failed;
    // The authored tree its cached modules and place types were checked
    // against. A play tree never replaces it.
    std::shared_ptr<const WorldSnap> last_world;
    // Each type-checked script's `require("Name")` names, from its last
    // check. Which module a name finds depends on the whole place.
    std::unordered_map<InstanceId, std::vector<std::string>> by_name;
    // Scripts checked against a play tree. The next authored tree checks them again.
    std::unordered_set<InstanceId> play_checked;
};

// Back on the authored tree after a playtest: drops every cached module, and
// every record, of a script neither `world` nor the last authored tree has,
// such as a module a play script required. They were never in an authored
// tree, so no diff names them. A script the last authored tree has is left
// for the diff, which also rechecks what required it.
void forget_play_scripts(PlaceChecker& checker, const std::shared_ptr<const WorldSnap>& world) {
    const auto authored = [&](InstanceId id) {
        const NodeSnap* now = world->find(id);
        const NodeSnap* was = checker.last_world != nullptr ? checker.last_world->find(id) : nullptr;
        return (now != nullptr && now->lua) || (was != nullptr && was->lua);
    };
    std::vector<Luau::ModuleName> gone;
    for (const auto& cached : checker.env->frontend->sourceNodes) {
        const std::optional<InstanceId> id = instance_of_module(cached.first);
        if (!id || !authored(*id)) {
            gone.push_back(cached.first);
        }
    }
    checker.env->frontend->clearModules(gone);
    std::lock_guard<std::mutex> lock(checker.reached_mu);
    for (auto it = checker.reached.begin(); it != checker.reached.end();) {
        it = authored(it->first) ? std::next(it) : checker.reached.erase(it);
    }
    for (auto it = checker.by_name.begin(); it != checker.by_name.end();) {
        it = authored(it->first) ? std::next(it) : checker.by_name.erase(it);
    }
    for (auto it = checker.failed.begin(); it != checker.failed.end();) {
        it = authored(*it) ? std::next(it) : checker.failed.erase(it);
    }
}

// Brings the place checker to `world` and adds to `names` every module the
// change can affect: scripts added or edited, scripts whose last check reached
// an instance the diff names, scripts that require by a name the change adds,
// removes, or moves, and scripts whose last check failed. A script that left
// the place leaves the cache and goes in `removed`, and what required it is
// added. A play tree changes none of this: scripts checked against it are
// noted, and the next authored tree checks them again.
void sync_place(PlaceChecker& checker, const std::shared_ptr<const WorldSnap>& world,
                std::unordered_set<std::string>& names, std::vector<InstanceId>& removed) {
    WorkerEnv& env = *checker.env;
    // A place with no root says nothing about the tree.
    if (world->nodes.empty()) {
        return;
    }
    if (world->play) {
        // Something to check play scripts against until the authored tree comes back.
        if (env.place == nullptr) {
            env.place = build_place_types(*env.frontend->globals.globalScope, world);
        }
        return;
    }
    if (checker.last_world == world && env.place != nullptr && !env.place->world->play) {
        return;
    }
    if (!checker.play_checked.empty() || (env.place != nullptr && env.place->world->play)) {
        forget_play_scripts(checker, world);
    }
    for (InstanceId id : checker.play_checked) {
        names.insert(module_name_of(id));
    }
    checker.play_checked.clear();
    // The first tree, a new frontend after a registry change, or types last
    // built from a play tree: every script is checked against new types.
    // Scripts gone since the last authored tree are still dropped.
    if (checker.last_world == nullptr || env.place == nullptr || env.place->world->play) {
        if (checker.last_world != nullptr) {
            for (const NodeSnap& node : checker.last_world->nodes) {
                if (node.lua && world->find(node.id) == nullptr) {
                    removed.push_back(node.id);
                }
            }
            std::lock_guard<std::mutex> lock(checker.reached_mu);
            for (InstanceId id : removed) {
                checker.reached.erase(id);
                checker.failed.erase(id);
                checker.by_name.erase(id);
            }
        }
        env.place = build_place_types(*env.frontend->globals.globalScope, world);
        for (const NodeSnap& node : world->nodes) {
            if (node.lua) {
                names.insert(module_name_of(node.id));
            }
        }
        checker.last_world = world;
        return;
    }
    const TreeDiff diff = diff_worlds(*checker.last_world, *world);
    update_place_types(*env.place, *env.frontend->globals.globalScope, world, diff);
    for (InstanceId id : diff.added_scripts) {
        names.insert(module_name_of(id));
    }
    for (InstanceId id : diff.edited_scripts) {
        names.insert(module_name_of(id));
    }
    // Module names a by-name require may now find differently: a module added,
    // removed, renamed, or moved, by its old name and its new one.
    std::unordered_set<std::string> module_names;
    for (InstanceId id : diff.moved) {
        if (const NodeSnap* was = checker.last_world->find(id)) {
            if (was->module) {
                module_names.insert(was->name);
            }
        }
        if (const NodeSnap* now = world->find(id)) {
            if (now->module) {
                module_names.insert(now->name);
            }
        }
    }
    for (InstanceId id : diff.added_scripts) {
        if (const NodeSnap* now = world->find(id)) {
            if (now->module) {
                module_names.insert(now->name);
            }
        }
    }
    // A tree change, not an edit: the edited script itself is checked anyway,
    // and an edit elsewhere cannot fix what made a check fail.
    const bool changed = !diff.parents.empty() || !diff.moved.empty() || !diff.added_scripts.empty() ||
                         !diff.removed_scripts.empty();
    {
        std::lock_guard<std::mutex> lock(checker.reached_mu);
        for (InstanceId id : diff.removed_scripts) {
            checker.reached.erase(id);
            checker.failed.erase(id);
            checker.by_name.erase(id);
        }
        for (const auto& entry : checker.reached) {
            for (InstanceId id : entry.second) {
                if (diff.parents.count(id) != 0 || diff.moved.count(id) != 0) {
                    names.insert(module_name_of(entry.first));
                    break;
                }
            }
        }
        if (changed) {
            for (InstanceId id : checker.failed) {
                names.insert(module_name_of(id));
            }
        }
    }
    for (const auto& entry : checker.by_name) {
        // A script that moved prefers modules beside its new parent.
        bool affected = diff.moved.count(entry.first) != 0;
        for (const std::string& name : entry.second) {
            affected = affected || module_names.count(name) != 0;
        }
        if (affected) {
            names.insert(module_name_of(entry.first));
        }
    }
    std::vector<Luau::ModuleName> gone;
    for (InstanceId id : diff.removed_scripts) {
        const std::string name = module_name_of(id);
        std::unordered_set<std::string> dependents;
        add_dependents(*env.frontend, name, dependents);
        dependents.erase(name);
        names.insert(dependents.begin(), dependents.end());
        gone.push_back(name);
        removed.push_back(id);
    }
    for (const Luau::ModuleName& name : gone) {
        names.erase(name);
    }
    env.frontend->clearModules(gone);
    checker.last_world = world;
}

// The scripts a batch checks, each with its generation when the batch took it.
using Claimed = std::unordered_map<InstanceId, std::uint64_t>;

struct BatchHost {
    // Takes a script into the batch and returns its generation now, or nothing
    // when it is waiting for its own batch with a newer source.
    std::function<std::optional<std::uint64_t>(InstanceId)> claim;
    // Hands one script's result to pump(). Any thread.
    std::function<void(Finished)> publish;
    // Tells pump() these scripts left the place, so what they published goes.
    std::function<void(const std::vector<InstanceId>&)> drop;
};

Finished finished_from(const CheckInput& input) {
    Finished finished;
    finished.id = input.id;
    finished.generation = input.generation;
    finished.name = input.name;
    finished.source = input.source;
    finished.diagnostics = input.diagnostics;
    finished.requires = input.requires;
    return finished;
}

// Publishes `input` with an Analysis diagnostic saying why it was not checked.
// Never throws: it runs where an exception would stop a thread.
void publish_failure(const BatchHost& host, const CheckInput* input, const char* failure) {
    if (input == nullptr) {
        return;
    }
    try {
        Finished finished = finished_from(*input);
        finished.diagnostics.push_back(
            make_diagnostic(input->id, TextRange{}, Severity::Error, "Analysis", std::string("could not be checked: ") + failure));
        host.publish(std::move(finished));
    } catch (...) {
    }
}

void run_batch(PlaceChecker& checker, AnalysisPool& pool, const std::shared_ptr<const WorldSnap>& world,
               Claimed& claimed, const std::shared_ptr<Luau::FrontendCancellationToken>& cancel,
               const BatchHost& host) {
    WorkerEnv& env = *checker.env;
    // Every module the batch changes: the scripts taken, those the tree change
    // reaches, and what requires each of them.
    std::unordered_set<std::string> names;
    std::vector<InstanceId> removed;
    sync_place(checker, world, names, removed);
    if (!removed.empty()) {
        host.drop(removed);
    }
    for (const auto& entry : claimed) {
        names.insert(module_name_of(entry.first));
    }
    std::unordered_set<std::string> affected;
    for (const std::string& name : names) {
        add_dependents(*env.frontend, name, affected);
    }
    std::vector<CheckInput> inputs;
    inputs.reserve(affected.size());
    for (const std::string& name : affected) {
        const std::optional<InstanceId> id = instance_of_module(name);
        const NodeSnap* node = id ? world->find(*id) : nullptr;
        if (node == nullptr || !node->lua) {
            continue;
        }
        if (claimed.count(*id) == 0) {
            const std::optional<std::uint64_t> generation = host.claim(*id);
            if (!generation) {
                continue;
            }
            claimed.emplace(*id, *generation);
        }
        CheckInput input;
        input.id = *id;
        input.generation = claimed.at(*id);
        inputs.push_back(std::move(input));
        env.frontend->markDirty(name);
    }

    std::vector<std::function<void()>> prepare;
    prepare.reserve(inputs.size());
    for (CheckInput& input : inputs) {
        // A pool task must not throw: run_all would wait for it forever.
        prepare.push_back([&env, &world, &input] {
            try {
                prepare_script(env, *world, input);
            } catch (...) {
                input.type_check = false;
                input.failed = true;
                input.diagnostics.push_back(
                    make_diagnostic(input.id, TextRange{}, Severity::Error, "Analysis", "analysis failed"));
            }
        });
    }
    pool.run_all(std::move(prepare));
    // The authored tree after a playtest checks these again.
    if (world->play) {
        for (const CheckInput& input : inputs) {
            checker.play_checked.insert(input.id);
        }
    }
    // Any failure below marks its script, so the next tree change retries it.
    const auto note_failed = [&checker](InstanceId id) {
        try {
            std::lock_guard<std::mutex> lock(checker.reached_mu);
            checker.failed.insert(id);
        } catch (...) {
        }
    };

    std::unordered_map<std::string, std::string> sources;
    std::unordered_map<InstanceId, const CheckInput*> by_id;
    std::vector<Luau::ModuleName> queue;
    for (const CheckInput& input : inputs) {
        by_id.emplace(input.id, &input);
        if (!input.type_check) {
            // Its result reads no types, so no tree change can alter it.
            {
                std::lock_guard<std::mutex> lock(checker.reached_mu);
                checker.reached.erase(input.id);
                if (input.failed) {
                    checker.failed.insert(input.id);
                } else {
                    checker.failed.erase(input.id);
                }
            }
            checker.by_name.erase(input.id);
            host.publish(finished_from(input));
            continue;
        }
        if (input.by_name.empty()) {
            checker.by_name.erase(input.id);
        } else {
            checker.by_name[input.id] = input.by_name;
        }
        sources.emplace(input.module_name, input.check_source);
        queue.push_back(input.module_name);
    }
    if (queue.empty() || cancel->requested()) {
        return;
    }

    std::mutex done_mu;
    std::unordered_set<InstanceId> done;
    // The env outlives this batch, so it must not keep pointers to its locals,
    // even when something below throws.
    struct ResetEnv {
        WorkerEnv& env;
        ~ResetEnv() {
            env.files.batch_sources = nullptr;
            env.files.world = nullptr;
            env.world = nullptr;
        }
    } reset_env{env};
    env.files.batch_sources = &sources;
    env.files.world = world.get();
    // A script's own --!nonstrict header was rewritten to --!strict, and one
    // without a header is checked by the full checker too.
    env.configs.config.mode = Luau::Mode::Strict;
    env.world = world.get();
    Luau::FrontendOptions options;
    options.runLintChecks = false;
    options.retainFullTypeGraphs = false;
    options.cancellationToken = cancel;
    // Runs on a pool thread as each module finishes, before Luau drops its
    // expression types, so its reached set can still be read.
    // Luau catches only its own internal errors around this, so it never throws.
    options.customModuleCheck = [&](const Luau::SourceModule& source, const Luau::Module& module) {
        const CheckInput* input = nullptr;
        try {
            const std::optional<InstanceId> id = instance_of_module(source.name);
            const auto found = id ? by_id.find(*id) : by_id.end();
            if (found == by_id.end() || !found->second->type_check || module.cancelled) {
                return;
            }
            input = found->second;
            {
                // After an internal error, checking a requirer again checks
                // modules that already finished. Each publishes once.
                std::lock_guard<std::mutex> lock(done_mu);
                if (!done.insert(input->id).second) {
                    return;
                }
            }
            Finished finished = finished_from(*input);
            add_type_errors(module, *input, env.files, finished.diagnostics);
            finished.reached = reached_instances(module);
            {
                std::lock_guard<std::mutex> lock(checker.reached_mu);
                checker.reached[input->id] = finished.reached;
                checker.failed.erase(input->id);
            }
            host.publish(std::move(finished));
        } catch (const std::exception& error) {
            if (input != nullptr) {
                note_failed(input->id);
            }
            publish_failure(host, input, error.what());
        } catch (...) {
            if (input != nullptr) {
                note_failed(input->id);
            }
            publish_failure(host, input, "analysis failed");
        }
    };
    try {
        env.frontend->queueModuleCheck(queue);
        env.frontend->checkQueuedModules(
            options, [&pool](std::vector<std::function<void()>> tasks) { pool.post(std::move(tasks)); });
    } catch (...) {
        // One module's internal error stops Luau's whole batch. What did not
        // finish is checked one at a time, so only the broken one says so.
        for (const std::string& name : queue) {
            if (cancel->requested()) {
                break;
            }
            const InstanceId id = *instance_of_module(name);
            {
                std::lock_guard<std::mutex> lock(done_mu);
                if (done.count(id) != 0) {
                    continue;
                }
            }
            std::string failure;
            try {
                env.frontend->check(name, options);
                continue;
            } catch (const std::exception& error) {
                failure = error.what();
            } catch (...) {
                failure = "analysis failed";
            }
            if (cancel->requested()) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(done_mu);
                if (!done.insert(id).second) {
                    continue;
                }
            }
            note_failed(id);
            publish_failure(host, by_id.at(id), failure.c_str());
        }
    }
}

}  // namespace

void lint_rule_names(std::vector<std::string>& out) {
    out.clear();
    for (int code = static_cast<int>(Luau::LintWarning::Code_Unknown) + 1;
         code < static_cast<int>(Luau::LintWarning::Code__Count); ++code) {
        const char* name = Luau::LintWarning::getName(static_cast<Luau::LintWarning::Code>(code));
        if (name == nullptr || name[0] == '\0') {
            continue;
        }
        out.emplace_back(name);
    }
}

struct ScriptAnalysis::State {
    std::mutex mu;
    // Set by note_world_changed. pump() turns it into one capture of the tree.
    std::atomic<bool> world_stale{false};
    // A script was queued, and pump() has not captured the tree since. pump()
    // takes one capture however many came in. True at first, so the first
    // pump() captures the place even when nothing is queued yet.
    std::atomic<bool> capture_needed{true};
    // A tree was captured while the simulation ran. Stop restores the
    // authored tree without a note, so once stopped pump() takes it as a tree
    // change.
    std::atomic<bool> play_stale{false};
    std::condition_variable cv;
    std::mutex start_mu;
    bool stop = false;
    bool enabled = true;
    bool started = false;
    std::uint64_t next_token = 0;
    // The place checker's thread, and the editor checker's, which answers Luau
    // requests so typing never waits behind a check of the place.
    StackThread place;
    StackThread editor;
    // Wakes the editor thread for completions. `cv` wakes the place thread.
    std::condition_variable editor_cv;

    struct Handler {
        std::uint64_t token = 0;
        bool live = true;
        std::function<void(InstanceId)> fn;
    };
    std::vector<Handler> handlers;
    std::unordered_map<InstanceId, std::uint64_t> generations;
    // Written by the worker after each job.
    std::atomic<std::size_t> cached_modules{0};
    std::unordered_map<InstanceId, Pending> pending;
    std::vector<Finished> results;
    struct Record {
        std::string name;
        std::string source;
        std::vector<Diagnostic> diagnostics;
        std::vector<InstanceId> reached;
    };
    std::unordered_map<InstanceId, Record> published;
    std::unordered_map<InstanceId, std::unordered_set<InstanceId>> requires_of;
    std::unordered_map<InstanceId, std::unordered_set<InstanceId>> required_by;

    // Luau autocomplete requests, answered before queued checks, and the one
    // the worker is answering now.
    std::deque<std::shared_ptr<CompleteRequest>> completions;
    std::shared_ptr<CompleteRequest> serving;

    // The newest tree captured, and its capture number: two captures can finish
    // out of order, and the newer one wins. `authored_world` is the newest one
    // captured while the simulation was stopped.
    std::shared_ptr<const WorldSnap> latest_world;
    std::uint64_t latest_seq = 0;
    std::shared_ptr<const WorldSnap> authored_world;
    std::uint64_t authored_seq = 0;
    std::atomic<std::uint64_t> world_seq{0};
    // Scripts in the batch the place checker is running, including what
    // requires them, and the batch's cancel.
    std::unordered_set<InstanceId> in_batch;
    std::shared_ptr<Luau::FrontendCancellationToken> batch_cancel;
    // A tree change the place checker has not taken yet, and when it is due.
    bool tree_pending = false;
    std::chrono::steady_clock::time_point tree_due{};
    // The running batch took a tree change, so it may yet take any script.
    bool tree_in_batch = false;
    // Results pump() published, per script.
    std::unordered_map<InstanceId, std::uint64_t> checks;
    // Scripts remove() dropped and nothing has scheduled since. A batch's tree
    // can be older than the removal, so claim() refuses these. The generation
    // is kept, never reset, so an id undo brings back cannot match a result
    // checked before it was removed.
    std::unordered_set<InstanceId> removed;

    void adopt(const std::shared_ptr<const WorldSnap>& world, std::uint64_t seq) {
        if (seq > latest_seq) {
            latest_world = world;
            latest_seq = seq;
        }
        if (!world->play && seq > authored_seq) {
            authored_world = world;
            authored_seq = seq;
        }
    }
};

void ScriptAnalysis::replace_requires(InstanceId script, const std::vector<InstanceId>& targets) {
    std::unordered_set<InstanceId>& current = state_->requires_of[script];
    for (InstanceId target : current) {
        const auto found = state_->required_by.find(target);
        if (found != state_->required_by.end()) {
            found->second.erase(script);
        }
    }
    current.clear();
    for (InstanceId target : targets) {
        if (target == script) {
            continue;
        }
        current.insert(target);
        state_->required_by[target].insert(script);
    }
}

void ScriptAnalysis::forget_requires(InstanceId script) {
    const auto own = state_->requires_of.find(script);
    if (own != state_->requires_of.end()) {
        for (InstanceId target : own->second) {
            const auto found = state_->required_by.find(target);
            if (found != state_->required_by.end()) {
                found->second.erase(script);
            }
        }
        state_->requires_of.erase(own);
    }
    const auto dependents = state_->required_by.find(script);
    if (dependents == state_->required_by.end()) {
        return;
    }
    for (InstanceId dependent : dependents->second) {
        const auto edge = state_->requires_of.find(dependent);
        if (edge != state_->requires_of.end()) {
            edge->second.erase(script);
        }
    }
    state_->required_by.erase(dependents);
}

void ScriptAnalysis::collect_dependents(InstanceId id, std::vector<InstanceId>& out,
                                        std::unordered_set<InstanceId>& seen) const {
    if (!seen.insert(id).second) {
        return;
    }
    out.push_back(id);
    const auto found = state_->required_by.find(id);
    if (found == state_->required_by.end()) {
        return;
    }
    const std::vector<InstanceId> dependents(found->second.begin(), found->second.end());
    for (InstanceId dependent : dependents) {
        collect_dependents(dependent, out, seen);
    }
}

ScriptAnalysis::ScriptAnalysis(DataModel& game, unsigned threads)
    : game_(game), state_(std::make_unique<State>()), threads_(threads == 0 ? AnalysisPool::default_size() : threads) {
    signal_.owner_ = this;
    game_.set_script_analysis(this);
}

ScriptAnalysis::~ScriptAnalysis() { shutdown(); }

void ScriptAnalysis::shutdown() {
    if (game_.script_analysis() == this) {
        game_.set_script_analysis(nullptr);
    }
    std::lock_guard<std::mutex> start(state_->start_mu);
    std::deque<std::shared_ptr<CompleteRequest>> stopped;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->stop = true;
        state_->enabled = false;
        state_->pending.clear();
        state_->tree_pending = false;
        if (state_->batch_cancel) {
            state_->batch_cancel->cancel();
        }
        state_->results.clear();
        state_->handlers.clear();
        // Luau requests still queued are answered with nothing, and the one
        // running stops, so neither a waiter nor the join waits on a check.
        if (state_->serving) {
            state_->serving->cancel->cancel();
        }
        stopped = std::move(state_->completions);
        state_->completions.clear();
        state_->cv.notify_all();
        state_->editor_cv.notify_all();
    }
    for (const std::shared_ptr<CompleteRequest>& request : stopped) {
        request->answer->facts.error = "script analysis has stopped";
        finish_request(*request);
    }
    if (state_->place.joinable()) {
        state_->place.join();
    }
    if (state_->editor.joinable()) {
        state_->editor.join();
    }
}

// Luau's parser allows 1000 levels of nesting and its checker hundreds more;
// a Debug build spends a few KB of stack on each. std::thread's default of 1 MB
// overflows on code nested that deep, so the worker reserves this much.
constexpr std::size_t kWorkerStackBytes = std::size_t{16} << 20;

void ScriptAnalysis::ensure_threads() {
    std::lock_guard<std::mutex> start(state_->start_mu);
    if (state_->started) {
        return;
    }
    state_->started = true;
    state_->place = StackThread(kWorkerStackBytes, [this] { run_place(); });
    state_->editor = StackThread(kWorkerStackBytes, [this] { run_editor(); });
}

void ScriptAnalysis::run_editor() {
    // Its own frontend: an unsaved buffer never reaches the place checker's cache.
    auto owned = std::make_unique<WorkerEnv>();
    owned->init();
    while (true) {
        std::shared_ptr<CompleteRequest> request;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->editor_cv.wait(lock, [&] { return state_->stop || !state_->completions.empty(); });
            if (state_->stop) {
                return;
            }
            request = std::move(state_->completions.front());
            state_->completions.pop_front();
            state_->serving = request;
        }
        if (owned->revision != lua_registry_revision()) {
            owned = std::make_unique<WorkerEnv>();
            owned->init();
        }
        request->answer->facts = facts_job(*owned, *request);
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->serving.reset();
        }
        finish_request(*request);
    }
}

void ScriptAnalysis::run_place() {
    // This thread and the pool never call the play VM and never take the
    // DataModel lock. They read snapshots captured on the gameplay thread.
    PlaceChecker checker;
    checker.env = std::make_unique<WorkerEnv>();
    checker.env->init();
    AnalysisPool pool(threads_, kWorkerStackBytes);
    BatchHost host;
    host.claim = [this](InstanceId id) -> std::optional<std::uint64_t> {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->pending.count(id) != 0 || state_->removed.count(id) != 0) {
            return std::nullopt;
        }
        state_->in_batch.insert(id);
        return state_->generations[id];
    };
    host.publish = [this](Finished finished) {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->stop || !state_->enabled) {
            return;
        }
        const auto generation = state_->generations.find(finished.id);
        if (generation == state_->generations.end() || generation->second != finished.generation) {
            return;
        }
        state_->results.push_back(std::move(finished));
    };
    host.drop = [this](const std::vector<InstanceId>& ids) {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->stop || !state_->enabled) {
            return;
        }
        for (InstanceId id : ids) {
            // At the script's generation now, so pump() keeps it in order with
            // its results: a check after it, once the script is back, wins.
            Finished finished;
            finished.id = id;
            finished.generation = state_->generations[id];
            finished.dropped = true;
            state_->results.push_back(std::move(finished));
        }
    };
    while (true) {
        Claimed claimed;
        bool tree = false;
        std::shared_ptr<const WorldSnap> world;
        std::shared_ptr<Luau::FrontendCancellationToken> cancel;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->cv.wait(lock,
                            [&] { return state_->stop || !state_->pending.empty() || state_->tree_pending; });
            if (state_->stop) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            auto next = std::chrono::steady_clock::time_point::max();
            for (auto it = state_->pending.begin(); it != state_->pending.end();) {
                if (it->second.ready_at > now) {
                    next = std::min(next, it->second.ready_at);
                    ++it;
                } else if (state_->latest_seq <= it->second.after) {
                    // Waiting for pump() to capture the tree it changed. The
                    // capture wakes this thread.
                    ++it;
                } else {
                    claimed.emplace(it->first, it->second.generation);
                    state_->in_batch.insert(it->first);
                    it = state_->pending.erase(it);
                }
            }
            if (state_->tree_pending) {
                if (state_->tree_due <= now) {
                    tree = true;
                    state_->tree_pending = false;
                } else {
                    next = std::min(next, state_->tree_due);
                }
            }
            if (claimed.empty() && !tree) {
                if (next == std::chrono::steady_clock::time_point::max()) {
                    state_->cv.wait(lock);
                } else {
                    state_->cv.wait_until(lock, next);
                }
                continue;
            }
            state_->tree_in_batch = tree;
            world = state_->latest_world;
            cancel = std::make_shared<Luau::FrontendCancellationToken>();
            state_->batch_cancel = cancel;
        }
        // An exception escaping here would end this thread with the batch's
        // scripts still in in_batch, so idle() would never be true again.
        const char* failure = nullptr;
        std::string failure_text;
        try {
            if (checker.env->revision != lua_registry_revision()) {
                // The new frontend has no place types, so sync_place checks the
                // whole place once. last_world stays only so the scripts gone
                // since it are still dropped.
                checker.env = std::make_unique<WorkerEnv>();
                checker.env->init();
                std::lock_guard<std::mutex> lock(checker.reached_mu);
                checker.reached.clear();
            }
            if (world != nullptr) {
                run_batch(checker, pool, world, claimed, cancel, host);
            }
        } catch (const std::exception& error) {
            failure = "analysis failed";
            try {
                failure_text = error.what();
                failure = failure_text.c_str();
            } catch (...) {
            }
        } catch (...) {
            failure = "analysis failed";
        }
        if (failure != nullptr) {
            // Each script the batch took says it could not be checked.
            for (const auto& entry : claimed) {
                try {
                    {
                        std::lock_guard<std::mutex> lock(checker.reached_mu);
                        checker.failed.insert(entry.first);
                    }
                    CheckInput input;
                    input.id = entry.first;
                    input.generation = entry.second;
                    if (const NodeSnap* node = world != nullptr ? world->find(entry.first) : nullptr) {
                        input.name = shown_name(*node);
                        input.source = node->source;
                    }
                    publish_failure(host, &input, failure);
                } catch (...) {
                }
            }
        }
        if (checker.env != nullptr && checker.env->frontend != nullptr) {
            state_->cached_modules.store(checker.env->frontend->sourceNodes.size(), std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lock(state_->mu);
        for (const auto& entry : claimed) {
            state_->in_batch.erase(entry.first);
        }
        state_->tree_in_batch = false;
        if (state_->batch_cancel == cancel) {
            state_->batch_cancel.reset();
        }
    }
}

struct ScriptAnalysis::LuauRequest : CompleteRequest {};

std::shared_ptr<ScriptAnalysis::LuauRequest> ScriptAnalysis::queue_luau(const std::vector<LuaNode>& world,
                                                                        InstanceId script, std::string source,
                                                                        std::size_t caret, const char* lane,
                                                                        std::vector<std::size_t> offsets) {
    auto request = std::make_shared<LuauRequest>();
    request->offsets = std::move(offsets);
    request->lane = lane != nullptr ? lane : "";
    request->world = world_from_nodes(world);
    // The command line has no script. Its buffer gets an id no instance has.
    request->script = script != 0 ? script : 0xfffffff0u;
    request->source = std::move(source);
    request->offset = caret;
    std::vector<std::shared_ptr<CompleteRequest>> replaced;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        // Analysis turned off runs no type checks, completion's included.
        if (state_->stop || !state_->enabled) {
            return nullptr;
        }
        if (!request->lane.empty()) {
            auto& queue = state_->completions;
            for (auto it = queue.begin(); it != queue.end();) {
                if ((*it)->lane == request->lane) {
                    replaced.push_back(std::move(*it));
                    it = queue.erase(it);
                } else {
                    ++it;
                }
            }
        }
        state_->completions.push_back(request);
        state_->editor_cv.notify_all();
    }
    for (const std::shared_ptr<CompleteRequest>& old : replaced) {
        old->answer->facts.error = "replaced by a newer request";
        finish_request(*old);
    }
    ensure_threads();
    return request;
}

LuauFacts ScriptAnalysis::luau_facts(const std::vector<LuaNode>& world, InstanceId script, std::string source,
                                     std::size_t caret, std::vector<std::size_t> offsets,
                                     std::chrono::milliseconds wait) {
    const std::shared_ptr<LuauRequest> request =
        queue_luau(world, script, std::move(source), caret, "", std::move(offsets));
    LuauFacts none;
    if (!request) {
        none.error = "script analysis is off or has stopped";
        return none;
    }
    std::unique_lock<std::mutex> lock(request->mu);
    if (!request->cv.wait_for(lock, wait, [&] { return request->done; })) {
        none.error = "timed out";
        return none;
    }
    return request->answer->facts;
}

std::shared_ptr<const LuauAnswer> ScriptAnalysis::luau_facts_later(const std::vector<LuaNode>& world,
                                                                   InstanceId script, std::string source,
                                                                   std::size_t caret, std::vector<std::size_t> offsets,
                                                                   const char* lane) {
    const std::shared_ptr<LuauRequest> request =
        queue_luau(world, script, std::move(source), caret, lane, std::move(offsets));
    return request ? request->answer : nullptr;
}

void ScriptAnalysis::set_enabled(bool enabled) {
    std::vector<InstanceId> cleared;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->enabled == enabled) {
            return;
        }
        state_->enabled = enabled;
        changed = true;
        if (!enabled) {
            state_->pending.clear();
            state_->tree_pending = false;
            if (state_->batch_cancel) {
                state_->batch_cancel->cancel();
            }
            state_->results.clear();
            for (auto& entry : state_->generations) {
                ++entry.second;
            }
            for (const auto& entry : state_->published) {
                cleared.push_back(entry.first);
            }
            state_->published.clear();
        }
    }
    if (!changed) {
        return;
    }
    if (!enabled) {
        fire(cleared);
        return;
    }
    invalidate_all();
}

bool ScriptAnalysis::enabled() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->enabled;
}

namespace {

// A Script or ModuleScript under the root. Only these are checked: an instance
// outside the place, such as one a paste builds before adding it, does not run.
// Id 0 is the root DataModel.
bool placed_script(const DataModel& game, InstanceId id) {
    if (dynamic_cast<const LuaSource*>(game.instance(id)) == nullptr) {
        return false;
    }
    for (InstanceId at = id; at != 0; at = game.parent(at)) {
        if (at == DataModel::kNoParent) {
            return false;
        }
    }
    return true;
}

}  // namespace

void ScriptAnalysis::capture_tree(bool tree_changed) {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            // Nothing will check it. busy() must not wait for it.
            state_->capture_needed.store(false, std::memory_order_relaxed);
            if (tree_changed) {
                state_->world_stale.store(false, std::memory_order_relaxed);
            }
            return;
        }
    }
    // Every script queued before here is in this capture. One queued after
    // sets the flag again and waits for the next.
    state_->capture_needed.store(false, std::memory_order_relaxed);
    const std::uint64_t seq = state_->world_seq.fetch_add(1) + 1;
    const std::shared_ptr<WorldSnap> world = capture_world(game_);
    // Play can start after pump() looks and before the capture takes the lock.
    // A play tree is not the authored one, so once stopped pump() diffs the
    // authored tree again.
    if (world->play) {
        state_->play_stale.store(true, std::memory_order_relaxed);
    }
    ensure_threads();
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->enabled || state_->stop) {
        return;
    }
    state_->adopt(world, seq);
    // Cleared only now, with tree_pending set, so settled() never sees neither.
    // The DataModel lock is held, so no tree change came in since pump() looked.
    if (tree_changed) {
        state_->world_stale.store(false, std::memory_order_relaxed);
        state_->tree_pending = true;
        state_->tree_due = std::chrono::steady_clock::now() + kDebounce;
        // A script remove() dropped that is in the tree again, as Stop brings
        // back one a playtest destroyed, is checked as new.
        for (auto it = state_->removed.begin(); it != state_->removed.end();) {
            const NodeSnap* node = world->find(*it);
            if (node == nullptr || !node->lua) {
                ++it;
                continue;
            }
            Pending pending;
            pending.generation = ++state_->generations[*it];
            pending.ready_at = state_->tree_due;
            pending.after = seq - 1;
            state_->pending[*it] = pending;
            it = state_->removed.erase(it);
        }
    }
    if (state_->authored_world == world) {
        // A rename the place checker does not recheck still shows in the report.
        for (auto& entry : state_->published) {
            if (const NodeSnap* node = world->find(entry.first)) {
                entry.second.name = shown_name(*node);
            }
        }
    }
    state_->cv.notify_all();
}

void ScriptAnalysis::schedule(const std::vector<InstanceId>& ids) {
    if (ids.empty()) {
        return;
    }
    std::vector<InstanceId> placed;
    placed.reserve(ids.size());
    for (InstanceId id : ids) {
        if (placed_script(game_, id)) {
            placed.push_back(id);
        }
    }
    if (placed.empty()) {
        return;
    }
    const bool playing = game_.simulation_running();
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
        const auto ready_at = std::chrono::steady_clock::now() + kDebounce;
        const std::uint64_t after = state_->world_seq.load();
        const WorldSnap* authored = state_->authored_world.get();
        for (InstanceId id : placed) {
            // A script that exists only in the play tree, such as a clone, is
            // never checked: Stop destroys it.
            if (playing && authored != nullptr) {
                const NodeSnap* node = authored->find(id);
                if (node == nullptr || !node->lua) {
                    continue;
                }
            }
            Pending pending;
            pending.generation = ++state_->generations[id];
            pending.ready_at = ready_at;
            pending.after = after;
            state_->removed.erase(id);
            state_->pending[id] = pending;
        }
    }
    state_->capture_needed.store(true, std::memory_order_relaxed);
    ensure_threads();
}

void ScriptAnalysis::invalidate(InstanceId script) {
    if (!placed_script(game_, script)) {
        return;
    }
    std::vector<InstanceId> chain;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
        std::unordered_set<InstanceId> seen;
        collect_dependents(script, chain, seen);
    }
    schedule(chain);
}

void ScriptAnalysis::invalidate_all() {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
    }
    std::vector<InstanceId> ids;
    game_.for_each_instance([&ids](DataModel& object) {
        if (dynamic_cast<LuaSource*>(&object) != nullptr) {
            ids.push_back(object.id());
        }
    });
    schedule(ids);
}

std::size_t ScriptAnalysis::cached_modules() const {
    return state_->cached_modules.load(std::memory_order_relaxed);
}

void ScriptAnalysis::remove(InstanceId script) {
    std::vector<InstanceId> dependents;
    bool notify = false;
    bool enabled = false;
    // Stop may bring it back without a note of its own. The tree diffed after
    // Stop checks it again then.
    if (game_.simulation_running()) {
        state_->play_stale.store(true, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        enabled = state_->enabled;
        std::uint64_t& generation = state_->generations[script];
        ++generation;
        state_->removed.insert(script);
        state_->pending.erase(script);
        state_->results.erase(std::remove_if(state_->results.begin(), state_->results.end(),
                                             [&](const Finished& finished) { return finished.id == script; }),
                              state_->results.end());
        notify = state_->published.erase(script) > 0;
        const auto found = state_->required_by.find(script);
        if (found != state_->required_by.end()) {
            dependents.assign(found->second.begin(), found->second.end());
        }
        forget_requires(script);
    }
    if (notify) {
        fire({script});
    }
    if (!enabled) {
        return;
    }
    for (InstanceId dependent : dependents) {
        invalidate(dependent);
    }
}

std::vector<Diagnostic> ScriptAnalysis::diagnostics() const {
    std::vector<Diagnostic> all;
    std::lock_guard<std::mutex> lock(state_->mu);
    for (const auto& entry : state_->published) {
        all.insert(all.end(), entry.second.diagnostics.begin(), entry.second.diagnostics.end());
    }
    std::sort(all.begin(), all.end(), [](const Diagnostic& left, const Diagnostic& right) {
        if (left.script != right.script) {
            return left.script < right.script;
        }
        if (left.range.start.line != right.range.start.line) {
            return left.range.start.line < right.range.start.line;
        }
        return left.range.start.character < right.range.start.character;
    });
    return all;
}

std::vector<Diagnostic> ScriptAnalysis::diagnostics(InstanceId script) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto found = state_->published.find(script);
    if (found == state_->published.end()) {
        return {};
    }
    return found->second.diagnostics;
}

std::optional<std::string> ScriptAnalysis::analyzed_source(InstanceId script) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto found = state_->published.find(script);
    if (found == state_->published.end()) {
        return std::nullopt;
    }
    return found->second.source;
}

std::vector<Diagnostic> ScriptAnalysis::get_diagnostics_for_line(InstanceId script, std::uint32_t line) const {
    std::vector<Diagnostic> matched;
    for (const Diagnostic& diagnostic : diagnostics(script)) {
        const std::uint32_t first = diagnostic.range.start.line;
        const std::uint32_t last = diagnostic.range.end.line < first ? first : diagnostic.range.end.line;
        if (line >= first && line <= last) {
            matched.push_back(diagnostic);
        }
    }
    return matched;
}

void ScriptAnalysis::note_world_changed() { state_->world_stale.store(true, std::memory_order_relaxed); }

void ScriptAnalysis::pump() {
    // A tree change is diffed once, however many changes came in. Only while
    // stopped: play changes are not the authored tree. The UI thread pumps
    // outside a step, so the read lock is short, and a pump() that cannot take
    // it captures next time.
    if (state_->play_stale.load(std::memory_order_relaxed) && !game_.simulation_running() &&
        state_->play_stale.exchange(false, std::memory_order_relaxed)) {
        state_->world_stale.store(true, std::memory_order_relaxed);
    }
    // Queued scripts wait for one capture too, however many were queued, and
    // that one capture is the tree change's as well.
    const bool tree = state_->world_stale.load(std::memory_order_relaxed) && !game_.simulation_running();
    if (tree || state_->capture_needed.load(std::memory_order_relaxed)) {
        DataModelLock lock(game_, DataModelLock::Read, std::chrono::milliseconds(2));
        if (lock.owns()) {
            capture_tree(tree);
        }
    }
    std::vector<Finished> ready;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        ready.swap(state_->results);
    }
    std::vector<InstanceId> fired;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled) {
            return;
        }
        for (Finished& finished : ready) {
            const auto generation = state_->generations.find(finished.id);
            if (generation == state_->generations.end() || generation->second != finished.generation) {
                continue;
            }
            if (finished.dropped) {
                // Its requirers are checked again in the batch that dropped it.
                forget_requires(finished.id);
                if (state_->published.erase(finished.id) > 0) {
                    fired.push_back(finished.id);
                }
                continue;
            }
            State::Record& record = state_->published[finished.id];
            record.name = std::move(finished.name);
            // A batch can check a tree older than the newest authored one, or a play tree.
            const NodeSnap* node =
                state_->authored_world != nullptr ? state_->authored_world->find(finished.id) : nullptr;
            if (node != nullptr) {
                record.name = shown_name(*node);
            }
            record.source = std::move(finished.source);
            record.diagnostics = std::move(finished.diagnostics);
            record.reached = std::move(finished.reached);
            ++state_->checks[finished.id];
            replace_requires(finished.id, finished.requires);
            fired.push_back(finished.id);
        }
    }
    fire(fired);
}

unsigned ScriptAnalysis::threads() const { return threads_; }

std::uint64_t ScriptAnalysis::checks(InstanceId script) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto found = state_->checks.find(script);
    return found == state_->checks.end() ? 0 : found->second;
}

std::vector<InstanceId> ScriptAnalysis::reached(InstanceId script) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto found = state_->published.find(script);
    return found == state_->published.end() ? std::vector<InstanceId>{} : found->second.reached;
}

void ScriptAnalysis::print_report(std::ostream& out) const {
    struct Row {
        std::string name;
        Diagnostic diagnostic;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        for (const auto& entry : state_->published) {
            for (const Diagnostic& diagnostic : entry.second.diagnostics) {
                rows.push_back(Row{entry.second.name, diagnostic});
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        if (left.diagnostic.range.start.line != right.diagnostic.range.start.line) {
            return left.diagnostic.range.start.line < right.diagnostic.range.start.line;
        }
        if (left.diagnostic.range.start.character != right.diagnostic.range.start.character) {
            return left.diagnostic.range.start.character < right.diagnostic.range.start.character;
        }
        return left.diagnostic.code < right.diagnostic.code;
    });
    for (const Row& row : rows) {
        out << row.name << " | " << severity_name(row.diagnostic.severity) << " | " << row.diagnostic.code << " | "
            << row.diagnostic.message << " | " << (row.diagnostic.range.start.line + 1) << '\n';
    }
}

bool ScriptAnalysis::busy() const {
    if (state_->world_stale.load(std::memory_order_relaxed) || play_stale_now()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    return !state_->pending.empty() || !state_->in_batch.empty() || state_->tree_pending || state_->tree_in_batch;
}

bool ScriptAnalysis::idle() const {
    if (state_->world_stale.load(std::memory_order_relaxed) || play_stale_now()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->pending.empty() && state_->in_batch.empty() && state_->results.empty() && !state_->tree_pending &&
           !state_->tree_in_batch;
}

bool ScriptAnalysis::settled(InstanceId script) const {
    if ((state_->world_stale.load(std::memory_order_relaxed) && !game_.simulation_running()) || play_stale_now()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    // A tree change not yet diffed, or being diffed, may check it again.
    if (state_->tree_pending || state_->tree_in_batch || state_->pending.count(script) != 0 ||
        state_->in_batch.count(script) != 0 ||
        std::any_of(state_->results.begin(), state_->results.end(),
                    [script](const Finished& finished) { return finished.id == script; })) {
        return false;
    }
    if (state_->published.count(script) != 0) {
        return true;
    }
    // A script outside the authored place, or one a playtest added, is never
    // checked: settled, with nothing to say.
    const WorldSnap* authored = state_->authored_world.get();
    if (authored == nullptr) {
        return false;
    }
    const NodeSnap* node = authored->find(script);
    return node == nullptr || !node->lua;
}

bool ScriptAnalysis::play_stale_now() const {
    return state_->play_stale.load(std::memory_order_relaxed) && !game_.simulation_running();
}

void ScriptAnalysis::fire(const std::vector<InstanceId>& ids) {
    if (ids.empty()) {
        return;
    }
    std::vector<std::function<void(InstanceId)>> handlers;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        for (const State::Handler& handler : state_->handlers) {
            if (handler.live && handler.fn) {
                handlers.push_back(handler.fn);
            }
        }
    }
    for (InstanceId id : ids) {
        for (const auto& handler : handlers) {
            handler(id);
        }
    }
}

std::uint64_t ScriptAnalysis::DiagnosticsSignal::connect(std::function<void(InstanceId)> handler) {
    std::lock_guard<std::mutex> lock(owner_->state_->mu);
    const std::uint64_t token = ++owner_->state_->next_token;
    owner_->state_->handlers.push_back(State::Handler{token, true, std::move(handler)});
    return token;
}

void ScriptAnalysis::DiagnosticsSignal::disconnect(std::uint64_t token) {
    std::lock_guard<std::mutex> lock(owner_->state_->mu);
    for (State::Handler& handler : owner_->state_->handlers) {
        if (handler.token == token) {
            handler.live = false;
            handler.fn = nullptr;
        }
    }
}

}  // namespace engine_core
