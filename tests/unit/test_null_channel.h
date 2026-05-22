#ifndef ATLAS_TESTS_UNIT_TEST_NULL_CHANNEL_H_
#define ATLAS_TESTS_UNIT_TEST_NULL_CHANNEL_H_

#include <cstdint>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

#include "foundation/error.h"
#include "foundation/intrusive_ptr.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "platform/io_poller.h"

namespace atlas::test_support {

// Refcounted no-op Channel; entity holders now hold IntrusivePtr so an
// integer-cast Channel* would crash on add_ref/release.
class TestNullChannel final : public Channel {
 public:
  using Channel::Channel;
  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    return data.size();
  }
};

// Same tag returns the same pointer so identity comparisons still hold;
// tag=0 always allocates a fresh channel.
inline auto FakeChannel(uintptr_t tag = 0) -> Channel* {
  static EventDispatcher d{"test_null_channel"};
  static InterfaceTable t;
  static std::unordered_map<uintptr_t, IntrusivePtr<TestNullChannel>> by_tag;
  static std::vector<IntrusivePtr<TestNullChannel>> anonymous;
  if (tag == 0) {
    anonymous.push_back(make_intrusive<TestNullChannel>(d, t, Address{}));
    return anonymous.back().get();
  }
  auto it = by_tag.find(tag);
  if (it == by_tag.end()) {
    it = by_tag.emplace(tag, make_intrusive<TestNullChannel>(d, t, Address{})).first;
  }
  return it->second.get();
}

}  // namespace atlas::test_support

#endif  // ATLAS_TESTS_UNIT_TEST_NULL_CHANNEL_H_
