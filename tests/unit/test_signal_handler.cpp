#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <gtest/gtest.h>

#include "platform/signal_handler.h"

using namespace atlas;
using namespace std::chrono_literals;

// Helper: call DispatchPendingSignals() in a polling loop until the condition
// is met or the timeout expires.  Avoids flaky sleep_for() patterns.
static bool poll_dispatch(std::atomic<int>& counter, int expected,
                          std::chrono::milliseconds timeout = 500ms) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    DispatchPendingSignals();
    if (counter.load() >= expected) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

// ============================================================================
// Basic install / dispatch
// ============================================================================

TEST(SignalHandler, InstallAndDispatch) {
  std::atomic<int> fired{0};
  InstallSignalHandler(Signal::kInterrupt, [&](Signal s) {
    EXPECT_EQ(s, Signal::kInterrupt);
    ++fired;
  });

  // Simulate the OS marking a signal as pending (same as the raw handler does)
  // We do this by raising SIGINT which triggers our installed handler.
  std::raise(SIGINT);

  EXPECT_TRUE(poll_dispatch(fired, 1));
  EXPECT_EQ(fired.load(), 1);

  RemoveSignalHandler(Signal::kInterrupt);
}

TEST(SignalHandler, RemoveStopsCallback) {
  std::atomic<int> fired{0};
  InstallSignalHandler(Signal::kInterrupt, [&](Signal) { ++fired; });

  // Raise signal → pending flag is set
  std::raise(SIGINT);

  // Remove BEFORE dispatching — pending flag exists but handler is gone
  RemoveSignalHandler(Signal::kInterrupt);

  DispatchPendingSignals();
  DispatchPendingSignals();

  EXPECT_EQ(fired.load(), 0);
}

TEST(SignalHandler, MultipleSignalsQueued) {
  std::atomic<int> int_count{0};
  std::atomic<int> term_count{0};

  InstallSignalHandler(Signal::kInterrupt, [&](Signal) { ++int_count; });
  InstallSignalHandler(Signal::kTerminate, [&](Signal) { ++term_count; });

  std::raise(SIGINT);
  std::raise(SIGTERM);

  EXPECT_TRUE(poll_dispatch(int_count, 1));
  EXPECT_TRUE(poll_dispatch(term_count, 1));

  EXPECT_EQ(int_count.load(), 1);
  EXPECT_EQ(term_count.load(), 1);

  RemoveSignalHandler(Signal::kInterrupt);
  RemoveSignalHandler(Signal::kTerminate);
}

TEST(SignalHandler, ReplaceCallback) {
  std::atomic<int> old_fired{0};
  std::atomic<int> new_fired{0};

  InstallSignalHandler(Signal::kInterrupt, [&](Signal) { ++old_fired; });
  // Replace with a new callback
  InstallSignalHandler(Signal::kInterrupt, [&](Signal) { ++new_fired; });

  std::raise(SIGINT);
  EXPECT_TRUE(poll_dispatch(new_fired, 1));

  EXPECT_EQ(old_fired.load(), 0);
  EXPECT_EQ(new_fired.load(), 1);

  RemoveSignalHandler(Signal::kInterrupt);
}

TEST(SignalHandler, DispatchWithNoPendingIsNoop) {
  // Calling dispatch when nothing is pending must not crash
  // (Don't raise a real signal with SIG_DFL installed — that would kill the process)
  DispatchPendingSignals();
  DispatchPendingSignals();
}
