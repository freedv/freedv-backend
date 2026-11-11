// Regression tests for the engine.io / socket.io frame parsers
// (handleEngineIoMessage_ / handleSocketIoMessage_).
//
// Both handlers used to treat the frame as a NUL terminated C string: strlen()
// ran past the end of a buffer that is only `length` bytes long (ASan
// heap-buffer-overflow, found by fuzzing the websocket receive path with random
// bytes), and an empty or type-only frame was read at ptr[0] regardless. Every
// test here hands the handler a heap buffer sized exactly to the frame, so a
// read past the end is a sanitizer failure rather than a silent pass.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include "../SocketIoClient.h"
#include "yyjson.h"

// SocketIoClient.h names SocketIoClientTest as a friend, so the class that
// reaches the private frame handlers has to be declared at global scope.
class SocketIoClientTest
{
public:
    static void feedEngine(SocketIoClient& client, const char* data, int length)
    {
        client.handleEngineIoMessage_(const_cast<char*>(data), length);
    }

    static void feedSocket(SocketIoClient& client, const char* data, int length)
    {
        client.handleSocketIoMessage_(const_cast<char*>(data), length);
    }

    static bool pingTimerRunning(SocketIoClient& client)
    {
        return client.pingTimer_.isRunning();
    }

    // authObj_ is only touched on the client's task thread, so the snapshot is
    // queued behind the publication it is checking; the test itself only ever
    // reads the mutex-protected copy.
    static void snapshotAuth(SocketIoClient& client, std::mutex& mx, std::string& slot)
    {
        client.enqueue_([&client, &mx, &slot]() {
            std::lock_guard<std::mutex> lk(mx);
            slot = client.authObj_;
        });
    }

    // Same for eventFnMap_: registered and looked up only on the task thread.
    static void snapshotEventCount(SocketIoClient& client, std::mutex& mx, int& slot)
    {
        client.enqueue_([&client, &mx, &slot]() {
            std::lock_guard<std::mutex> lk(mx);
            slot = (int) client.eventFnMap_.size();
        });
    }
};

