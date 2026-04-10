#include "network/event_dispatcher.hpp"
#include "network/socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>

using namespace atlas;

TEST(EventDispatcher, ProcessOnceWithNoRegistrations)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));
    dispatcher.process_once();  // should not crash
}

TEST(EventDispatcher, TimerFires)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    int fired = 0;
    auto handle = dispatcher.add_timer(Milliseconds(0), [&](TimerHandle) { ++fired; });
    (void)handle;

    dispatcher.process_once();
    EXPECT_EQ(fired, 1);
}

TEST(EventDispatcher, RepeatingTimerMultipleFires)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    int count = 0;
    auto handle = dispatcher.add_repeating_timer(Milliseconds(1),
                                                 [&](TimerHandle)
                                                 {
                                                     ++count;
                                                     if (count >= 3)
                                                     {
                                                         // Will be cancelled externally
                                                     }
                                                 });

    // Process several times
    for (int i = 0; i < 10; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        dispatcher.process_once();
    }

    EXPECT_GE(count, 3);
    dispatcher.cancel_timer(handle);
}

TEST(EventDispatcher, FrequentTaskRunsEveryIteration)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    struct CountTask : FrequentTask
    {
        int count = 0;
        void do_task() override { ++count; }
    };

    CountTask task;
    auto reg = dispatcher.add_frequent_task(&task);

    dispatcher.process_once();
    dispatcher.process_once();
    dispatcher.process_once();

    EXPECT_EQ(task.count, 3);

    reg.reset();  // explicit deregistration
    dispatcher.process_once();
    EXPECT_EQ(task.count, 3);  // no longer called
}

TEST(EventDispatcher, FrequentTaskSafeRemovalDuringIteration)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    struct SelfRemovingTask : FrequentTask
    {
        EventDispatcher* disp;
        int count = 0;
        void do_task() override
        {
            ++count;
            if (count == 2)
            {
                disp->remove_frequent_task(this);
            }
        }
    };

    SelfRemovingTask task;
    task.disp = &dispatcher;
    auto reg = dispatcher.add_frequent_task(&task);

    dispatcher.process_once();  // count=1
    dispatcher.process_once();  // count=2, self-remove
    dispatcher.process_once();  // should not call task
    EXPECT_EQ(task.count, 2);
}

TEST(EventDispatcher, StopDuringRun)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    // Timer that stops the loop after 1 fire
    auto handle = dispatcher.add_timer(Milliseconds(0), [&](TimerHandle) { dispatcher.stop(); });
    (void)handle;

    dispatcher.run();  // should return after timer fires and calls stop()
    EXPECT_FALSE(dispatcher.is_running());
}

TEST(EventDispatcher, IoReadNotification)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(50));

    // Create UDP socketpair for testing IO
    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());

    auto receiver = Socket::create_udp();
    ASSERT_TRUE(receiver.has_value());
    ASSERT_TRUE(receiver->bind(Address("127.0.0.1", 0)).has_value());
    auto recv_addr = receiver->local_address().value();

    bool read_triggered = false;
    auto reg = dispatcher.register_reader(receiver->fd(),
                                          [&](FdHandle, IOEvent events)
                                          {
                                              if ((events & IOEvent::Readable) != IOEvent::None)
                                              {
                                                  read_triggered = true;
                                              }
                                          });
    ASSERT_TRUE(reg.has_value());

    // Send data to trigger read event
    std::array<std::byte, 4> data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto sent = sender->send_to(data, recv_addr);
    ASSERT_TRUE(sent.has_value());

    // Process — should detect readable
    dispatcher.process_once();
    EXPECT_TRUE(read_triggered);

    (void)dispatcher.deregister(receiver->fd());
}

// ============================================================================
// Review issue #1: SelectPoller callback freshness — re-registration during
// callback should use the new callback, not the stale snapshot.
// ============================================================================

