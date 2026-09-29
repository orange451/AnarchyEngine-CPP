#include "ScriptAnalysis.hpp"

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

struct NodeSnap {
    InstanceId id = 0;
    InstanceId parent = DataModel::kNoParent;
    std::string name;
    std::string class_name;
    std::string source;
    bool lua = false;
    bool module = false;
    // Direct children in the same order FindFirstChild walks them.
    std::vector<InstanceId> children;
};

struct WorldSnap {
    InstanceId root = 0;
    std::vector<NodeSnap> nodes;

    const NodeSnap* find(InstanceId id) const {
        for (const NodeSnap& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }

    std::optional<InstanceId> child_named(InstanceId parent, std::string_view name) const {
        const NodeSnap* parent_node = find(parent);
        if (parent_node == nullptr) {
            return std::nullopt;
        }
        for (InstanceId child : parent_node->children) {
            const NodeSnap* node = find(child);
            if (node != nullptr && node->name == name) {
                return child;
            }
        }
        return std::nullopt;
    }

    std::optional<InstanceId> module_named(std::string_view name, std::optional<InstanceId> prefer_parent) const {
        std::optional<InstanceId> fallback;
        for (const NodeSnap& node : nodes) {
            if (!node.module || node.name != name) {
                continue;
            }
            if (prefer_parent && node.parent == *prefer_parent) {
                return node.id;
            }
            if (!fallback) {
                fallback = node.id;
            }
        }
        return fallback;
    }

    // workspace: the root's Workspace child.
    std::optional<InstanceId> workspace() const {
        const NodeSnap* root_node = find(root);
        if (root_node != nullptr) {
            for (InstanceId child : root_node->children) {
                const NodeSnap* node = find(child);
                if (node != nullptr && node->class_name == "Workspace") {
                    return child;
                }
            }
        }
        return std::nullopt;
    }
};

struct Job {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<const WorldSnap> world;
    std::shared_ptr<Luau::FrontendCancellationToken> cancel;
};

struct Pending {
    Job job;
    std::chrono::steady_clock::time_point ready_at{};
};

struct Finished {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    std::string name;
    std::string source;
    std::vector<Diagnostic> diagnostics;
    std::vector<InstanceId> requires;
    bool cancelled = false;
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

