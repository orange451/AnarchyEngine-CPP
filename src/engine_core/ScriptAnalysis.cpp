#include "ScriptAnalysis.hpp"

#include "DataModel.hpp"
#include "Script.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
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

extern const char kEngineDefinitionSource[];

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
        if (index == nullptr || index->index != "FindFirstChild" || call->args.size < 1) {
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
// still optional. assert then removes the nil.
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

void attach_find_child(WorkerEnv& env);

struct SourceFileResolver : Luau::FileResolver {
    const std::string* module_name = nullptr;
    const std::string* source = nullptr;
    const std::string* display = nullptr;
    Luau::SourceCode::Type type = Luau::SourceCode::Script;

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
        if (module_name == nullptr || source == nullptr || name != *module_name) {
            return std::nullopt;
        }
        return Luau::SourceCode{*source, type};
    }

    std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override {
        if (display != nullptr && !display->empty() && module_name != nullptr && name == *module_name) {
            return *display;
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
    out << "engine.d.lua failed to load";
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
    std::shared_ptr<Luau::MagicFunction> find_child;

    void init() {
        configs.config.mode = Luau::Mode::Nonstrict;
        configs.config.parseOptions.captureComments = true;
        configs.config.enabledLint.setDefaults();
        frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::New, &files, &configs, Luau::FrontendOptions{});
        frontend->iceHandler.onInternalError = [](const char*) {};
        try {
            Luau::unfreeze(frontend->globals.globalTypes);
            Luau::registerBuiltinGlobals(*frontend, frontend->globals);
            const Luau::LoadDefinitionFileResult loaded = frontend->loadDefinitionFile(
                frontend->globals, frontend->globals.globalScope, kEngineDefinitionSource, "@anarchy",
                /*captureComments*/ false, /*typeCheckForAutocomplete*/ false);
            if (!loaded.success) {
                init_error = definition_failure(loaded);
            } else {
                attach_find_child(*this);
            }
            Luau::freeze(frontend->globals.globalTypes);
        } catch (const std::exception& error) {
            init_error = error.what();
        } catch (...) {
            init_error = "engine.d.lua failed to load";
        }
    }
};

void attach_find_child(WorkerEnv& env) {
    if (env.frontend == nullptr || env.frontend->globals.globalScope == nullptr) {
        return;
    }
    const auto found = env.frontend->globals.globalScope->exportedTypeBindings.find("Instance");
    if (found == env.frontend->globals.globalScope->exportedTypeBindings.end()) {
        return;
    }
    Luau::ExternType* instance = Luau::getMutable<Luau::ExternType>(Luau::follow(found->second.type));
    if (instance == nullptr) {
        return;
    }
    const auto prop = instance->props.find("FindFirstChild");
    if (prop == instance->props.end() || !prop->second.readTy) {
        return;
    }
    Luau::FunctionType* function = Luau::getMutable<Luau::FunctionType>(Luau::follow(*prop->second.readTy));
    if (function == nullptr) {
        return;
    }
    env.find_child = std::make_shared<FindChildMagic>(&env);
    function->magic = env.find_child;
}

bool FindChildMagic::infer(const Luau::MagicFunctionCallContext& context) {
    if (env == nullptr || env->world == nullptr) {
        return false;
    }
    const std::optional<InstanceId> child =
        resolve_expr(*env->world, env->self, const_cast<Luau::AstExprCall*>(context.callSite.get()));
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
    const Luau::TypeId optional =
        arena->addType(Luau::UnionType{{context.solver->builtinTypes->nilType, class_ty}});
    Luau::asMutable(context.result)->ty.emplace<Luau::BoundTypePack>(arena->addTypePack({optional}));
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

    // Stage 3. Type check against engine.d.lua. Lint already ran, so the
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
        const std::string module_name = "script-" + std::to_string(job.id);
        env.files.module_name = &module_name;
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
        env.self = 0;
        if (job.cancel && job.cancel->requested()) {
            finished.cancelled = true;
            return finished;
        }
        Luau::TypeErrorToStringOptions stringify;
        stringify.fileResolver = &env.files;
        for (const Luau::TypeError& error : checked.errors) {
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
        env.self = 0;
        if (job.cancel && job.cancel->requested()) {
            finished.cancelled = true;
            return finished;
        }
        finished.diagnostics.push_back(make_diagnostic(job.id, TextRange{}, Severity::Error, "Analysis", error.what()));
    } catch (...) {
        env.world = nullptr;
        env.self = 0;
        if (job.cancel && job.cancel->requested()) {
            finished.cancelled = true;
            return finished;
        }
        finished.diagnostics.push_back(make_diagnostic(job.id, TextRange{}, Severity::Error, "Analysis", "analysis failed"));
    }
    env.world = nullptr;
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

struct ScriptAnalysis::State {
    std::mutex mu;
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

void ScriptAnalysis::invalidate(InstanceId script) {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        enabled = state_->enabled && !state_->stop;
    }
    if (!enabled) {
        return;
    }
    DataModel* object = model_.instance(script);
    if (dynamic_cast<LuaSource*>(object) == nullptr) {
        return;
    }
    const std::shared_ptr<WorldSnap> world = capture_world(model_);
    std::vector<InstanceId> chain;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
        std::unordered_set<InstanceId> seen;
        collect_dependents(script, chain, seen);
    }
    ensure_worker();
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
        const TypeMode mode = state_->default_mode;
        const auto ready_at = std::chrono::steady_clock::now() + kDebounce;
        for (InstanceId id : chain) {
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
}

void ScriptAnalysis::invalidate_all() {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        enabled = state_->enabled && !state_->stop;
    }
    if (!enabled) {
        return;
    }
    const std::shared_ptr<WorldSnap> world = capture_world(model_);
    ensure_worker();
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
        const TypeMode mode = state_->default_mode;
        const auto ready_at = std::chrono::steady_clock::now() + kDebounce;
        for (const NodeSnap& node : world->nodes) {
            if (!node.lua) {
                continue;
            }
            std::uint64_t& generation = state_->generations[node.id];
            ++generation;
            const auto previous = state_->tokens.find(node.id);
            if (previous != state_->tokens.end() && previous->second) {
                previous->second->cancel();
            }
            auto token = std::make_shared<Luau::FrontendCancellationToken>();
            state_->tokens[node.id] = token;
            Job job;
            job.id = node.id;
            job.generation = generation;
            job.default_mode = mode;
            job.world = world;
            job.cancel = std::move(token);
            state_->pending[node.id] = Pending{std::move(job), ready_at};
        }
        state_->cv.notify_all();
    }
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

void ScriptAnalysis::pump() {
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
    std::lock_guard<std::mutex> lock(state_->mu);
    return !state_->pending.empty() || state_->inflight > 0;
}

bool ScriptAnalysis::idle() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->pending.empty() && state_->inflight == 0 && state_->results.empty();
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
