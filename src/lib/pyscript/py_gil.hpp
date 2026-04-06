#pragma once

#include "pyscript/py_object.hpp"  // pulls in Python.h

namespace atlas
{

// ============================================================================
// GILGuard — acquire the Global Interpreter Lock
// ============================================================================
//
// Use when calling Python C API from a non-Python thread.
// The GIL is released when the guard goes out of scope.
//
// Thread safety: Safe to use from any thread. The Python interpreter must
// be initialized before constructing a GILGuard.

class GILGuard
{
public:
    GILGuard() noexcept : state_(PyGILState_Ensure()) {}

    ~GILGuard() { PyGILState_Release(state_); }

    GILGuard(const GILGuard&) = delete;
    GILGuard& operator=(const GILGuard&) = delete;
    GILGuard(GILGuard&&) = delete;
    GILGuard& operator=(GILGuard&&) = delete;

private:
    PyGILState_STATE state_;
};

// ============================================================================
// GILRelease — release the Global Interpreter Lock
// ============================================================================
//
// Use during long C++ operations to let other Python threads run.
// The caller must hold the GIL when constructing a GILRelease.
// The GIL is re-acquired when the guard goes out of scope.

class GILRelease
{
public:
    GILRelease() noexcept : state_(PyEval_SaveThread()) {}

    ~GILRelease() { PyEval_RestoreThread(state_); }

    GILRelease(const GILRelease&) = delete;
    GILRelease& operator=(const GILRelease&) = delete;
    GILRelease(GILRelease&&) = delete;
    GILRelease& operator=(GILRelease&&) = delete;

private:
    PyThreadState* state_;
};

}  // namespace atlas