namespace
{
using Driver = SocketIoClientTest;

// A heap buffer holding exactly `bytes`: reads past the end are out of bounds.
std::vector<char> frameOf(const std::string& bytes)
{
    return std::vector<char>(bytes.begin(), bytes.end());
}

bool test1OpenFrameSizedToThePayload()
{
    std::cout << "Test 1 (engine.io open, no NUL terminator): ";

    SocketIoClient client;
    auto frame = frameOf("0{\"pingInterval\":25000,\"pingTimeout\":20000}");

    Driver::feedEngine(client, frame.data(), (int)frame.size());

    // Parsed: the ping watchdog is armed. strlen() would have read past the
    // buffer here, and there is no NUL for it to stop at.
    bool result = Driver::pingTimerRunning(client);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool test2OpenFrameStopsAtTheLength()
{
    SocketIoClient client;
    // The frame is `payload` bytes long; the bytes after it are not part of it
    // and must never be read.
    const std::string payload = "0{\"pingInterval\":25000,\"pingTimeout\":20000}";
    std::string bytes = payload;
    bytes.push_back('\0');
    bytes += "not json";
    auto frame = frameOf(bytes);

    Driver::feedEngine(client, frame.data(), (int)payload.size());

    bool result = Driver::pingTimerRunning(client);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool test3EventFrameStopsAtTheLength()
{
    std::cout << "Test 3 (socket.io event parsed by length, not strlen): ";

    SocketIoClient client;
    std::string gotArg;
    // The callback runs on the client's task thread, so the counter and the
    // string it publishes need release/acquire ordering against this thread.
    std::atomic<int> events{0};
    client.on("onTest", [&](yyjson_val* params) {
        if (yyjson_is_arr(params) && yyjson_arr_size(params) == 1)
        {
            auto first = yyjson_arr_get(params, 0);
            if (first != nullptr && yyjson_is_str(first))
            {
                gotArg = yyjson_get_str(first);
            }
        }
        events.fetch_add(1, std::memory_order_release);
    });

    // '4' engine.io message wrapping a '2' socket.io event, followed by bytes
    // that must never be read: a strlen()-based parse would run past the end of
    // the buffer looking for the NUL.
    const std::string payload = "42[\"onTest\",[\"hello\"]]";
    std::string bytes = payload;
    bytes += "not json";
    auto frame = frameOf(bytes);
    Driver::feedEngine(client, frame.data(), (int)payload.size());

    // The handler queues the event on the connection's task queue: give it a
    // moment to run.
    for (int waited = 0; waited < 2000 && events.load(std::memory_order_acquire) == 0; waited += 50)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool result = (events.load(std::memory_order_acquire) == 1) && (gotArg == "hello");
    if (!result)
    {
        std::cout << "[" << events.load(std::memory_order_acquire) << " events, arg '" << gotArg << "']";
    }
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool test4EmptyAndTypeOnlyFrames()
{
    std::cout << "Test 4 (empty, type-only and null frames): ";

    SocketIoClient client;

    // An empty frame must not read ptr[0]; a frame holding only the engine.io
    // type byte must not hand a zero-length payload to the socket.io parser.
    auto typeOnly = frameOf("4");
    Driver::feedEngine(client, typeOnly.data(), 1);

    Driver::feedEngine(client, nullptr, 0);
    Driver::feedSocket(client, nullptr, 0);

    std::cout << "PASS\n";
    return true;
}

bool test5TruncatedJsonIsIgnored()
{
    std::cout << "Test 5 (truncated JSON payload): ";

    SocketIoClient client;
    std::atomic<int> events{0}; // the callback runs on the client's task thread
    client.on("onTest", [&](yyjson_val*) { events++; });

    // Cut mid-value: nothing may be dispatched, and nothing may crash.
    auto frame = frameOf("42[\"onTest\",[\"hel");
    Driver::feedEngine(client, frame.data(), (int)frame.size());

    auto openBroken = frameOf("0{\"pingInterval\":");
    Driver::feedEngine(client, openBroken.data(), (int)openBroken.size());

    for (int waited = 0; waited < 500; waited += 50)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool result = (events.load(std::memory_order_acquire) == 0);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool test6RandomFrames()
{
    std::cout << "Test 6 (random frames of random length): ";

    // The fuzz sweep that found the strlen() overrun, with a fixed seed so a
    // regression still fails the suite deterministically.
    std::mt19937 rng(20261110);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<int> lenDist(0, 64);

    for (int i = 0; i < 500; i++)
    {
        SocketIoClient client;
        int len = lenDist(rng);
        std::vector<char> frame(len);
        for (int n = 0; n < len; n++)
        {
            frame[n] = (char)byteDist(rng);
        }
        Driver::feedEngine(client, frame.data(), len);
        Driver::feedSocket(client, frame.data(), len);
    }

    std::cout << "PASS\n";
    return true;
}
} // namespace

bool test7HugePingTimingsDontOverflow()
{
    std::cout << "Test 7 (huge ping timings from the server): ";

    // Each fits an int; their sum doesn't. Added as ints, that's a signed
    // overflow (UBSan); the timer must still be armed with a sane timeout.
    bool result = true;
    for (const char* json : {
             "0{\"pingInterval\":2000000000,\"pingTimeout\":2000000000}",
             "0{\"pingInterval\":9223372036854775807,\"pingTimeout\":1}",
             "0{\"pingInterval\":-2000000000,\"pingTimeout\":-2000000000}",
         })
    {
        SocketIoClient client;
        auto frame = frameOf(json);
        Driver::feedEngine(client, frame.data(), (int)frame.size());
        result &= Driver::pingTimerRunning(client);
    }
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool test8AuthDictionaryPublication()
{
    std::cout << "Test 8 (auth dictionary: unusable doc, task-thread publication): ";

    // The snapshot storage is declared ahead of the clients so it is still
    // alive while their destructors drain the task queue.
    std::mutex mx;
    std::string seenRootless, seen;

    // A document with no root: yyjson_mut_write() returns NULL for it, and
    // assigning that straight to std::string threw std::logic_error out of the
    // library, which terminates the process.
    SocketIoClient rootless;
    rootless.setAuthDictionary(yyjson_mut_doc_new(nullptr));

    // The write itself is published through the client's task queue, because
    // the websocketpp open handler reads the member on that thread.
    SocketIoClient client;
    auto doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, root, "callsign", "K6AQ");
    yyjson_mut_doc_set_root(doc, root);
    client.setAuthDictionary(doc); // takes ownership of doc

    Driver::snapshotAuth(rootless, mx, seenRootless);
    Driver::snapshotAuth(client, mx, seen);

    // The lock is not held across the sleep: doing that starves the task
    // thread that wants to take the snapshot.
    bool ready = false;
    for (int waited = 0; waited < 2000 && !ready; waited += 10)
    {
        {
            std::lock_guard<std::mutex> lk(mx);
            ready = !seenRootless.empty() && !seen.empty();
        }
        if (!ready)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool result;
    {
        std::lock_guard<std::mutex> lk(mx);
        result = (seenRootless == "{}") && (seen == "{\"callsign\":\"K6AQ\"}");
        if (!result)
        {
            std::cout << "[" << seenRootless << " / " << seen << "]";
        }
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool test9UnknownEventNamesDoNotGrowTheMap()
{
    std::cout << "Test 9 (unknown event names don't register themselves): ";

    // Declared ahead of the client so the storage outlives the queue drain.
    std::mutex mx;
    int counted = -1;
    std::atomic<int> fired{0};

    SocketIoClient client;
    client.on("known", [&](yyjson_val*) { fired.fetch_add(1, std::memory_order_release); });

    // fireEvent() looked names up with operator[], which inserts a null entry
    // for each one that isn't registered: 200 frames naming fresh events grew
    // the map by 200, from data the far end chose.
    for (int i = 0; i < 200; i++)
    {
        auto frame = frameOf("42[\"unknown" + std::to_string(i) + "\",[]]");
        Driver::feedEngine(client, frame.data(), (int)frame.size());
    }

    // The registered handler still has to fire.
    auto known = frameOf("42[\"known\",[]]");
    Driver::feedEngine(client, known.data(), (int)known.size());

    // Queued behind every dispatch above, so the count is the final one.
    Driver::snapshotEventCount(client, mx, counted);

    bool ready = false;
    for (int waited = 0; waited < 2000 && !ready; waited += 10)
    {
        {
            std::lock_guard<std::mutex> lk(mx);
            ready = (counted >= 0);
        }
        if (!ready)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool result;
    {
        std::lock_guard<std::mutex> lk(mx);
        result = (counted == 1) && (fired.load(std::memory_order_acquire) == 1);
        if (!result)
        {
            std::cout << "[" << counted << " entries, " << fired.load(std::memory_order_acquire) << " fired]";
        }
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

int main()
{
    bool result = true;

    result &= test1OpenFrameSizedToThePayload();
    result &= test2OpenFrameStopsAtTheLength();
    result &= test3EventFrameStopsAtTheLength();
    result &= test4EmptyAndTypeOnlyFrames();
    result &= test5TruncatedJsonIsIgnored();
    result &= test6RandomFrames();
    result &= test7HugePingTimingsDontOverflow();
    result &= test8AuthDictionaryPublication();
    result &= test9UnknownEventNamesDoNotGrowTheMap();

    return result ? 0 : -1;
}
