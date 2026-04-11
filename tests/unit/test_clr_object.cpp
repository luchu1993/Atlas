#include "clrscript/clr_error.hpp"
#include "clrscript/clr_object.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace atlas::test
{

// ============================================================================
// Fake vtable implementations
// ============================================================================

namespace
{

// Sentinel value used as fake GCHandles in tests.
// Each "allocation" bumps a counter so we can track frees.
static std::atomic<int64_t> g_alloc_count{0};
static std::atomic<int64_t> g_free_count{0};

// We pass a non-null pointer as the "GCHandle".  Use incrementing indices cast
// to void* — never dereferenced.
void* make_fake_handle()
{
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    // Return a non-null pointer (index as address; never dereferenced).
    return reinterpret_cast<void*>(static_cast<uintptr_t>(g_alloc_count.load()));
}

void fake_free_handle(void* /*handle*/)
{
    g_free_count.fetch_add(1, std::memory_order_relaxed);
}

int32_t fake_get_type_name(void* /*handle*/, char* buf, int32_t buf_len)
{
    const char* name = "FakeType";
    const int32_t len = static_cast<int32_t>(std::strlen(name));
    if (buf == nullptr || buf_len <= 0)
        return len;
    const int32_t copy_len = std::min(len, buf_len);
    std::memcpy(buf, name, static_cast<std::size_t>(copy_len));
    return copy_len;
}

uint8_t fake_is_none(void* handle)
{
    return handle == nullptr ? 1 : 0;
}

int32_t fake_to_int64(void* /*handle*/, int64_t* out)
{
    *out = 42LL;
    return 0;
}

int32_t fake_to_double(void* /*handle*/, double* out)
{
    *out = 3.14;
    return 0;
}

int32_t fake_to_string(void* /*handle*/, char* buf, int32_t buf_len)
{
    const char* str = "hello";
    const int32_t len = static_cast<int32_t>(std::strlen(str));
    if (buf == nullptr || buf_len <= 0)
        return len;
    const int32_t copy_len = std::min(len, buf_len);
    std::memcpy(buf, str, static_cast<std::size_t>(copy_len));
    return copy_len;
}

int32_t fake_to_bool(void* /*handle*/, uint8_t* out)
{
    *out = 1;
    return 0;
}

// ---- Error-returning variants -----------------------------------------------

int32_t error_to_int64(void* /*handle*/, int64_t* /*out*/)
{
    clr_error_set(-1, "int64 error", 11);
    return -1;
}

int32_t error_to_double(void* /*handle*/, double* /*out*/)
{
    clr_error_set(-1, "double error", 12);
    return -1;
}

int32_t error_to_string(void* /*handle*/, char* /*buf*/, int32_t /*buf_len*/)
{
    clr_error_set(-1, "string error", 12);
    return -1;
}

int32_t error_to_bool(void* /*handle*/, uint8_t* /*out*/)
{
    clr_error_set(-1, "bool error", 10);
    return -1;
}

// ---- Build vtables ----------------------------------------------------------

ClrObjectVTable make_normal_vtable()
{
    ClrObjectVTable vt{};
    vt.free_handle = fake_free_handle;
    vt.get_type_name = fake_get_type_name;
    vt.is_none = fake_is_none;
    vt.to_int64 = fake_to_int64;
    vt.to_double = fake_to_double;
    vt.to_string = fake_to_string;
    vt.to_bool = fake_to_bool;
    return vt;
}

ClrObjectVTable make_error_vtable()
{
    ClrObjectVTable vt{};
    vt.free_handle = fake_free_handle;
    vt.get_type_name = fake_get_type_name;
    vt.is_none = fake_is_none;
    vt.to_int64 = error_to_int64;
    vt.to_double = error_to_double;
    vt.to_string = error_to_string;
    vt.to_bool = error_to_bool;
    return vt;
}

}  // namespace

// ============================================================================
// Fixture: register the vtable and reset counters before each test
// ============================================================================

class ClrObjectTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        set_clr_object_vtable(make_normal_vtable());
        g_alloc_count.store(0);
        g_free_count.store(0);
        clear_clr_error();
    }
};

// ============================================================================
// Lifecycle / ownership
// ============================================================================

TEST_F(ClrObjectTest, DestructorFreesHandle)
{
    {
        ClrObject obj{make_fake_handle()};
        EXPECT_EQ(g_free_count.load(), 0);
    }
    EXPECT_EQ(g_free_count.load(), 1);
}

TEST_F(ClrObjectTest, MoveConstructorTransfersOwnership)
{
    ClrObject src{make_fake_handle()};
    auto* handle = src.gc_handle();

    ClrObject dst{std::move(src)};
    EXPECT_EQ(dst.gc_handle(), handle);
    EXPECT_EQ(src.gc_handle(), nullptr);  // NOLINT(bugprone-use-after-move)

    // Only dst goes out of scope → exactly one free.
}

TEST_F(ClrObjectTest, MoveAssignmentTransfersOwnership)
{
    ClrObject a{make_fake_handle()};
    ClrObject b{make_fake_handle()};

    // b takes ownership of a's handle; a's old handle is freed.
    // Actually the move assignment releases b's existing handle first.
    a = std::move(b);
    // b's old handle is freed by a's release() in operator=.
    EXPECT_EQ(g_free_count.load(), 1);
    EXPECT_EQ(b.gc_handle(), nullptr);  // NOLINT(bugprone-use-after-move)
}

