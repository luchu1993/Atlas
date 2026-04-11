#include "clrscript/clr_error.hpp"
#include "clrscript/clr_invoke.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace atlas::test
{

// ============================================================================
// Fake C# [UnmanagedCallersOnly] methods (C-linkage function pointers)
// ============================================================================
//
// In unit tests we don't have a live CLR.  We instead bypass ClrHost and
// directly inject function pointers into ClrStaticMethod via a test-only
// helper.  The methods mimic what real [UnmanagedCallersOnly] entry points
// would look like.

namespace
{

// ---- add(int, int) → int  (success) ----------------------------------------
int fake_add(int a, int b)
{
    return a + b;
}

// ---- multiply(float, float) → float ----------------------------------------
float fake_multiply(float a, float b)
{
    return a * b;
}

// ---- void action(int) → void -----------------------------------------------
static int s_last_action_arg = 0;
void fake_action(int x)
{
    s_last_action_arg = x;
}

// ---- fallible method that succeeds: returns 0 (int convention) -------------
int fake_succeeds(int /*x*/)
{
    return 0;
}

// ---- fallible method that fails: sets CLR error and returns -1 --------------
int fake_fails(int /*x*/)
{
    clr_error_set(42, "fake failure", 12);
    return -1;
}

// ---- inject a function pointer directly (test-only) -------------------------
// ClrStaticMethod normally requires ClrHost, but for unit tests we want to
// bypass it.  We use a friend-class trick via a tiny shim template.

template <typename Ret, typename... Args>
void inject_fn(ClrStaticMethod<Ret, Args...>& method, Ret (*fn)(Args...))
{
    // We reconstruct the method by move-assigning from a freshly-built wrapper.
    // Since the private fn_ member is inaccessible, we bind through a trivial
    // mock ClrHost that returns our pre-built pointer.
    //
    // Alternative: expose a test-only constructor.  For now, we use the public
    // bind() API with a mock assembly path and rely on the fact that ClrHost
    // is not constructed (is_initialized() == false).  Since ClrStaticMethod
    // does not call ClrHost under the hood in tests, we instead store through
    // a union cast.  To keep production code pristine, we use memcpy.

    static_assert(sizeof(method) == sizeof(fn),
                  "ClrStaticMethod<> must be exactly one function pointer in size");
    std::memcpy(&method, &fn, sizeof(fn));
}

}  // namespace

// ============================================================================
// is_bound()
// ============================================================================

TEST(ClrInvoke, DefaultIsNotBound)
{
    ClrStaticMethod<int, int, int> m;
    EXPECT_FALSE(m.is_bound());
}

TEST(ClrInvoke, AfterInjectIsNotBound_DirectBind)
{
    // We skip host-based bind() here — verify the flag works with inject_fn.
    ClrStaticMethod<int, int, int> m;
    inject_fn(m, &fake_add);
    EXPECT_TRUE(m.is_bound());
}

TEST(ClrInvoke, ResetClearsBound)
{
    ClrStaticMethod<int, int, int> m;
    inject_fn(m, &fake_add);
    EXPECT_TRUE(m.is_bound());
    m.reset();
    EXPECT_FALSE(m.is_bound());
}

// ============================================================================
// invoke() — int-returning (error convention)
// ============================================================================

TEST(ClrInvoke, InvokeWhenNotBoundReturnsError)
{
    ClrStaticMethod<int, int, int> m;
    auto res = m.invoke(1, 2);
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message().find("not bound"), std::string::npos);
}

TEST(ClrInvoke, InvokeIntAddSuccess)
{
    ClrStaticMethod<int, int, int> m;
    inject_fn(m, &fake_add);
    auto res = m.invoke(3, 4);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 7);
}

// ============================================================================
// invoke() — int-returning fallible convention (0=ok, -1=error)
// ============================================================================

TEST(ClrInvoke, FallibleSuccessReturnsZero)
{
    clear_clr_error();
    ClrFallibleMethod<int> m;
    inject_fn(m, &fake_succeeds);
    auto res = m.invoke(99);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 0);
}

TEST(ClrInvoke, FallibleFailurePropagatesClrError)
{
    clear_clr_error();
    ClrFallibleMethod<int> m;
    inject_fn(m, &fake_fails);
    auto res = m.invoke(0);
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message().find("fake failure"), std::string::npos);
    // Buffer should be cleared after read_clr_error().
    EXPECT_FALSE(has_clr_error());
}

// ============================================================================
// invoke() — void-returning
// ============================================================================

TEST(ClrInvoke, VoidMethodInvoked)
{
    ClrVoidMethod<int> m;
    inject_fn(m, &fake_action);
    s_last_action_arg = 0;
    auto res = m.invoke(123);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(s_last_action_arg, 123);
}

TEST(ClrInvoke, VoidMethodNotBoundReturnsError)
{
    ClrVoidMethod<int> m;
    auto res = m.invoke(0);
    EXPECT_FALSE(res.has_value());
}

// ============================================================================
// invoke() — float-returning (non-int, non-void)
// ============================================================================

TEST(ClrInvoke, FloatMethodInvoked)
{
    ClrStaticMethod<float, float, float> m;
    inject_fn(m, &fake_multiply);
    auto res = m.invoke(3.0f, 4.0f);
    ASSERT_TRUE(res.has_value());
    EXPECT_FLOAT_EQ(*res, 12.0f);
}

// ============================================================================
// move semantics
// ============================================================================

TEST(ClrInvoke, MoveTransfersBinding)
{
    ClrStaticMethod<int, int, int> m1;
    inject_fn(m1, &fake_add);
    EXPECT_TRUE(m1.is_bound());

    ClrStaticMethod<int, int, int> m2 = std::move(m1);
    EXPECT_TRUE(m2.is_bound());

    auto res = m2.invoke(10, 20);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 30);
}

TEST(ClrInvoke, MoveAssignTransfersBinding)
{
    ClrStaticMethod<float, float, float> m1;
    inject_fn(m1, &fake_multiply);

    ClrStaticMethod<float, float, float> m2;
    m2 = std::move(m1);
    EXPECT_TRUE(m2.is_bound());

    auto res = m2.invoke(2.0f, 5.0f);
    ASSERT_TRUE(res.has_value());
    EXPECT_FLOAT_EQ(*res, 10.0f);
}

}  // namespace atlas::test
