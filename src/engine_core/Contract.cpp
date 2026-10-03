#include "Contract.hpp"

#include "types.hpp"

#include <cstdio>
#include <cstdlib>

namespace engine_core {
namespace {

ContractHandler gHandler = nullptr;
thread_local ThreadRole gRole = ThreadRole::Unknown;
thread_local int gScriptDepth = 0;

}  // namespace

void set_thread_role(ThreadRole role) { gRole = role; }

ThreadRole thread_role() { return gRole; }

void set_contract_handler(ContractHandler handler) { gHandler = handler; }

ScriptContractScope::ScriptContractScope() { ++gScriptDepth; }

ScriptContractScope::~ScriptContractScope() { --gScriptDepth; }

bool in_script_contract_scope() { return gScriptDepth > 0; }

void contract_fail(const char* message) {
    if (gScriptDepth > 0) {
        throw ContractViolation(message);
    }
    if (gHandler != nullptr) {
        gHandler(message);
    }
    std::fprintf(stderr, "engine_core contract: %s\n", message);
    std::abort();
}

}  // namespace engine_core
