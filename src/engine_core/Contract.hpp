#pragma once

#include <stdexcept>

namespace engine_core {

// Thrown by the test handler. The default handler aborts instead.
class ContractViolation : public std::logic_error {
public:
    explicit ContractViolation(const char* what) : std::logic_error(what) {}
};

using ContractHandler = void (*)(const char* message);

// nullptr restores the default, which aborts the process.
void set_contract_handler(ContractHandler handler);

// While Lua runs, contract_fail throws ContractViolation instead of calling the
// handler or aborting: the binding's lua_guard, or Luau's own catch around a C
// function, turns it into a script error that pcall can catch. ScriptRuntime holds
// one around every lua_resume, on whichever thread enters the VM, so a refusal a
// script reaches is a Lua error in the studio too, where no handler is installed.
class ScriptContractScope {
public:
    ScriptContractScope();
    ~ScriptContractScope();
    ScriptContractScope(const ScriptContractScope&) = delete;
    ScriptContractScope& operator=(const ScriptContractScope&) = delete;
};

// Whether a ScriptContractScope is alive on this thread.
bool in_script_contract_scope();

[[noreturn]] void contract_fail(const char* message);

}  // namespace engine_core
