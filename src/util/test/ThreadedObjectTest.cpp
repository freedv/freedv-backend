// Tests for ThreadedObject: tasks run in order on the object's thread, and
// once a derived class's destructor has waited for its tasks
// (waitForAllTasksComplete_(), as TcpConnectionHandler/SocketIoClient do), no
// task may still be running or start afterwards -- otherwise it runs against a
// destroyed object.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../ThreadedObject.h"
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

// Outlives each TestObject so tasks can record what they saw.
struct Tracker
{
    std::atomic<bool> destroyed{false};
    std::atomic<int> ran{0};
    std::atomic<int> violations{0};
};

class TestObject : public ThreadedObject
{
public:
    explicit TestObject(std::shared_ptr<Tracker> tracker)
        : ThreadedObject("TestObject")
        , tracker_(std::move(tracker))
    {
    }

    virtual ~TestObject()
    {
        // As TcpConnectionHandler and SocketIoClient do: wait for our tasks
        // before our members go away.
        waitForAllTasksComplete_();
        tracker_->destroyed = true;
    }

    // Queues a task that checks it isn't running on a destroyed object, both
    // when it starts and when it finishes.
    void enqueueChecked(std::chrono::microseconds work)
    {
        auto tracker = tracker_;
        enqueue_([tracker, work]() {
            if (tracker->destroyed) tracker->violations++;
            std::this_thread::sleep_for(work);
            tracker->ran++;
            if (tracker->destroyed) tracker->violations++;
        });
    }

    void enqueue(std::function<void()> fn) { enqueue_(std::move(fn)); }

    void enqueueTimeout(std::function<void()> fn, int timeoutMilliseconds)
    {
        enqueue_(std::move(fn), timeoutMilliseconds);
    }

    void waitAll() { waitForAllTasksComplete_(); }

private:
    std::shared_ptr<Tracker> tracker_;
};

class ChildObject : public ThreadedObject
{
public:
    explicit ChildObject(ThreadedObject* parent)
        : ThreadedObject("ChildObject", parent)
    {
    }

    void enqueueTask(std::function<void()> fn) { enqueue_(std::move(fn)); }
};

bool testOrdering()
{
    std::cout << "Test 1 (tasks run in order on one thread): ";

    auto tracker = std::make_shared<Tracker>();
    std::vector<int> order;
    std::thread::id firstThread;
    std::atomic<bool> sameThread(true);
    {
        TestObject obj(tracker);
        for (int i = 0; i < 100; i++)
        {
            obj.enqueue([&, i]() {
                if (i == 0) firstThread = std::this_thread::get_id();
                else if (std::this_thread::get_id() != firstThread) sameThread = false;
                order.push_back(i);
            });
        }
        obj.waitAll();
    }

    bool result = CHECK(order.size() == 100);
    for (int i = 0; i < (int)order.size(); i++)
    {
        if (order[i] != i)
        {
            result &= CHECK(order[i] == i);
            break;
        }
    }
    result &= CHECK(sameThread.load());
    result &= CHECK(firstThread != std::this_thread::get_id());
    return report(result);
}

bool testLongTaskBlocksDestruction()
{
    std::cout << "Test 2 (destructor waits for a task longer than 250 ms): ";

    // The Linux implementation used to give up waiting after ~250 ms and let
    // the destructor continue while the task was still running.
    auto tracker = std::make_shared<Tracker>();
    {
        TestObject obj(tracker);
        obj.enqueueChecked(600ms);
        std::this_thread::sleep_for(20ms); // let it start
    }

    bool result = CHECK(tracker->ran == 1);
    result &= CHECK(tracker->violations == 0);
    return report(result);
}

bool testDestroyWithTasksInFlight()
{
    std::cout << "Test 3 (no task runs after destruction, many cycles): ";

    // Repeatedly destroy objects with tasks queued and running, from the main
    // thread and a second thread enqueueing concurrently. Any task that
    // starts or is still running after the destructor's wait returned is a
    // violation.
    auto tracker = std::make_shared<Tracker>();
    int cycles = 0;
    for (int i = 0; i < 300; i++)
    {
        tracker->destroyed = false;
        auto obj = std::make_unique<TestObject>(tracker);
        std::atomic<bool> stop(false);
        std::thread producer([&]() {
            int n = 0;
            while (!stop && n < 20)
            {
                obj->enqueueChecked(std::chrono::microseconds(50 + (n % 4) * 50));
                n++;
            }
        });
        for (int n = 0; n < 5; n++)
        {
            obj->enqueueChecked(std::chrono::microseconds((i + n) % 3 * 100));
        }
        std::this_thread::sleep_for(std::chrono::microseconds((i % 5) * 200));
        stop = true;
        producer.join();
        obj.reset();
        cycles++;
    }
    std::this_thread::sleep_for(20ms);

    std::cout << "(" << cycles << " cycles, " << tracker->ran << " tasks, "
              << tracker->violations << " violations) ";
    bool result = CHECK(tracker->violations == 0);
    result &= CHECK(tracker->ran > 0);
    return report(result);
}

#if !defined(__APPLE__)
bool testWaitFromOwnTask()
{
    std::cout << "Test 4 (waiting from the object's own task returns): ";

    // A task can't wait for its own queue (it would wait for itself); the
    // call must return instead of hanging. (Linux implementation only; the
    // GCD-based macOS implementation counts the running task as queued.)
    auto tracker = std::make_shared<Tracker>();
    std::atomic<bool> returned(false);
    {
        TestObject obj(tracker);
        obj.enqueue([&]() {
            obj.waitAll();
            returned = true;
        });
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!returned && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
    }
    return report(CHECK(returned.load()));
}
#endif // !defined(__APPLE__)

