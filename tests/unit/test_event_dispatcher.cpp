#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>

#include <gtest/gtest.h>

#include "network/event_dispatcher.h"
#include "network/socket.h"

using namespace atlas;

TEST(EventDispatcher, ProcessOnceWithNoRegistrations) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(1));
  dispatcher.ProcessOnce();  // should not crash
}

TEST(EventDispatcher, TimerFires) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(1));

  int fired = 0;
  auto handle = dispatcher.AddTimer(Milliseconds(0), [&](TimerHandle) { ++fired; });
  (void)handle;

  dispatcher.ProcessOnce();
  EXPECT_EQ(fired, 1);
}

TEST(EventDispatcher, RepeatingTimerMultipleFires) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(1));

  int count = 0;
  auto handle = dispatcher.AddRepeatingTimer(Milliseconds(1), [&](TimerHandle) {
    ++count;
    if (count >= 3) {
      // Will be cancelled externally
    }
  });

  // Process several times
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    dispatcher.ProcessOnce();
  }

  EXPECT_GE(count, 3);
  dispatcher.CancelTimer(handle);
}

TEST(EventDispatcher, FrequentTaskRunsEveryIteration) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(1));

  struct CountTask : FrequentTask {
    int count = 0;
    void DoTask() override { ++count; }
  };

  CountTask task;
  auto reg = dispatcher.AddFrequentTask(&task);

  dispatcher.ProcessOnce();
  dispatcher.ProcessOnce();
  dispatcher.ProcessOnce();

  EXPECT_EQ(task.count, 3);

  reg.Reset();  // explicit deregistration
  dispatcher.ProcessOnce();
  EXPECT_EQ(task.count, 3);  // no longer called
}

TEST(EventDispatcher, FrequentTaskSafeRemovalDuringIteration) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(1));

  struct SelfRemovingTask : FrequentTask {
    EventDispatcher* disp;
    int count = 0;
    void DoTask() override {
      ++count;
      if (count == 2) {
        disp->RemoveFrequentTask(this);
      }
    }
  };

  SelfRemovingTask task;
  task.disp = &dispatcher;
  auto reg = dispatcher.AddFrequentTask(&task);

  dispatcher.ProcessOnce();  // count=1
  dispatcher.ProcessOnce();  // count=2, self-remove
  dispatcher.ProcessOnce();  // should not call task
  EXPECT_EQ(task.count, 2);
}

TEST(EventDispatcher, StopDuringRun) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(1));

  // Timer that stops the loop after 1 fire
  auto handle = dispatcher.AddTimer(Milliseconds(0), [&](TimerHandle) { dispatcher.Stop(); });
  (void)handle;

  dispatcher.Run();  // should return after timer fires and calls stop()
  EXPECT_FALSE(dispatcher.IsRunning());
}

TEST(EventDispatcher, IoReadNotification) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(50));

  // Create UDP socketpair for testing IO
  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());

  auto receiver = Socket::CreateUdp();
  ASSERT_TRUE(receiver.HasValue());
  ASSERT_TRUE(receiver->Bind(Address("127.0.0.1", 0)).HasValue());
  auto recv_addr = receiver->LocalAddress().Value();

  bool read_triggered = false;
  auto reg = dispatcher.RegisterReader(receiver->Fd(), [&](FdHandle, IOEvent events) {
    if ((events & IOEvent::kReadable) != IOEvent::kNone) {
      read_triggered = true;
    }
  });
  ASSERT_TRUE(reg.HasValue());

  // Send data to trigger read event
  std::array<std::byte, 4> data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  auto sent = sender->SendTo(data, recv_addr);
  ASSERT_TRUE(sent.HasValue());

  // Process — should detect readable
  dispatcher.ProcessOnce();
  EXPECT_TRUE(read_triggered);

  (void)dispatcher.Deregister(receiver->Fd());
}

// ============================================================================
// Review issue #1: SelectPoller callback freshness — re-registration during
// callback should use the new callback, not the stale snapshot.
// ============================================================================

