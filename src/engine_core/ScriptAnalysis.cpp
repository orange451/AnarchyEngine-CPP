#include "ScriptAnalysis.hpp"

#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "LuaApi.hpp"
#include "Script.hpp"

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
#include "Luau/Frontend.h"
#include "Luau/Linter.h"
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
};

struct Job {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    TypeMode default_mode = TypeMode::NonStrict;
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

Luau::Mode to_luau_mode(TypeMode mode) {
    switch (mode) {
    case TypeMode::NoCheck:
        return Luau::Mode::NoCheck;
    case TypeMode::Strict:
        return Luau::Mode::Strict;
    case TypeMode::NonStrict:
        return Luau::Mode::Nonstrict;
    }
    return Luau::Mode::Nonstrict;
}

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

std::string one_line(std::string message) {
    for (char& character : message) {
        if (character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    return message;
}

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
        if (global->name == "game" || global->name == "workspace") {
            return world.root;
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
// literal and that child is in the place, the result is that child's class,
// still optional. assert then removes the nil. WaitForChild's is not optional.
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
            } else if (global->name == "game" || global->name == "workspace") {
                found = world->root;
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
    std::shared_ptr<Luau::MagicFunction> find_child;
    std::shared_ptr<Luau::MagicFunction> service_result;
    std::shared_ptr<Luau::MagicFunction> creatable_result;

    void init() {
        configs.config.mode = Luau::Mode::Nonstrict;
        configs.config.parseOptions.captureComments = true;
        configs.config.enabledLint.setDefaults();
        frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::New, &files, &configs, Luau::FrontendOptions{});
        frontend->iceHandler.onInternalError = [](const char*) {};
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
    const std::optional<InstanceId> child =
        resolve_expr(*env->world, self, const_cast<Luau::AstExprCall*>(context.callSite.get()));
    if (!child) {
        return false;
    }
    const NodeSnap* node = env->world->find(*child);
    if (node == nullptr || node->class_name.empty()) {
        return false;
    }
    const std::optional<Luau::TypeFun> type_fun = context.solver->rootScope->lookupType(node->class_name);
    if (!type_fun) {
        return false;
    }
    const Luau::TypeId class_ty = Luau::follow(type_fun->type);
    if (Luau::get<Luau::ExternType>(class_ty) == nullptr) {
        return false;
    }
    Luau::TypeArena* arena = context.solver->arena.get();
    // FindFirstChild can give nil and stays optional. WaitForChild declares
    // Instance, since it yields until the child is there.
    Luau::TypeId result = class_ty;
    const auto* method = context.callSite->func->as<Luau::AstExprIndexName>();
    const LuaField* field = method != nullptr ? lua_class_find("DataModel", method->index.value) : nullptr;
    const std::string_view declared = field != nullptr && field->type_name != nullptr ? field->type_name : "?";
    if (declared.empty() || declared.back() == '?') {
        result = arena->addType(Luau::UnionType{{context.solver->builtinTypes->nilType, class_ty}});
    }
    Luau::asMutable(context.result)->ty.emplace<Luau::BoundTypePack>(arena->addTypePack({result}));
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

    const Luau::Mode mode = Luau::parseMode(parsed.hotcomments).value_or(to_luau_mode(job.default_mode));
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
                                                            /*module*/ nullptr, parsed.hotcomments, lint_options);
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
    // A leading `--!nonstrict` is rewritten so Luau does not switch to its
    // smaller nonstrict pass, which skips unknown properties such as PreRender.
    try {
        std::string check_source = self->source;
        if (mode == Luau::Mode::Nonstrict) {
            const std::size_t line_end = check_source.find('\n');
            const std::size_t head = line_end == std::string::npos ? check_source.size() : line_end;
            const std::size_t at = check_source.find("--!nonstrict");
            if (at != std::string::npos && at < head) {
                check_source.replace(at, std::char_traits<char>::length("--!nonstrict"), "--!strict");
            }
        }
        const std::string module_name = module_name_of(job.id);
        // Required modules stay cached in the frontend. A new snapshot can
        // change their source or what their paths reach, so recheck them.
        if (env.checked_world != job.world) {
            for (const NodeSnap& node : job.world->nodes) {
                if (node.lua) {
                    env.frontend->markDirty(module_name_of(node.id));
                }
            }
            env.checked_world = job.world;
        }
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

std::shared_ptr<WorldSnap> capture_world(DataModel& model) {
    auto world = std::make_shared<WorldSnap>();
    world->root = model.id();
    NodeSnap root;
    root.id = world->root;
    root.parent = DataModel::kNoParent;
    root.name = model.name(world->root);
    root.class_name = model.class_name();
    world->nodes.push_back(std::move(root));
    model.for_each_instance([&](DataModel& object) {
        NodeSnap node;
        node.id = object.id();
        node.parent = model.parent(object.id());
        node.name = model.name(object.id());
        node.class_name = object.class_name() != nullptr ? object.class_name() : "";
        if (auto* source = dynamic_cast<LuaSource*>(&object)) {
            node.lua = true;
            node.module = dynamic_cast<ModuleScript*>(source) != nullptr;
            node.source = source->source();
        }
        world->nodes.push_back(std::move(node));
    });
    for (NodeSnap& node : world->nodes) {
        for (InstanceId child = model.first_child(node.id); child != 0; child = model.next_sibling(child)) {
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
    TypeMode default_mode = TypeMode::NonStrict;
    std::uint64_t next_token = 0;
    int inflight = 0;
    std::thread worker;

    struct Handler {
        std::uint64_t token = 0;
        bool live = true;
        std::function<void(InstanceId)> fn;
    };
    std::vector<Handler> handlers;
    std::unordered_map<InstanceId, std::uint64_t> generations;
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

ScriptAnalysis::ScriptAnalysis(DataModel& model) : model_(model), state_(std::make_unique<State>()) {
    signal_.owner_ = this;
    model_.set_script_analysis(this);
}

ScriptAnalysis::~ScriptAnalysis() { shutdown(); }

void ScriptAnalysis::shutdown() {
    if (model_.script_analysis() == this) {
        model_.set_script_analysis(nullptr);
    }
    std::lock_guard<std::mutex> start(state_->start_mu);
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
        state_->cv.notify_all();
    }
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
}

void ScriptAnalysis::ensure_worker() {
    std::lock_guard<std::mutex> start(state_->start_mu);
    if (state_->started) {
        return;
    }
    state_->started = true;
    state_->worker = std::thread([this] { run(); });
}

void ScriptAnalysis::run() {
    // This thread never calls the play VM and never takes the DataModel lock.
    // Jobs carry a copy of Source and Name taken on the gameplay thread.
    WorkerEnv env;
    env.init();
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->cv.wait(lock, [&] { return state_->stop || !state_->pending.empty(); });
            if (state_->stop) {
                return;
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
        }
        Finished finished = analyze_job(env, job);
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            --state_->inflight;
            const auto generation = state_->generations.find(job.id);
            const bool current = generation != state_->generations.end() && generation->second == job.generation;
            if (!state_->stop && state_->enabled && current && !finished.cancelled &&
                !(job.cancel && job.cancel->requested())) {
                state_->results.push_back(std::move(finished));
            }
        }
    }
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

void ScriptAnalysis::set_default_mode(TypeMode mode) {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->default_mode == mode) {
            return;
        }
        state_->default_mode = mode;
    }
    invalidate_all();
}

TypeMode ScriptAnalysis::default_mode() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->default_mode;
}

void ScriptAnalysis::schedule(const std::vector<InstanceId>& ids) {
    if (ids.empty()) {
        return;
    }
    const std::shared_ptr<WorldSnap> world = capture_world(model_);
    ensure_worker();
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->enabled || state_->stop) {
        return;
    }
    const TypeMode mode = state_->default_mode;
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
        job.default_mode = mode;
        job.world = world;
        job.cancel = std::move(token);
        state_->pending[id] = Pending{std::move(job), ready_at};
    }
    state_->cv.notify_all();
}

void ScriptAnalysis::invalidate(InstanceId script) {
    DataModel* object = model_.instance(script);
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
        model_.for_each_instance([&ids](DataModel& object) {
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
    if (state_->world_stale.load(std::memory_order_relaxed) && !model_.simulation_running()) {
        DataModelLock lock(model_, DataModelLock::Read, std::chrono::milliseconds(2));
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
        DataModelLock lock(model_, DataModelLock::Read, std::chrono::milliseconds(2));
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