TEST_F(ClrObjectTest, SelfMoveAssignmentIsNoOp)
{
    ClrObject obj{make_fake_handle()};
    auto* handle = obj.gc_handle();

    // Suppress Clang-Tidy self-assign warning for the test.
    auto& ref = obj;
    obj = std::move(ref);  // self-move

    // Handle must still be valid (not freed).
    EXPECT_EQ(obj.gc_handle(), handle);
    EXPECT_EQ(g_free_count.load(), 0);
}

// ============================================================================
// ScriptObject interface — normal vtable
// ============================================================================

TEST_F(ClrObjectTest, IsNoneReturnsFalseForValidHandle)
{
    ClrObject obj{make_fake_handle()};
    EXPECT_FALSE(obj.is_none());
}

TEST_F(ClrObjectTest, TypeNameReturnsExpected)
{
    ClrObject obj{make_fake_handle()};
    EXPECT_EQ(obj.type_name(), "FakeType");
}

TEST_F(ClrObjectTest, AsIntReturnsValue)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_int();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 42LL);
}

TEST_F(ClrObjectTest, AsDoubleReturnsValue)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_double();
    ASSERT_TRUE(res.has_value());
    EXPECT_DOUBLE_EQ(*res, 3.14);
}

TEST_F(ClrObjectTest, AsStringReturnsValue)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_string();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, "hello");
}

TEST_F(ClrObjectTest, AsBoolReturnsTrue)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_bool();
    ASSERT_TRUE(res.has_value());
    EXPECT_TRUE(*res);
}

TEST_F(ClrObjectTest, IsCallableReturnsFalseInPhase2)
{
    ClrObject obj{make_fake_handle()};
    EXPECT_FALSE(obj.is_callable());
}

TEST_F(ClrObjectTest, ToDebugStringContainsTypeName)
{
    ClrObject obj{make_fake_handle()};
    auto s = obj.to_debug_string();
    EXPECT_NE(s.find("FakeType"), std::string::npos);
}

// ============================================================================
// ScriptObject interface — error vtable
// ============================================================================

class ClrObjectErrorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        set_clr_object_vtable(make_error_vtable());
        clear_clr_error();
    }
};

TEST_F(ClrObjectErrorTest, AsIntPropagatesClrError)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_int();
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message().find("int64 error"), std::string::npos);
}

TEST_F(ClrObjectErrorTest, AsDoublePropagatesClrError)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_double();
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message().find("double error"), std::string::npos);
}

TEST_F(ClrObjectErrorTest, AsStringPropagatesClrError)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_string();
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message().find("string error"), std::string::npos);
}

TEST_F(ClrObjectErrorTest, AsBoolPropagatesClrError)
{
    ClrObject obj{make_fake_handle()};
    auto res = obj.as_bool();
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message().find("bool error"), std::string::npos);
}

// ============================================================================
// Null-handle guard
// ============================================================================

class ClrObjectNullTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        set_clr_object_vtable(make_normal_vtable());
        clear_clr_error();
    }
};

TEST_F(ClrObjectNullTest, MovedFromObjectIsNone)
{
    ClrObject a{make_fake_handle()};
    ClrObject b{std::move(a)};
    EXPECT_TRUE(a.is_none());  // NOLINT(bugprone-use-after-move)
}

TEST_F(ClrObjectNullTest, MovedFromObjectAsIntReturnsError)
{
    ClrObject a{make_fake_handle()};
    ClrObject b{std::move(a)};
    // NOLINT(bugprone-use-after-move)
    auto res = a.as_int();
    EXPECT_FALSE(res.has_value());
}

// ============================================================================
// Debug-mode GCHandleTracker (only compiled with ATLAS_DEBUG)
// ============================================================================

#if ATLAS_DEBUG

TEST_F(ClrObjectTest, TrackerAllocCountIncrements)
{
    int64_t before = GCHandleTracker::alloc_count();
    {
        ClrObject obj{make_fake_handle()};
    }
    EXPECT_EQ(GCHandleTracker::alloc_count(), before + 1);
}

TEST_F(ClrObjectTest, TrackerFreeCountIncrements)
{
    int64_t before = GCHandleTracker::free_count();
    {
        ClrObject obj{make_fake_handle()};
    }
    EXPECT_EQ(GCHandleTracker::free_count(), before + 1);
}

TEST_F(ClrObjectTest, TrackerLeakCountIsZeroAfterConstruct)
{
    int64_t pre_leak = GCHandleTracker::leak_count();
    {
        ClrObject obj{make_fake_handle()};
    }
    EXPECT_EQ(GCHandleTracker::leak_count(), pre_leak);
}

#endif  // ATLAS_DEBUG

// ============================================================================
// Phase 6: Boundary tests
// ============================================================================

TEST_F(ClrObjectTest, DoubleRelease)
{
    auto obj = std::make_unique<ClrObject>(make_fake_handle());
    obj.reset();
    // Object is already destroyed; this test just verifies no double-free crash.
    SUCCEED();
}

TEST_F(ClrObjectTest, MoveFromReleasedObject)
{
    ClrObject a(make_fake_handle());
    ClrObject b(std::move(a));

    EXPECT_TRUE(a.is_none());
    EXPECT_FALSE(b.is_none());

    ClrObject c(std::move(a));
    EXPECT_TRUE(c.is_none());
    EXPECT_TRUE(a.is_none());
}

}  // namespace atlas::test
