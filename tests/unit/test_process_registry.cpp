#include <gtest/gtest.h>

#include "machined/process_registry.h"

using namespace atlas;
using namespace atlas::machined;

namespace {

// Build a minimal ProcessEntry without a real Channel
ProcessEntry make_entry(ProcessType type, std::string name, uint32_t pid,
                        Channel* channel = nullptr, uint32_t ip = 0x7F000001) {
  ProcessEntry e;
  e.process_type = type;
  e.name = std::move(name);
  e.internal_addr = Address(ip, 7100);
  e.external_addr = Address(0, 0);
  e.pid = pid;
  e.channel = channel;
  return e;
}

}  // namespace

// ============================================================================
// register_process
// ============================================================================

TEST(ProcessRegistry, RegisterAndSize) {
  ProcessRegistry reg;
  EXPECT_EQ(reg.Size(), 0u);

  EXPECT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100)));
  EXPECT_EQ(reg.Size(), 1u);

  EXPECT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kCellApp, "cellapp-1", 200)));
  EXPECT_EQ(reg.Size(), 2u);
}

TEST(ProcessRegistry, DuplicateNameRejected) {
  ProcessRegistry reg;
  EXPECT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100)));
  // Same type + name → rejected
  EXPECT_FALSE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 101)));
  EXPECT_EQ(reg.Size(), 1u);
}

TEST(ProcessRegistry, SameNameDifferentTypeAllowed) {
  ProcessRegistry reg;
  EXPECT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "app-1", 100)));
  // Different type → allowed
  EXPECT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kCellApp, "app-1", 200)));
  EXPECT_EQ(reg.Size(), 2u);
}

// Use a fake pointer value for channel uniqueness test
TEST(ProcessRegistry, DuplicateChannelRejected) {
  ProcessRegistry reg;
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0xDEAD});
  EXPECT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100, fake_ch)));
  // Different name but same channel → rejected
  EXPECT_FALSE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-2", 101, fake_ch)));
  EXPECT_EQ(reg.Size(), 1u);
}

// ============================================================================
// find_by_type
// ============================================================================

TEST(ProcessRegistry, FindByType) {
  ProcessRegistry reg;
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100)));
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-2", 101)));
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kCellApp, "cellapp-1", 200)));

  auto baseapps = reg.FindByType(ProcessType::kBaseApp);
  ASSERT_EQ(baseapps.size(), 2u);

  auto cellapps = reg.FindByType(ProcessType::kCellApp);
  ASSERT_EQ(cellapps.size(), 1u);
  EXPECT_EQ(cellapps[0].name, "cellapp-1");

  auto dbs = reg.FindByType(ProcessType::kDbApp);
  EXPECT_TRUE(dbs.empty());
}

// ============================================================================
// find_by_name
// ============================================================================

TEST(ProcessRegistry, FindByName) {
  ProcessRegistry reg;
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100)));

  auto found = reg.FindByName(ProcessType::kBaseApp, "baseapp-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->pid, 100u);

  auto not_found = reg.FindByName(ProcessType::kBaseApp, "no-such");
  EXPECT_FALSE(not_found.has_value());
}

// ============================================================================
// find_by_channel
// ============================================================================

TEST(ProcessRegistry, FindByChannel) {
  ProcessRegistry reg;
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0xBEEF});
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kCellApp, "cellapp-1", 200, fake_ch)));

  auto found = reg.FindByChannel(fake_ch);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->name, "cellapp-1");

  auto not_found = reg.FindByChannel(nullptr);
  EXPECT_FALSE(not_found.has_value());
}

// ============================================================================
// unregister_by_channel
// ============================================================================

TEST(ProcessRegistry, UnregisterByChannel) {
  ProcessRegistry reg;
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0x1234});
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100, fake_ch)));
  EXPECT_EQ(reg.Size(), 1u);

  auto removed = reg.UnregisterByChannel(fake_ch);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->name, "baseapp-1");
  EXPECT_EQ(reg.Size(), 0u);

  // Second unregister returns nullopt
  auto gone = reg.UnregisterByChannel(fake_ch);
  EXPECT_FALSE(gone.has_value());
}

// ============================================================================
// unregister_by_name
// ============================================================================

TEST(ProcessRegistry, UnregisterByName) {
  ProcessRegistry reg;
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100)));
  EXPECT_EQ(reg.Size(), 1u);

  auto removed = reg.UnregisterByName(ProcessType::kBaseApp, "baseapp-1");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->pid, 100u);
  EXPECT_EQ(reg.Size(), 0u);

  auto gone = reg.UnregisterByName(ProcessType::kBaseApp, "baseapp-1");
  EXPECT_FALSE(gone.has_value());
}

// ============================================================================
// update_load
// ============================================================================

TEST(ProcessRegistry, UpdateLoad) {
  ProcessRegistry reg;
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  Channel* fake_ch = reinterpret_cast<Channel*>(uintptr_t{0xAAAA});
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100, fake_ch)));

  reg.UpdateLoad(fake_ch, 0.8f, 500);

  auto found = reg.FindByChannel(fake_ch);
  ASSERT_TRUE(found.has_value());
  EXPECT_FLOAT_EQ(found->load, 0.8f);
}

TEST(ProcessRegistry, FindTcpChannelByPidDisambiguatesSharedIpHeartbeats) {
  ProcessRegistry reg;
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  Channel* ch1 = reinterpret_cast<Channel*>(uintptr_t{0x1111});
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  Channel* ch2 = reinterpret_cast<Channel*>(uintptr_t{0x2222});

  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100, ch1)));
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-2", 200, ch2)));

  EXPECT_EQ(reg.FindTcpChannelByPid(100, 0x7F000001), ch1);
  EXPECT_EQ(reg.FindTcpChannelByPid(200, 0x7F000001), ch2);
}

// ============================================================================
// for_each
// ============================================================================

TEST(ProcessRegistry, ForEach) {
  ProcessRegistry reg;
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kBaseApp, "baseapp-1", 100)));
  ASSERT_TRUE(reg.RegisterProcess(make_entry(ProcessType::kCellApp, "cellapp-1", 200)));

  int count = 0;
  reg.ForEach([&](const ProcessEntry&) { ++count; });
  EXPECT_EQ(count, 2);
}
