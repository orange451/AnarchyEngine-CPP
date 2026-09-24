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

[[noreturn]] void contract_fail(const char* message);

}  // namespace engine_core
