#pragma once

#include "pyscript/py_object.hpp"
#include "foundation/error.hpp"

#include <string>

namespace atlas
{

// ============================================================================
// Python error handling utilities
// ============================================================================

// Check if a Python exception is pending. If so, format it, clear it,
// and return as Error. If no exception is pending, returns success.
[[nodiscard]] auto check_python_error() -> Result<void>;

// Format the current Python exception as "TypeName: message".
// Temporarily fetches and restores the exception — caller must still
// call clear_python_error() or PyErr_Clear() to actually clear it.
[[nodiscard]] auto format_python_error() -> std::string;

// Clear the current Python exception.
void clear_python_error();

// Set a Python exception from an Atlas Error.
void set_python_error(const Error& error);

} // namespace atlas
