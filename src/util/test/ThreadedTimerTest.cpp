#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

#include "../ThreadedTimer.h"

using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    // Single-shot time ordering test
    std::cout << "Test 1 (Single-shot time ordering): ";
    std::atomic<int> count = 0;
    bool result = true;
    ThreadedTimer timer1(250, [&](ThreadedTimer&) { if (count == 0) count++; }, false);
    ThreadedTimer timer2(500, [&](ThreadedTimer&) { if (count == 1) count++; }, false);
    ThreadedTimer timer3(750, [&](ThreadedTimer&) { if (count == 2) count++; }, false);

    timer1.start();
    timer2.start();
    timer3.start();

    std::this_thread::sleep_for(1s);
    bool testResult = (count == 3);
    std::cout << "(count = " << count << ") ";
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // Repeated trigger test
    std::cout << "Test 2 (Repeated triggering): ";
    count = 0;
    ThreadedTimer timer4(250, [&](ThreadedTimer&) { count++; }, true);
    timer4.start();

    std::this_thread::sleep_for(1s);
    testResult = (count >= 3);
    std::cout << "(count = " << count << ") ";
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // Timer stop test
    std::cout << "Test 3 (Stop repeated timer): ";
    timer4.stop();

    std::this_thread::sleep_for(1s);
    testResult = (count >= 3);
    std::cout << "(count = " << count << ") ";
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // Test 3 only checks the count didn't go backwards; check it really
    // stopped.
    std::cout << "Test 3b (Stopped timer no longer fires): ";
    int countAfterStop = count;
    std::this_thread::sleep_for(600ms);
    testResult = (count == countAfterStop) && !timer4.isRunning();
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // stop() must not return while the timer's callback is still running,
    // otherwise the caller can destroy what the callback is using.
    std::cout << "Test 4 (stop() waits for a running callback): ";
    {
        std::atomic<bool> inCallback(false);
        std::atomic<bool> callbackDone(false);
        ThreadedTimer slow(10, [&](ThreadedTimer&) {
            inCallback = true;
            std::this_thread::sleep_for(200ms);
            callbackDone = true;
        }, false);
        slow.start();
        while (!inCallback)
        {
            std::this_thread::sleep_for(1ms);
        }
        slow.stop();
        testResult = callbackDone.load();
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // Destroying a timer while its callback runs: no callback may still be
    // running, or start, after the destructor returns.
    std::cout << "Test 5 (Destroy timer while its callback runs): ";
    {
        struct Shared
        {
            std::atomic<bool> destroyed{false};
            std::atomic<int> violations{0};
            std::atomic<int> calls{0};
        };
        auto shared = std::make_shared<Shared>();
        for (int i = 0; i < 50; i++)
        {
            shared->destroyed = false;
            auto timer = std::make_unique<ThreadedTimer>(1, [shared](ThreadedTimer&) {
                if (shared->destroyed) shared->violations++;
                shared->calls++;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                if (shared->destroyed) shared->violations++;
            }, true);
            int callsBefore = shared->calls;
            timer->start();
            // Destroy right after a callback has started (that's the window
            // being tested), not after a fixed delay that a loaded machine
            // may not honour.
            auto deadline = std::chrono::steady_clock::now() + 2s;
            while (shared->calls == callsBefore && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::microseconds((i % 7) * 100));
            timer.reset();
            shared->destroyed = true;
        }
        std::this_thread::sleep_for(50ms);
        testResult = shared->violations == 0 && shared->calls >= 50;
        std::cout << "(calls = " << shared->calls << ", violations = " << shared->violations << ") ";
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // A repeating timer stopped while it's being rescheduled must stay
    // stopped (previously it could be put back on the queue after stop()).
    std::cout << "Test 6 (Stopped repeating timer stays stopped): ";
    {
        std::atomic<int> fires(0);
        ThreadedTimer fast(1, [&](ThreadedTimer&) { fires++; }, true);
        int resurrected = 0;
        for (int i = 0; i < 200; i++)
        {
            int firesBefore = fires;
            fast.start();
            // Stop just after a firing, while the server is rescheduling it --
            // the window where a stopped timer used to get re-queued.
            auto deadline = std::chrono::steady_clock::now() + 2s;
            while (fires == firesBefore && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::microseconds((i % 5) * 50));
            fast.stop();
            int afterStop = fires;
            std::this_thread::sleep_for(5ms);
            if (fires != afterStop || fast.isRunning()) resurrected++;
        }
        testResult = resurrected == 0 && fires >= 200;
        std::cout << "(fires = " << fires << ", resurrected = " << resurrected << ") ";
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // A callback may stop or restart its own timer without deadlocking.
    std::cout << "Test 7 (stop()/restart() from inside the callback): ";
    {
        std::atomic<int> repeats(0);
        ThreadedTimer selfStopping(5, [&](ThreadedTimer& t) {
            if (++repeats == 3) t.stop();
        }, true);
        selfStopping.start();

        std::atomic<int> restarts(0);
        ThreadedTimer selfRestarting(5, [&](ThreadedTimer& t) {
            if (++restarts < 5) t.restart();
        }, false);
        selfRestarting.start();

        std::this_thread::sleep_for(300ms);
        testResult = repeats == 3 && restarts == 5 && !selfStopping.isRunning();
        std::cout << "(repeats = " << repeats << ", restarts = " << restarts << ") ";
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // Many threads creating, starting and destroying short timers at once.
    std::cout << "Test 8 (Concurrent create/start/destroy): ";
    {
        std::atomic<int> fires(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; t++)
        {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < 100; i++)
                {
                    ThreadedTimer timer(1 + (i + t) % 3, [&](ThreadedTimer&) { fires++; }, (i % 2) == 0);
                    timer.start();
                    std::this_thread::sleep_for(std::chrono::microseconds(((i * 7 + t) % 5) * 500));
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        testResult = fires > 0;
        std::cout << "(fires = " << fires << ") ";
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // A timer registered without any callback must be fired safely too
    // (fire_() has to tolerate an empty std::function).
    std::cout << "Test 9 (Timer with no callback fires and stops cleanly): ";
    {
        ThreadedTimer noCallback;
        noCallback.setTimeout(10);
        noCallback.setRepeat(true);
        noCallback.start();
        std::this_thread::sleep_for(100ms);
        testResult = noCallback.isRunning();
        noCallback.setCallback(nullptr); // explicit empty callback; still no crash
        std::this_thread::sleep_for(50ms);
        noCallback.stop();
        testResult &= !noCallback.isRunning();
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    // A one-shot timer that stops itself from inside its callback: the
    // callback runs on the timer server's own thread, so the unregister
    // path has to skip the firing-done wait (it would be waiting on its
    // own firing) and just drop the entry.
    std::cout << "Test 10 (One-shot timer stops itself from its callback): ";
    {
        std::atomic<int> calls(0);
        ThreadedTimer selfStop(10, [&](ThreadedTimer& t) {
            calls++;
            t.stop();
        }, false);
        selfStop.start();

        auto deadline = std::chrono::steady_clock::now() + 5s;
        while ((calls.load() == 0 || selfStop.isRunning()) &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        testResult = calls.load() == 1 && !selfStop.isRunning();

        // With its entry already erased, the timer cannot fire again; the
        // destructor's own stop() (from the main thread) must also stay a
        // no-op.
        int afterStop = calls.load();
        std::this_thread::sleep_for(200ms);
        testResult &= calls.load() == afterStop && !selfStop.isRunning();
        std::cout << "(calls = " << calls.load() << ") ";
    }
    std::cout << (testResult ? "PASS" : "FAIL") << "\n";
    result &= testResult;

    return result ? 0 : -1;
}
