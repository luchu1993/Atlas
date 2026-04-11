#include "machined/process_registry.hpp"

#include <gtest/gtest.h>

using namespace atlas;
using namespace atlas::machined;

namespace
{

// Build a minimal ProcessEntry without a real Channel
ProcessEntry make_entry(ProcessType type, std::string name, uint32_t pid,
                        Channel* channel = nullptr)
{
    ProcessEntry e;
    e.process_type = type;
    e.name = std::move(name);
    e.internal_addr = Address(0x7F000001, 7100);
    e.external_addr = Address(0, 0);
    e.pid = pid;
    e.channel = channel;
    return e;
}

}  // namespace

// ============================================================================
// register_process
// ============================================================================

TEST(ProcessRegistry, RegisterAndSize)
{
    ProcessRegistry reg;
    EXPECT_EQ(reg.size(), 0u);

    EXPECT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100)));
    EXPECT_EQ(reg.size(), 1u);

    EXPECT_TRUE(reg.register_process(make_entry(ProcessType::CellApp, "cellapp-1", 200)));
    EXPECT_EQ(reg.size(), 2u);
}

TEST(ProcessRegistry, DuplicateNameRejected)
{
    ProcessRegistry reg;
    EXPECT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100)));
    // Same type + name → rejected
    EXPECT_FALSE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 101)));
    EXPECT_EQ(reg.size(), 1u);
}

TEST(ProcessRegistry, SameNameDifferentTypeAllowed)
{
    ProcessRegistry reg;
    EXPECT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "app-1", 100)));
    // Different type → allowed
    EXPECT_TRUE(reg.register_process(make_entry(ProcessType::CellApp, "app-1", 200)));
    EXPECT_EQ(reg.size(), 2u);
}

// Use a fake pointer value for channel uniqueness test
TEST(ProcessRegistry, DuplicateChannelRejected)
{
    ProcessRegistry reg;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0xDEAD});
    EXPECT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100, fake_ch)));
    // Different name but same channel → rejected
    EXPECT_FALSE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-2", 101, fake_ch)));
    EXPECT_EQ(reg.size(), 1u);
}

// ============================================================================
// find_by_type
// ============================================================================

TEST(ProcessRegistry, FindByType)
{
    ProcessRegistry reg;
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100)));
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-2", 101)));
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::CellApp, "cellapp-1", 200)));

    auto baseapps = reg.find_by_type(ProcessType::BaseApp);
    ASSERT_EQ(baseapps.size(), 2u);

    auto cellapps = reg.find_by_type(ProcessType::CellApp);
    ASSERT_EQ(cellapps.size(), 1u);
    EXPECT_EQ(cellapps[0].name, "cellapp-1");

    auto dbs = reg.find_by_type(ProcessType::DBApp);
    EXPECT_TRUE(dbs.empty());
}

// ============================================================================
// find_by_name
// ============================================================================

TEST(ProcessRegistry, FindByName)
{
    ProcessRegistry reg;
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100)));

    auto found = reg.find_by_name(ProcessType::BaseApp, "baseapp-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->pid, 100u);

    auto not_found = reg.find_by_name(ProcessType::BaseApp, "no-such");
    EXPECT_FALSE(not_found.has_value());
}

// ============================================================================
// find_by_channel
// ============================================================================

TEST(ProcessRegistry, FindByChannel)
{
    ProcessRegistry reg;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0xBEEF});
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::CellApp, "cellapp-1", 200, fake_ch)));

    auto found = reg.find_by_channel(fake_ch);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "cellapp-1");

    auto not_found = reg.find_by_channel(nullptr);
    EXPECT_FALSE(not_found.has_value());
}

// ============================================================================
// unregister_by_channel
// ============================================================================

TEST(ProcessRegistry, UnregisterByChannel)
{
    ProcessRegistry reg;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0x1234});
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100, fake_ch)));
    EXPECT_EQ(reg.size(), 1u);

    auto removed = reg.unregister_by_channel(fake_ch);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->name, "baseapp-1");
    EXPECT_EQ(reg.size(), 0u);

    // Second unregister returns nullopt
    auto gone = reg.unregister_by_channel(fake_ch);
    EXPECT_FALSE(gone.has_value());
}

// ============================================================================
// unregister_by_name
// ============================================================================

TEST(ProcessRegistry, UnregisterByName)
{
    ProcessRegistry reg;
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100)));
    EXPECT_EQ(reg.size(), 1u);

    auto removed = reg.unregister_by_name(ProcessType::BaseApp, "baseapp-1");
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->pid, 100u);
    EXPECT_EQ(reg.size(), 0u);

    auto gone = reg.unregister_by_name(ProcessType::BaseApp, "baseapp-1");
    EXPECT_FALSE(gone.has_value());
}

// ============================================================================
// update_load
// ============================================================================

TEST(ProcessRegistry, UpdateLoad)
{
    ProcessRegistry reg;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0xAAAA});
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100, fake_ch)));

    reg.update_load(fake_ch, 0.8f, 500);

    auto found = reg.find_by_channel(fake_ch);
    ASSERT_TRUE(found.has_value());
    EXPECT_FLOAT_EQ(found->load, 0.8f);
}

// ============================================================================
// for_each
// ============================================================================

TEST(ProcessRegistry, ForEach)
{
    ProcessRegistry reg;
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::BaseApp, "baseapp-1", 100)));
    ASSERT_TRUE(reg.register_process(make_entry(ProcessType::CellApp, "cellapp-1", 200)));

    int count = 0;
    reg.for_each([&](const ProcessEntry&) { ++count; });
    EXPECT_EQ(count, 2);
}
