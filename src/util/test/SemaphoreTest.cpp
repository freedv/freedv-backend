// Tests for Semaphore: a signal before a wait isn't lost, waitFor() times out
// when nothing signals and returns early when something does, wait() blocks
// until signaled, and no wake-ups are lost in a tight hand-off between
// threads (the pattern ulog_async and the RADE steps use).
//
// Only one pending signal is assumed: the Windows implementation caps the
// count at 1, while macOS and Linux count every signal.

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "../Semaphore.h"

using namespace std::chrono_literals;

namespace {

#define CHECK(expr) checkImpl_((expr), #expr, __LINE__)

bool checkImpl_(bool ok, const char* expr, int line)
{
    if (!ok)
    {
        std::cout << "\n    check failed at line " << line << ": " << expr << "\n    ";
    }
    return ok;
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

using Clock = std::chrono::steady_clock;

bool testSignalBeforeWait()
{
    std::cout << "Test 1 (signal before wait is not lost): ";

    Semaphore sem;
    sem.signal();
    auto start = Clock::now();
    sem.wait();
    bool result = CHECK(Clock::now() - start < 100ms);

    sem.signal();
    start = Clock::now();
    sem.waitFor(2000);
    result &= CHECK(Clock::now() - start < 100ms);

    return report(result);
}

bool testWaitForTimesOut()
{
    std::cout << "Test 2 (waitFor() times out when nothing signals): ";

    Semaphore sem;
    auto start = Clock::now();
    sem.waitFor(200);
    auto elapsed = Clock::now() - start;
    bool result = CHECK(elapsed >= 190ms);
    result &= CHECK(elapsed < 2s);

    // A signal consumed by an earlier wait doesn't satisfy a later one.
    sem.signal();
    sem.wait();
    start = Clock::now();
    sem.waitFor(100);
    result &= CHECK(Clock::now() - start >= 90ms);

    return report(result);
}

// Timeouts over about 2.1 s overflowed an int nanosecond count on macOS and
// returned at once.
bool testLongWaitForTimesOut()
{
    std::cout << "Test 7 (a waitFor() longer than 2.1 s lasts as long as asked): ";

    Semaphore sem;
    auto start = Clock::now();
    sem.waitFor(3000);
    auto elapsed = Clock::now() - start;
    bool result = CHECK(elapsed >= 2900ms);
    result &= CHECK(elapsed < 6s);

    return report(result);
}

bool testWaitForWokenEarly()
{
    std::cout << "Test 3 (waitFor() returns as soon as another thread signals): ";

    Semaphore sem;
    std::thread signaler([&]() {
        std::this_thread::sleep_for(100ms);
        sem.signal();
    });

    auto start = Clock::now();
    sem.waitFor(10000);
    auto elapsed = Clock::now() - start;
    signaler.join();

    bool result = CHECK(elapsed >= 90ms);
    result &= CHECK(elapsed < 2s);

    return report(result);
}

bool testWaitBlocksUntilSignaled()
{
    std::cout << "Test 4 (wait() blocks until signaled): ";

    Semaphore sem;
    std::atomic<bool> returned(false);
    std::thread waiter([&]() {
        sem.wait();
        returned.store(true);
    });

    std::this_thread::sleep_for(200ms);
    bool result = CHECK(!returned.load());

    sem.signal();
    auto start = Clock::now();
    while (!returned.load() && Clock::now() - start < 5s)
    {
        std::this_thread::sleep_for(1ms);
    }
    result &= CHECK(returned.load());
    if (!returned.load())
    {
        // Don't hang the test on a stuck waiter.
        sem.signal();
    }
    waiter.join();

    return report(result);
}

bool testPingPong()
{
    std::cout << "Test 5 (no lost wake-ups in a 20000-round hand-off): ";

    // Each side waits for the other before continuing, so a single lost
    // signal stalls the exchange; waitFor() keeps a stall from hanging the
    // test, and the round counts show it.
    constexpr int ROUNDS = 20000;
    Semaphore ping;
    Semaphore pong;
    std::atomic<int> pongRounds(0);
    std::atomic<bool> stalled(false);

    std::thread responder([&]() {
        for (int i = 0; i < ROUNDS && !stalled.load(); i++)
        {
            auto start = Clock::now();
            ping.waitFor(5000);
            if (Clock::now() - start >= 4s)
            {
                stalled.store(true);
                break;
            }
            pongRounds.fetch_add(1);
            pong.signal();
        }
    });

    for (int i = 0; i < ROUNDS && !stalled.load(); i++)
    {
        ping.signal();
        auto start = Clock::now();
        pong.waitFor(5000);
        if (Clock::now() - start >= 4s)
        {
            stalled.store(true);
        }
    }
    ping.signal(); // release the responder if it's stuck
    responder.join();

    bool result = CHECK(!stalled.load());
    result &= CHECK(pongRounds.load() == ROUNDS);

    return report(result);
}

bool testConcurrentSignalers()
{
    std::cout << "Test 6 (signals from several threads wake a waiter each time): ";

    // Several threads take turns signaling (one at a time, so at most one
    // signal is pending); the waiter must see every one.
    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 500;
    Semaphore sem;
    Semaphore done;
    std::atomic<int> received(0);

    std::thread waiter([&]() {
        for (int i = 0; i < THREADS * PER_THREAD; i++)
        {
            auto start = Clock::now();
            sem.waitFor(5000);
            if (Clock::now() - start >= 4s)
            {
                break;
            }
            received.fetch_add(1);
            done.signal();
        }
    });

    std::mutex turn;
    std::vector<std::thread> signalers;
    for (int t = 0; t < THREADS; t++)
    {
        signalers.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; i++)
            {
                std::lock_guard<std::mutex> lk(turn);
                sem.signal();
                done.waitFor(5000);
            }
        });
    }
    for (auto& t : signalers)
    {
        t.join();
    }
    waiter.join();

    bool result = CHECK(received.load() == THREADS * PER_THREAD);

    return report(result);
}

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testSignalBeforeWait();
    result &= testWaitForTimesOut();
    result &= testWaitForWokenEarly();
    result &= testWaitBlocksUntilSignaled();
    result &= testPingPong();
    result &= testConcurrentSignalers();
    result &= testLongWaitForTimesOut();

    return result ? 0 : -1;
}