    bool visit(Luau::AstExprCall* call) override {
        auto* global = call->func != nullptr ? call->func->as<Luau::AstExprGlobal>() : nullptr;
        if (global != nullptr && global->name == "require" && call->args.size > 0 && world != nullptr) {
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

std::vector<InstanceId> find_requires(const WorldSnap& world, InstanceId self, Luau::AstStat* root) {
    if (root == nullptr) {
        return {};
    }
    RequireWalk walk;
    walk.world = &world;
    walk.self = self;
    root->visit(&walk);
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

std::unique_ptr<PlaceTypes> build_place_types(const Luau::Scope& globals, std::shared_ptr<const WorldSnap> world) {
    auto place = std::make_unique<PlaceTypes>();
    place->world = std::move(world);
    std::unordered_map<InstanceId, const NodeSnap*> nodes;
    for (const NodeSnap& node : place->world->nodes) {
        nodes[node.id] = &node;
        std::optional<Luau::TypeId> base = class_type(globals, node.class_name);
        if (!base) {
            base = class_type(globals, "DataModel");
        }
        if (!base) {
            continue;
        }
        const Luau::ExternType* base_class = Luau::get<Luau::ExternType>(*base);
        place->types[node.id] = place->arena.addType(Luau::ExternType{base_class->name, {}, *base, std::nullopt, {},
                                                                      std::make_shared<InstanceTag>(node.id), "@anarchy",
                                                                      std::nullopt});
    }
    for (const NodeSnap& node : place->world->nodes) {
        const std::optional<Luau::TypeId> own = place->find(node.id);
        if (!own) {
            continue;
        }
        Luau::ExternType* type = Luau::getMutable<Luau::ExternType>(*own);
        const Luau::ExternType* base_class = Luau::get<Luau::ExternType>(*type->parent);
        for (InstanceId child : node.children) {
            const auto child_node = nodes.find(child);
            const std::optional<Luau::TypeId> child_type = place->find(child);
            if (child_node == nodes.end() || !child_type) {
                continue;
            }
            const std::string& name = child_node->second->name;
            if (name.empty() || type->props.count(name) != 0 || Luau::lookupExternTypeProp(base_class, name) != nullptr) {
                continue;
            }
            type->props[name] = Luau::Property::readonly(*child_type);
        }
        const std::optional<Luau::TypeId> parent =
            node.parent != DataModel::kNoParent ? place->find(node.parent) : std::nullopt;
        if (!parent) {
            continue;
        }
        const Luau::Property* declared = Luau::lookupExternTypeProp(base_class, "Parent");
        if (declared != nullptr && declared->writeTy) {
            type->props["Parent"] = Luau::Property::rw(*parent, *declared->writeTy);
        } else {
            type->props["Parent"] = Luau::Property::readonly(*parent);
        }
    }
    return place;
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
        return property->key == "PreRender" || property->key == "RenderStepped";
    }
    if (const Luau::UnknownPropButFoundLikeProp* property = Luau::get<Luau::UnknownPropButFoundLikeProp>(error)) {
        return property->key == "PreRender" || property->key == "RenderStepped";
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
};

void stamp_vector(Luau::Frontend& frontend);
void attach_api(WorkerEnv& env);

constexpr std::string_view kModulePrefix = "script-";

std::string module_name_of(InstanceId id) {
    return std::string(kModulePrefix) + std::to_string(id);
}

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

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
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
                return node->name.empty() ? node->class_name : node->name;
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
    InstanceId self = 0;
    // The snapshot the frontend's cached modules were checked against.
    std::shared_ptr<const WorldSnap> checked_world;
    // Built from checked_world. Every script is marked dirty when it is
    // replaced, so no cached module still uses the old one's types.
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

bool FindChildMagic::infer(const Luau::MagicFunctionCallContext& context) {
    if (env == nullptr || env->world == nullptr) {
        return false;
    }
    // A required module is checked in the same pass, so `script` is whichever
    // module this call is in.
    InstanceId self = env->self;
    if (context.constraint->moduleName != nullptr) {
        if (const std::optional<InstanceId> owner = instance_of_module(*context.constraint->moduleName)) {
            self = *owner;
        }
    }
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

// Two snapshots with the same instances, parents, names, and classes. Sources
// may differ. Completion's snapshots and the analyzer's are made apart, so
// they are compared by what they hold, not by identity.
bool same_tree(const WorldSnap& a, const WorldSnap& b) {
    if (a.root != b.root || a.nodes.size() != b.nodes.size()) {
        return false;
    }
    std::unordered_map<InstanceId, const NodeSnap*> before;
    before.reserve(a.nodes.size());
    for (const NodeSnap& node : a.nodes) {
        before.emplace(node.id, &node);
    }
    for (const NodeSnap& node : b.nodes) {
        const auto was = before.find(node.id);
        // Children in the same order too: FindFirstChild takes the first of two
        // siblings with one name, and the place's types follow that order.
        if (was == before.end() || was->second->parent != node.parent || was->second->name != node.name ||
            was->second->class_name != node.class_name || was->second->lua != node.lua ||
            was->second->module != node.module || was->second->children != node.children) {
            return false;
        }
    }
    return true;
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

Finished analyze_job(WorkerEnv& env, const Job& job) {
    Finished finished;
    finished.id = job.id;
    finished.generation = job.generation;
    if (job.world == nullptr) {
        finished.cancelled = true;
        return finished;
    }
    const NodeSnap* self = job.world->find(job.id);
    if (self == nullptr || !self->lua) {
        finished.cancelled = true;
        return finished;
    }
    finished.name = self->name.empty() ? self->class_name : self->name;
    finished.source = self->source;

    if (job.cancel && job.cancel->requested()) {
        finished.cancelled = true;
        return finished;
    }

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions parse_options;
    parse_options.captureComments = true;
    const Luau::ParseResult parsed =
        Luau::Parser::parse(self->source.c_str(), self->source.size(), names, allocator, parse_options);

    // Stage 1. A parse failure stops the pipeline. The compiler is unchanged.
    if (!parsed.errors.empty()) {
        for (const Luau::ParseError& error : parsed.errors) {
            finished.diagnostics.push_back(make_diagnostic(job.id, range_from(error.getLocation()), Severity::Error,
                                                           "Syntax", error.getMessage()));
        }
        if (parsed.root != nullptr) {
            finished.requires = find_requires(*job.world, job.id, parsed.root);
        }
        return finished;
    }

    // A script without a `--!` mode comment on its first lines is checked nonstrict.
    const Luau::Mode mode = Luau::parseMode(parsed.hotcomments).value_or(Luau::Mode::Nonstrict);
    if (parsed.root != nullptr) {
        finished.requires = find_requires(*job.world, job.id, parsed.root);
    }
    if (mode == Luau::Mode::NoCheck) {
        return finished;
    }

    if (!env.init_error.empty() || env.frontend == nullptr || env.frontend->globals.globalScope == nullptr) {
        const std::string message = env.init_error.empty() ? "script analysis is unavailable" : env.init_error;
        finished.diagnostics.push_back(
            make_diagnostic(job.id, TextRange{}, Severity::Error, "Analysis", message));
        return finished;
    }

    // Stage 2. Builtin lints. Unknown globals are left to the type checker.
    Luau::LintOptions lint_options;
    lint_options.setDefaults();
    lint_options.warningMask &= ~Luau::LintWarning::parseMask(parsed.hotcomments);
    lint_options.disableWarning(Luau::LintWarning::Code_UnknownGlobal);
    if (mode == Luau::Mode::Strict) {
        lint_options.disableWarning(Luau::LintWarning::Code_ImplicitReturn);
    }
    const std::vector<Luau::LintWarning> lints = Luau::lint(parsed.root, names, env.frontend->globals.globalScope,
                                                            env.untyped.get(), parsed.hotcomments, lint_options);
    for (const Luau::LintWarning& warning : lints) {
        Severity severity = Severity::Warning;
        if (warning.code == Luau::LintWarning::Code_LocalUnused || warning.code == Luau::LintWarning::Code_FunctionUnused ||
            warning.code == Luau::LintWarning::Code_ImportUnused) {
            severity = Severity::Hint;
        }
        const char* name = Luau::LintWarning::getName(warning.code);
        finished.diagnostics.push_back(make_diagnostic(job.id, range_from(warning.location), severity,
                                                       std::string("Lint/") + (name != nullptr ? name : "Unknown"),
                                                       warning.text));
    }

    if (job.cancel && job.cancel->requested()) {
        finished.cancelled = true;
        return finished;
    }

    // Stage 3. Type check against the registered API. Lint already ran, so the
    // frontend does not lint again.
    // The full checker runs for both strict and nonstrict. Nonstrict then
    // downgrades type errors to warnings. `--!nocheck` never reaches here.
    try {
        const std::string check_source = checked_source(self->source, parsed, mode);
        const std::string module_name = module_name_of(job.id);
        sync_world(env, job.world, true);
        env.files.module_name = &module_name;
        env.files.world = job.world.get();
        env.files.source = &check_source;
        env.files.display = &finished.name;
        env.files.type = self->module ? Luau::SourceCode::Module : Luau::SourceCode::Script;
        env.configs.config.mode = Luau::Mode::Strict;
        env.world = job.world.get();
        env.self = job.id;
        env.frontend->markDirty(module_name);

        Luau::FrontendOptions options;
        options.runLintChecks = false;
        options.retainFullTypeGraphs = false;
        options.cancellationToken = job.cancel;
        const Luau::CheckResult checked = env.frontend->check(module_name, options);
        env.world = nullptr;
        env.files.world = nullptr;
        env.self = 0;
        if (job.cancel && job.cancel->requested()) {
            finished.cancelled = true;
            return finished;
        }
        Luau::TypeErrorToStringOptions stringify;
        stringify.fileResolver = &env.files;
        for (const Luau::TypeError& error : checked.errors) {
            // A required module reports its own problems when it is analyzed.
            if (error.moduleName != module_name) {
                continue;
            }
            if (Luau::get<Luau::SyntaxError>(error) != nullptr) {
                finished.diagnostics.push_back(make_diagnostic(job.id, range_from(error.location), Severity::Error, "Syntax",
                                                               Luau::toString(error, stringify)));
                continue;
            }
            if (const Luau::UnknownSymbol* symbol = Luau::get<Luau::UnknownSymbol>(error)) {
                if (symbol->context == Luau::UnknownSymbol::Binding) {
                    finished.diagnostics.push_back(make_diagnostic(job.id, range_from(error.location), Severity::Warning,
                                                                   "Lint/UnknownGlobal", Luau::toString(error, stringify)));
                    continue;
                }
            }
            Severity severity = mode == Luau::Mode::Strict ? Severity::Error : Severity::Warning;
            if (missing_render_member(error)) {
                severity = Severity::Warning;
            }
            finished.diagnostics.push_back(
                make_diagnostic(job.id, range_from(error.location), severity, "Type", Luau::toString(error, stringify)));
        }
    } catch (const std::exception& error) {
        env.world = nullptr;
        env.files.world = nullptr;
        env.self = 0;
        if (job.cancel && job.cancel->requested()) {
            finished.cancelled = true;
            return finished;
        }
        finished.diagnostics.push_back(make_diagnostic(job.id, TextRange{}, Severity::Error, "Analysis", error.what()));
    } catch (...) {
        env.world = nullptr;
        env.files.world = nullptr;
        env.self = 0;
        if (job.cancel && job.cancel->requested()) {
            finished.cancelled = true;
            return finished;
        }
        finished.diagnostics.push_back(make_diagnostic(job.id, TextRange{}, Severity::Error, "Analysis", "analysis failed"));
    }
    env.world = nullptr;
    env.files.world = nullptr;
    env.self = 0;
    return finished;
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

// The place as completion sees it, as the snapshot a check reads. The root is
// the parentless Game or DataModel.
std::shared_ptr<WorldSnap> world_from_nodes(const std::vector<LuaNode>& nodes) {
    auto world = std::make_shared<WorldSnap>();
    bool rooted = false;
    for (const LuaNode& item : nodes) {
        NodeSnap node;
        node.id = item.id;
        node.parent = item.parent;
        node.name = item.name;
        node.class_name = item.class_name;
        node.source = item.source;
        node.module = item.class_name == "ModuleScript";
        node.lua = node.module || item.class_name == "Script";
        if (!rooted && item.parent == DataModel::kNoParent &&
            (item.class_name == "Game" || item.class_name == "DataModel")) {
            world->root = item.id;
            rooted = true;
        }
        world->nodes.push_back(std::move(node));
    }
    // Children in the order the nodes came, which completion_world makes the
    // tree's sibling order, as capture_world has it.
    std::unordered_map<InstanceId, std::size_t> index;
    index.reserve(world->nodes.size());
    for (std::size_t at = 0; at < world->nodes.size(); ++at) {
        index.emplace(world->nodes[at].id, at);
    }
    for (const NodeSnap& child : world->nodes) {
        const auto parent = index.find(child.parent);
        if (parent != index.end() && child.parent != child.id) {
            world->nodes[parent->second].children.push_back(child.id);
        }
    }
    return world;
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
        env.self = request.script;
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
    env.self = 0;
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
            const bool own_initializer = std::any_of(declaring.begin(), declaring.end(), [&](const Luau::AstLocal* var) {
                return name == var->name.value;
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

std::shared_ptr<WorldSnap> capture_world(DataModel& game) {
    auto world = std::make_shared<WorldSnap>();
    world->root = game.id();
    NodeSnap root;
    root.id = world->root;
    root.parent = DataModel::kNoParent;
    root.name = game.name(world->root);
    root.class_name = game.class_name();
    world->nodes.push_back(std::move(root));
    game.for_each_instance([&](DataModel& object) {
        NodeSnap node;
        node.id = object.id();
        node.parent = game.parent(object.id());
        node.name = game.name(object.id());
        node.class_name = object.class_name() != nullptr ? object.class_name() : "";
        if (auto* source = dynamic_cast<LuaSource*>(&object)) {
            node.lua = true;
            node.module = dynamic_cast<ModuleScript*>(source) != nullptr;
            node.source = source->source();
        }
        world->nodes.push_back(std::move(node));
    });
    for (NodeSnap& node : world->nodes) {
        for (InstanceId child = game.first_child(node.id); child != 0; child = game.next_sibling(child)) {
            node.children.push_back(child);
        }
    }
    return world;
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
    // Set by note_world_changed. pump() turns it into one invalidate_all.
    std::atomic<bool> world_stale{false};
    std::condition_variable cv;
    std::mutex start_mu;
    bool stop = false;
    bool enabled = true;
    bool started = false;
    std::uint64_t next_token = 0;
    int inflight = 0;
    // The script the worker is checking. 0 between jobs.
    InstanceId running = 0;
    // Luau parses and checks here, as deep as its own recursion limits allow.
    StackThread worker;

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
    std::unordered_map<InstanceId, std::shared_ptr<Luau::FrontendCancellationToken>> tokens;
    std::vector<Finished> results;
    struct Record {
        std::string name;
        std::string source;
        std::vector<Diagnostic> diagnostics;
    };
    std::unordered_map<InstanceId, Record> published;
    std::unordered_map<InstanceId, std::unordered_set<InstanceId>> requires_of;
    std::unordered_map<InstanceId, std::unordered_set<InstanceId>> required_by;

    AnalysisScope scope = AnalysisScope::All;
    // Luau autocomplete requests, answered before queued checks, and the one
    // the worker is answering now.
    std::deque<std::shared_ptr<CompleteRequest>> completions;
    std::shared_ptr<CompleteRequest> serving;
    // Editors showing each script. Open scope analyzes these and what they require.
    std::unordered_map<InstanceId, int> watched;
    // Waiting for pump() to capture the tree: newly watched scripts, and
    // modules a watched script turned out to require.
    std::unordered_set<InstanceId> to_schedule;
};

std::unordered_set<InstanceId> ScriptAnalysis::active_locked() const {
    std::unordered_set<InstanceId> active;
    std::vector<InstanceId> frontier;
    for (const auto& entry : state_->watched) {
        if (active.insert(entry.first).second) {
            frontier.push_back(entry.first);
        }
    }
    while (!frontier.empty()) {
        const InstanceId next = frontier.back();
        frontier.pop_back();
        const auto found = state_->requires_of.find(next);
        if (found == state_->requires_of.end()) {
            continue;
        }
        for (InstanceId target : found->second) {
            if (active.insert(target).second) {
                frontier.push_back(target);
            }
        }
    }
    return active;
}

std::vector<InstanceId> ScriptAnalysis::drop_inactive_locked() {
    const std::unordered_set<InstanceId> active = active_locked();
    std::unordered_set<InstanceId> known;
    for (const auto& entry : state_->published) {
        known.insert(entry.first);
    }
    for (const auto& entry : state_->pending) {
        known.insert(entry.first);
    }
    for (const auto& entry : state_->tokens) {
        known.insert(entry.first);
    }
    for (InstanceId id : state_->to_schedule) {
        known.insert(id);
    }
    std::vector<InstanceId> dropped;
    for (InstanceId id : known) {
        if (active.count(id) != 0) {
            continue;
        }
        ++state_->generations[id];
        state_->pending.erase(id);
        state_->to_schedule.erase(id);
        const auto token = state_->tokens.find(id);
        if (token != state_->tokens.end()) {
            if (token->second) {
                token->second->cancel();
            }
            state_->tokens.erase(token);
        }
        state_->results.erase(std::remove_if(state_->results.begin(), state_->results.end(),
                                             [id](const Finished& finished) { return finished.id == id; }),
                              state_->results.end());
        if (state_->published.erase(id) > 0) {
            dropped.push_back(id);
        }
    }
    // An inactive script's own requires no longer keep anything alive.
    for (InstanceId id : known) {
        if (active.count(id) == 0) {
            const auto own = state_->requires_of.find(id);
            if (own != state_->requires_of.end()) {
                for (InstanceId target : own->second) {
                    const auto found = state_->required_by.find(target);
                    if (found != state_->required_by.end()) {
                        found->second.erase(id);
                    }
                }
                state_->requires_of.erase(own);
            }
        }
    }
    return dropped;
}

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

ScriptAnalysis::ScriptAnalysis(DataModel& game) : game_(game), state_(std::make_unique<State>()) {
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
        for (auto& entry : state_->tokens) {
            if (entry.second) {
                entry.second->cancel();
            }
        }
        state_->tokens.clear();
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
    }
    for (const std::shared_ptr<CompleteRequest>& request : stopped) {
        request->answer->facts.error = "script analysis has stopped";
        finish_request(*request);
    }
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
}

// Luau's parser allows 1000 levels of nesting and its checker hundreds more;
// a Debug build spends a few KB of stack on each. std::thread's default of 1 MB
// overflows on code nested that deep, so the worker reserves this much.
constexpr std::size_t kWorkerStackBytes = std::size_t{16} << 20;

void ScriptAnalysis::ensure_worker() {
    std::lock_guard<std::mutex> start(state_->start_mu);
    if (state_->started) {
        return;
    }
    state_->started = true;
    state_->worker = StackThread(kWorkerStackBytes, [this] { run(); });
}

void ScriptAnalysis::run() {
    // This thread never calls the play VM and never takes the DataModel lock.
    // Jobs carry a copy of Source and Name taken on the gameplay thread.
    // On the heap: its frontend points at its own resolvers, so it is rebuilt
    // in place, not moved, when the registry changed.
    auto owned = std::make_unique<WorkerEnv>();
    owned->init();
    while (true) {
        Job job;
        std::shared_ptr<CompleteRequest> request;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->cv.wait(lock,
                            [&] { return state_->stop || !state_->pending.empty() || !state_->completions.empty(); });
            if (state_->stop) {
                return;
            }
            if (!state_->completions.empty()) {
                request = std::move(state_->completions.front());
                state_->completions.pop_front();
                state_->serving = request;
            }
        }
        if (owned->revision != lua_registry_revision()) {
            owned = std::make_unique<WorkerEnv>();
            owned->init();
        }
        WorkerEnv& env = *owned;
        if (request) {
            request->answer->facts = facts_job(env, *request);
            {
                std::lock_guard<std::mutex> lock(state_->mu);
                state_->serving.reset();
            }
            finish_request(*request);
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            if (state_->stop) {
                return;
            }
            if (state_->pending.empty()) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            auto due = state_->pending.begin();
            for (auto it = state_->pending.begin(); it != state_->pending.end(); ++it) {
                if (it->second.ready_at < due->second.ready_at) {
                    due = it;
                }
            }
            if (due->second.ready_at > now) {
                state_->cv.wait_until(lock, due->second.ready_at);
                continue;
            }
            job = std::move(due->second.job);
            state_->pending.erase(due);
            ++state_->inflight;
            state_->running = job.id;
        }
        Finished finished = analyze_job(*owned, job);
        if (owned->frontend != nullptr) {
            state_->cached_modules.store(owned->frontend->sourceNodes.size(), std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            --state_->inflight;
            state_->running = 0;
            const auto generation = state_->generations.find(job.id);
            const bool current = generation != state_->generations.end() && generation->second == job.generation;
            if (!state_->stop && state_->enabled && current && !finished.cancelled &&
                !(job.cancel && job.cancel->requested())) {
                state_->results.push_back(std::move(finished));
            }
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
        state_->cv.notify_all();
    }
    for (const std::shared_ptr<CompleteRequest>& old : replaced) {
        old->answer->facts.error = "replaced by a newer request";
        finish_request(*old);
    }
    ensure_worker();
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
            for (auto& entry : state_->tokens) {
                if (entry.second) {
                    entry.second->cancel();
                }
            }
            state_->tokens.clear();
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

void ScriptAnalysis::schedule(const std::vector<InstanceId>& ids) {
    if (ids.empty()) {
        return;
    }
    {
        // The capture copies every script's source. Skip it when nothing will run.
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
    }
    const std::shared_ptr<WorldSnap> world = capture_world(game_);
    ensure_worker();
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->enabled || state_->stop) {
        return;
    }
    const auto ready_at = std::chrono::steady_clock::now() + kDebounce;
    for (InstanceId id : ids) {
        // Handled either way: a dead or non-script id has nothing to check.
        state_->to_schedule.erase(id);
        const NodeSnap* node = world->find(id);
        if (node == nullptr || !node->lua) {
            continue;
        }
        std::uint64_t& generation = state_->generations[id];
        ++generation;
        const auto previous = state_->tokens.find(id);
        if (previous != state_->tokens.end() && previous->second) {
            previous->second->cancel();
        }
        auto token = std::make_shared<Luau::FrontendCancellationToken>();
        state_->tokens[id] = token;
        Job job;
        job.id = id;
        job.generation = generation;
        job.world = world;
        job.cancel = std::move(token);
        state_->pending[id] = Pending{std::move(job), ready_at};
    }
    state_->cv.notify_all();
}

void ScriptAnalysis::invalidate(InstanceId script) {
    DataModel* object = game_.instance(script);
    if (dynamic_cast<LuaSource*>(object) == nullptr) {
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
        if (state_->scope == AnalysisScope::Open) {
            const std::unordered_set<InstanceId> active = active_locked();
            chain.erase(std::remove_if(chain.begin(), chain.end(),
                                       [&active](InstanceId id) { return active.count(id) == 0; }),
                        chain.end());
        }
    }
    schedule(chain);
}

void ScriptAnalysis::invalidate_all() {
    std::vector<InstanceId> ids;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
        if (state_->scope == AnalysisScope::Open) {
            const std::unordered_set<InstanceId> active = active_locked();
            ids.assign(active.begin(), active.end());
        }
    }
    if (ids.empty()) {
        // All scope: every script in the tree. schedule skips non-scripts.
        bool all = false;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            all = state_->scope == AnalysisScope::All;
        }
        if (!all) {
            return;
        }
        game_.for_each_instance([&ids](DataModel& object) {
            if (dynamic_cast<LuaSource*>(&object) != nullptr) {
                ids.push_back(object.id());
            }
        });
    }
    schedule(ids);
}

void ScriptAnalysis::set_scope(AnalysisScope scope) {
    std::vector<InstanceId> dropped;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->scope == scope) {
            return;
        }
        state_->scope = scope;
        if (scope == AnalysisScope::Open) {
            dropped = drop_inactive_locked();
        }
    }
    fire(dropped);
    if (scope == AnalysisScope::All) {
        state_->world_stale.store(true, std::memory_order_relaxed);
    }
}

AnalysisScope ScriptAnalysis::scope() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->scope;
}

void ScriptAnalysis::watch(InstanceId script) {
    std::lock_guard<std::mutex> lock(state_->mu);
    ++state_->watched[script];
    // Checked against the tree as it is when the editor opens, not whenever
    // its last result was made.
    state_->to_schedule.insert(script);
}

void ScriptAnalysis::unwatch(InstanceId script) {
    std::vector<InstanceId> dropped;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        const auto found = state_->watched.find(script);
        if (found == state_->watched.end()) {
            return;
        }
        if (--found->second > 0) {
            return;
        }
        state_->watched.erase(found);
        if (state_->scope == AnalysisScope::Open) {
            dropped = drop_inactive_locked();
        } else {
            state_->to_schedule.erase(script);
        }
    }
    fire(dropped);
}

std::size_t ScriptAnalysis::cached_modules() const {
    return state_->cached_modules.load(std::memory_order_relaxed);
}

void ScriptAnalysis::remove(InstanceId script) {
    std::vector<InstanceId> dependents;
    bool notify = false;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        enabled = state_->enabled;
        std::uint64_t& generation = state_->generations[script];
        ++generation;
        state_->pending.erase(script);
        const auto token = state_->tokens.find(script);
        if (token != state_->tokens.end() && token->second) {
            token->second->cancel();
        }
        state_->tokens.erase(script);
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
    // A tree change rechecks every script once, however many changes came in.
    // Only while stopped: play changes are not the authored tree. The UI
    // thread pumps outside a step, so the read lock is short and uncontended.
    if (state_->world_stale.load(std::memory_order_relaxed) && !game_.simulation_running()) {
        DataModelLock lock(game_, DataModelLock::Read, std::chrono::milliseconds(2));
        if (lock.owns() && state_->world_stale.exchange(false, std::memory_order_relaxed)) {
            invalidate_all();
        }
    }
    // Newly watched scripts, and modules they turned out to require.
    std::vector<InstanceId> wanted;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        wanted.assign(state_->to_schedule.begin(), state_->to_schedule.end());
    }
    if (!wanted.empty()) {
        DataModelLock lock(game_, DataModelLock::Read, std::chrono::milliseconds(2));
        if (lock.owns()) {
            schedule(wanted);
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
            State::Record& record = state_->published[finished.id];
            record.name = std::move(finished.name);
            record.source = std::move(finished.source);
            record.diagnostics = std::move(finished.diagnostics);
            replace_requires(finished.id, finished.requires);
            fired.push_back(finished.id);
        }
        if (state_->scope == AnalysisScope::Open && !fired.empty()) {
            // A required module is on the watched script's path. Check it too,
            // and what it requires, as each result arrives.
            for (InstanceId id : fired) {
                const auto found = state_->requires_of.find(id);
                if (found == state_->requires_of.end()) {
                    continue;
                }
                for (InstanceId target : found->second) {
                    if (state_->published.count(target) == 0 && state_->pending.count(target) == 0 &&
                        state_->tokens.count(target) == 0) {
                        state_->to_schedule.insert(target);
                    }
                }
            }
            // A dropped require can leave a module nobody watches.
            const std::vector<InstanceId> dropped = drop_inactive_locked();
            fired.insert(fired.end(), dropped.begin(), dropped.end());
        }
    }
    fire(fired);
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
    if (state_->world_stale.load(std::memory_order_relaxed)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    return !state_->pending.empty() || state_->inflight > 0 || !state_->to_schedule.empty();
}

bool ScriptAnalysis::idle() const {
    if (state_->world_stale.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->pending.empty() && state_->inflight == 0 && state_->results.empty() &&
           state_->to_schedule.empty();
}

bool ScriptAnalysis::settled(InstanceId script) const {
    if (state_->world_stale.load(std::memory_order_relaxed) && !game_.simulation_running()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->published.count(script) == 0 || state_->pending.count(script) != 0 ||
        state_->to_schedule.count(script) != 0 || state_->running == script) {
        return false;
    }
    return std::none_of(state_->results.begin(), state_->results.end(),
                        [script](const Finished& finished) { return finished.id == script; });
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