bool testChildEnqueuesOnParent()
{
    std::cout << "Test 5 (child objects enqueue on their parent's thread): ";

    // A ThreadedObject created with a parent never spawns its own thread; its
    // tasks are handed to the parent instead, and its destructor has nothing
    // to join.
    auto tracker = std::make_shared<Tracker>();
    std::atomic<int> ran{0};
    std::thread::id taskThread;
    {
        TestObject parent(tracker);
        ChildObject child(&parent);
        child.enqueueTask([&]() {
            ran = 1;
            taskThread = std::this_thread::get_id();
        });
        parent.waitAll();
    }

    bool result = CHECK(ran == 1);
    result &= CHECK(taskThread != std::this_thread::get_id());
    result &= CHECK(tracker->violations == 0);
    return report(result);
}

bool testNullTaskIsSkipped()
{
    std::cout << "Test 6 (an empty task is dequeued and skipped, not run): ";

    auto tracker = std::make_shared<Tracker>();
    std::atomic<bool> before{false};
    std::atomic<bool> after{false};
    {
        TestObject obj(tracker);
        obj.enqueue([&]() { before = true; });
        obj.enqueue(std::function<void()>());
        obj.enqueue([&]() { after = true; });
        obj.waitAll();
    }

    bool result = CHECK(before.load());
    result &= CHECK(after.load());
    result &= CHECK(tracker->violations == 0);
    return report(result);
}

bool testEnqueueSuppressedWhileWaiting()
{
    std::cout << "Test 7 (enqueue is suppressed while another thread waits for tasks): ";

    // While waitForAllTasksComplete_() is spinning, new enqueues are dropped:
    // the object is on its way out, so nothing may be added behind the wait.
    auto tracker = std::make_shared<Tracker>();
    Semaphore hold;
    std::atomic<bool> started{false};
    std::atomic<int> probesRun{0};
    {
        TestObject obj(tracker);
        obj.enqueue([&]() {
            started = true;
            hold.wait();
        });
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!started && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }

        std::thread waiter([&]() { obj.waitAll(); });
        std::this_thread::sleep_for(30ms); // the waiter has set the suppression flag by now

        for (int i = 0; i < 20; i++)
        {
            obj.enqueue([&]() { probesRun++; }); // dropped: suppression is on
        }

        hold.signal();
        waiter.join();

        obj.enqueue([&]() { probesRun++; }); // suppression is cleared once the wait returned
        obj.waitAll();
    }

    bool result = CHECK(started.load());
    result &= CHECK(probesRun == 1);
    result &= CHECK(tracker->violations == 0);
    return report(result);
}

bool testLongWaitWarnsButFinishes()
{
    std::cout << "Test 8 (waiting over 2 s for a task warns but still waits): ";

    // The waiter logs a warning after 2 s but must keep waiting, not give up.
    auto tracker = std::make_shared<Tracker>();
    Semaphore hold;
    std::atomic<bool> finished{false};
    {
        TestObject obj(tracker);
        obj.enqueue([&]() {
            hold.wait();
            finished = true;
        });
        std::this_thread::sleep_for(50ms); // let the task start

        std::thread waiter([&]() { obj.waitAll(); });
        for (int i = 0; i < 12; i++)
        {
            std::this_thread::sleep_for(200ms); // keep it blocked for > 2 s total
        }
        hold.signal();
        waiter.join();
    }

    bool result = CHECK(finished.load());
    result &= CHECK(tracker->violations == 0);
    return report(result);
}

bool testTimeoutEnqueueUnderContention()
{
    std::cout << "Test 9 (timeout enqueues under heavy contention): ";

    // Four threads hammer the same queue with timed enqueues (two with a
    // generous timeout, two with 1 ms so they give up when they collide).
    // Whether a given attempt retries, succeeds or gives up is timing-dependent
    // and deliberately not asserted -- what must hold is that the object
    // survives the storm, a good number of tasks ran, and normal enqueueing
    // works again afterwards.
    auto tracker = std::make_shared<Tracker>();
    std::atomic<int> ran{0};
    {
        TestObject obj(tracker);
        std::vector<std::thread> hammers;
        for (int t = 0; t < 4; t++)
        {
            int timeout = (t % 2 == 0) ? 500 : 1;
            hammers.push_back(std::thread([&obj, &ran, timeout]() {
                for (int i = 0; i < 2500; i++)
                {
                    obj.enqueueTimeout([&]() { ran.fetch_add(1); }, timeout);
                }
            }));
        }
        for (auto& hammer : hammers)
        {
            hammer.join();
        }
        obj.waitAll();
    }

    bool result = CHECK(ran > 100);

    // The object must still be usable afterwards.
    auto tracker2 = std::make_shared<Tracker>();
    {
        TestObject fresh(tracker2);
        std::atomic<bool> worked{false};
        fresh.enqueue([&]() { worked = true; });
        fresh.waitAll();
        result &= CHECK(worked.load());
    }
    return report(result);
}

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testOrdering();
    result &= testLongTaskBlocksDestruction();
    result &= testDestroyWithTasksInFlight();
#if !defined(__APPLE__)
    result &= testWaitFromOwnTask();
#endif // !defined(__APPLE__)

    result &= testChildEnqueuesOnParent();
    result &= testNullTaskIsSkipped();
    result &= testEnqueueSuppressedWhileWaiting();
    result &= testLongWaitWarnsButFinishes();
    result &= testTimeoutEnqueueUnderContention();

    return result ? 0 : -1;
}
