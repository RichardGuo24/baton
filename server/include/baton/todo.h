#pragma once

#include <stdexcept>
#include <string>

namespace baton {

// Placeholder for code that has not been written yet. Delete calls as you implement things.
[[noreturn]] inline void todo(const char* what) {
    throw std::logic_error(std::string("not implemented yet: ") + what);
}

}  // namespace baton
