#pragma once

#include "pyscript/py_object.hpp"
#include "foundation/error.hpp"

#include <string>

namespace atlas
{

// ============================================================================
// Python error handling utilities
// ============================================================================

// Check if a Python exception is pending. If so, format it and return as Error.
// Clears the Python exception state.
[[nodiscard]] auto check_python_error() -> Result<void>;

// Format the current Python exception as a string (Type: message).
// Does NOT clear the exception.
[[nodiscard]] auto format_python_error() -> std::string;

// Clear the current Python exception.
void clear_python_error();

// Set a Python exception from an Atlas Error.
void set_python_error(const Error& error);

} // namespace atlas