TEST(EventDispatcher, ReRegisterDuringCallbackDoesNotCrash) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(50));

  auto sock = Socket::CreateUdp();
  ASSERT_TRUE(sock.HasValue());
  ASSERT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto sock_addr = sock->LocalAddress().Value();

  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());

  bool callback_a_called = false;
  bool callback_b_called = false;
  bool reregister_ok = false;

  // Register callback A. Inside A, deregister then re-register with callback B.
  auto reg = dispatcher.RegisterReader(sock->Fd(), [&](FdHandle fd, IOEvent) {
    callback_a_called = true;

    // Drain the data so socket is no longer ready after
    // this
    std::array<std::byte, 64> drain{};
    (void)sock->RecvFrom(drain);

    // Deregister and re-register with a different
    // callback
    auto dereg = dispatcher.Deregister(fd);
    if (dereg.HasValue()) {
      auto reg2 = dispatcher.RegisterReader(fd, [&](FdHandle, IOEvent) {
        callback_b_called = true;
        std::array<std::byte, 64> drain2{};
        (void)sock->RecvFrom(drain2);
      });
      reregister_ok = reg2.HasValue();
    }
  });
  ASSERT_TRUE(reg.HasValue());

  // Send data to trigger callback A
  std::array<std::byte, 1> data = {std::byte{0xAA}};
  (void)sender->SendTo(data, sock_addr);

  dispatcher.ProcessOnce();
  EXPECT_TRUE(callback_a_called);
  EXPECT_TRUE(reregister_ok) << "Re-registration during callback should succeed";

  // Send again to trigger callback B
  (void)sender->SendTo(data, sock_addr);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  dispatcher.ProcessOnce();
  EXPECT_TRUE(callback_b_called);

  (void)dispatcher.Deregister(sock->Fd());
}

// ============================================================================
// Review issue #5: EventDispatcher re-registration during callback — verify
// deregister + re-register with new callback during poll is safe via deferred
// deregistration, and the NEW callback is used on next poll.
// ============================================================================

TEST(EventDispatcher, ReRegisterNewCallbackUsedOnNextPoll) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(50));

  auto receiver = Socket::CreateUdp();
  ASSERT_TRUE(receiver.HasValue());
  ASSERT_TRUE(receiver->Bind(Address("127.0.0.1", 0)).HasValue());
  auto recv_addr = receiver->LocalAddress().Value();

  auto sender = Socket::CreateUdp();
  ASSERT_TRUE(sender.HasValue());

  int callback_a_count = 0;
  int callback_b_count = 0;

  // Shared lambda for callback B (captured by reference)
  std::function<void(FdHandle, IOEvent)> callback_b = [&](FdHandle, IOEvent) {
    ++callback_b_count;
    std::array<std::byte, 64> drain{};
    (void)receiver->RecvFrom(drain);
  };

  // Callback A: deregister self, re-register with callback B, then send
  // more data so B gets triggered on next process_once().
  auto reg = dispatcher.RegisterReader(receiver->Fd(), [&](FdHandle fd, IOEvent) {
    ++callback_a_count;
    std::array<std::byte, 64> drain{};
    (void)receiver->RecvFrom(drain);

    // Swap to callback B
    (void)dispatcher.Deregister(fd);
    auto reg2 = dispatcher.RegisterReader(fd, callback_b);
    (void)reg2;

    // Send more data so fd is ready on next poll
    std::array<std::byte, 1> pkt = {std::byte{0xBB}};
    (void)sender->SendTo(pkt, recv_addr);
  });
  ASSERT_TRUE(reg.HasValue());

  // Trigger callback A
  std::array<std::byte, 1> data = {std::byte{0xAA}};
  (void)sender->SendTo(data, recv_addr);
  dispatcher.ProcessOnce();

  EXPECT_EQ(callback_a_count, 1);
  EXPECT_EQ(callback_b_count, 0);

  // Next process_once() should invoke callback B (not A)
  dispatcher.ProcessOnce();

  EXPECT_EQ(callback_a_count, 1);  // A should NOT be called again
  EXPECT_EQ(callback_b_count, 1);  // B should be called

  (void)dispatcher.Deregister(receiver->Fd());
}

TEST(EventDispatcher, DeregisterDuringCallback) {
  EventDispatcher dispatcher("test");
  dispatcher.SetMaxPollWait(Milliseconds(50));

  auto sock1 = Socket::CreateUdp();
  ASSERT_TRUE(sock1.HasValue());
  ASSERT_TRUE(sock1->Bind(Address("127.0.0.1", 0)).HasValue());

  auto sock2 = Socket::CreateUdp();
  ASSERT_TRUE(sock2.HasValue());
  ASSERT_TRUE(sock2->Bind(Address("127.0.0.1", 0)).HasValue());
  auto addr2 = sock2->LocalAddress().Value();

  bool callback_called = false;
  auto reg = dispatcher.RegisterReader(sock2->Fd(), [&](FdHandle fd, IOEvent) {
    callback_called = true;
    // Deregister self during callback — should be safe
    // (deferred)
    (void)dispatcher.Deregister(fd);
  });
  ASSERT_TRUE(reg.HasValue());

  // Send data to trigger
  std::array<std::byte, 1> data = {std::byte{0xFF}};
  (void)sock1->SendTo(data, addr2);

  // Should not crash
  dispatcher.ProcessOnce();
  EXPECT_TRUE(callback_called);
}
