#include "Contract.hpp"

#include "types.hpp"

#include <cstdio>
#include <cstdlib>

namespace engine_core {
namespace {

ContractHandler gHandler = nullptr;
thread_local ThreadRole gRole = ThreadRole::Unknown;

}  // namespace

void set_thread_role(ThreadRole role) { gRole = role; }

ThreadRole thread_role() { return gRole; }

void set_contract_handler(ContractHandler handler) { gHandler = handler; }

void contract_fail(const char* message) {
    if (gHandler != nullptr) {
        gHandler(message);
    }
    std::fprintf(stderr, "engine_core contract: %s\n", message);
    std::abort();
}

}  // namespace engine_core
