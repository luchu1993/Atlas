#include <gtest/gtest.h>
#include "network/event_dispatcher.hpp"
#include "network/socket.hpp"

#include <array>
#include <chrono>
#include <cstddef>

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
    auto handle = dispatcher.add_timer(Milliseconds(0), [&](TimerHandle)
    {
        ++fired;
    });
    (void)handle;

    dispatcher.process_once();
    EXPECT_EQ(fired, 1);
}

TEST(EventDispatcher, RepeatingTimerMultipleFires)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    int count = 0;
    auto handle = dispatcher.add_repeating_timer(Milliseconds(1), [&](TimerHandle)
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
    dispatcher.add_frequent_task(&task);

    dispatcher.process_once();
    dispatcher.process_once();
    dispatcher.process_once();

    EXPECT_EQ(task.count, 3);

    dispatcher.remove_frequent_task(&task);
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
    dispatcher.add_frequent_task(&task);

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
    auto handle = dispatcher.add_timer(Milliseconds(0), [&](TimerHandle)
    {
        dispatcher.stop();
    });
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
            // Deregister self during callback — should be safe (deferred)
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