TEST(EventDispatcher, ReRegisterDuringCallbackDoesNotCrash)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(50));

    auto sock = Socket::create_udp();
    ASSERT_TRUE(sock.has_value());
    ASSERT_TRUE(sock->bind(Address("127.0.0.1", 0)).has_value());
    auto sock_addr = sock->local_address().value();

    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());

    bool callback_a_called = false;
    bool callback_b_called = false;
    bool reregister_ok = false;

    // Register callback A. Inside A, deregister then re-register with callback B.
    auto reg = dispatcher.register_reader(sock->fd(),
                                          [&](FdHandle fd, IOEvent)
                                          {
                                              callback_a_called = true;

                                              // Drain the data so socket is no longer ready after
                                              // this
                                              std::array<std::byte, 64> drain{};
                                              (void)sock->recv_from(drain);

                                              // Deregister and re-register with a different
                                              // callback
                                              auto dereg = dispatcher.deregister(fd);
                                              if (dereg.has_value())
                                              {
                                                  auto reg2 = dispatcher.register_reader(
                                                      fd,
                                                      [&](FdHandle, IOEvent)
                                                      {
                                                          callback_b_called = true;
                                                          std::array<std::byte, 64> drain2{};
                                                          (void)sock->recv_from(drain2);
                                                      });
                                                  reregister_ok = reg2.has_value();
                                              }
                                          });
    ASSERT_TRUE(reg.has_value());

    // Send data to trigger callback A
    std::array<std::byte, 1> data = {std::byte{0xAA}};
    (void)sender->send_to(data, sock_addr);

    dispatcher.process_once();
    EXPECT_TRUE(callback_a_called);
    EXPECT_TRUE(reregister_ok) << "Re-registration during callback should succeed";

    // Send again to trigger callback B
    (void)sender->send_to(data, sock_addr);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dispatcher.process_once();
    EXPECT_TRUE(callback_b_called);

    (void)dispatcher.deregister(sock->fd());
}

// ============================================================================
// Review issue #5: EventDispatcher re-registration during callback — verify
// deregister + re-register with new callback during poll is safe via deferred
// deregistration, and the NEW callback is used on next poll.
// ============================================================================

TEST(EventDispatcher, ReRegisterNewCallbackUsedOnNextPoll)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(50));

    auto receiver = Socket::create_udp();
    ASSERT_TRUE(receiver.has_value());
    ASSERT_TRUE(receiver->bind(Address("127.0.0.1", 0)).has_value());
    auto recv_addr = receiver->local_address().value();

    auto sender = Socket::create_udp();
    ASSERT_TRUE(sender.has_value());

    int callback_a_count = 0;
    int callback_b_count = 0;

    // Shared lambda for callback B (captured by reference)
    std::function<void(FdHandle, IOEvent)> callback_b = [&](FdHandle, IOEvent)
    {
        ++callback_b_count;
        std::array<std::byte, 64> drain{};
        (void)receiver->recv_from(drain);
    };

    // Callback A: deregister self, re-register with callback B, then send
    // more data so B gets triggered on next process_once().
    auto reg = dispatcher.register_reader(receiver->fd(),
                                          [&](FdHandle fd, IOEvent)
                                          {
                                              ++callback_a_count;
                                              std::array<std::byte, 64> drain{};
                                              (void)receiver->recv_from(drain);

                                              // Swap to callback B
                                              (void)dispatcher.deregister(fd);
                                              auto reg2 =
                                                  dispatcher.register_reader(fd, callback_b);
                                              (void)reg2;

                                              // Send more data so fd is ready on next poll
                                              std::array<std::byte, 1> pkt = {std::byte{0xBB}};
                                              (void)sender->send_to(pkt, recv_addr);
                                          });
    ASSERT_TRUE(reg.has_value());

    // Trigger callback A
    std::array<std::byte, 1> data = {std::byte{0xAA}};
    (void)sender->send_to(data, recv_addr);
    dispatcher.process_once();

    EXPECT_EQ(callback_a_count, 1);
    EXPECT_EQ(callback_b_count, 0);

    // Next process_once() should invoke callback B (not A)
    dispatcher.process_once();

    EXPECT_EQ(callback_a_count, 1);  // A should NOT be called again
    EXPECT_EQ(callback_b_count, 1);  // B should be called

    (void)dispatcher.deregister(receiver->fd());
}

TEST(EventDispatcher, DeregisterDuringCallback)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(50));

    auto sock1 = Socket::create_udp();
    ASSERT_TRUE(sock1.has_value());
    ASSERT_TRUE(sock1->bind(Address("127.0.0.1", 0)).has_value());

    auto sock2 = Socket::create_udp();
    ASSERT_TRUE(sock2.has_value());
    ASSERT_TRUE(sock2->bind(Address("127.0.0.1", 0)).has_value());
    auto addr2 = sock2->local_address().value();

    bool callback_called = false;
    auto reg = dispatcher.register_reader(sock2->fd(),
                                          [&](FdHandle fd, IOEvent)
                                          {
                                              callback_called = true;
                                              // Deregister self during callback — should be safe
                                              // (deferred)
                                              (void)dispatcher.deregister(fd);
                                          });
    ASSERT_TRUE(reg.has_value());

    // Send data to trigger
    std::array<std::byte, 1> data = {std::byte{0xFF}};
    (void)sock1->send_to(data, addr2);

    // Should not crash
    dispatcher.process_once();
    EXPECT_TRUE(callback_called);
}
