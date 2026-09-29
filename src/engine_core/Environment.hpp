#pragma once

// Environment variables, read the same way on every platform. MSVC deprecates
// getenv in favor of _dupenv_s, which copies the value out.

#include <cstdlib>
#include <optional>
#include <string>

namespace engine_core {

// The variable's value, or nullopt when it is not set.
inline std::optional<std::string> environment_variable(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

}  // namespace engine_core
