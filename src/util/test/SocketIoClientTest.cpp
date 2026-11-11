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

int main()
{
    bool result = true;

    result &= test1OpenFrameSizedToThePayload();
    result &= test2OpenFrameStopsAtTheLength();
    result &= test3EventFrameStopsAtTheLength();
    result &= test4EmptyAndTypeOnlyFrames();
    result &= test5TruncatedJsonIsIgnored();
    result &= test6RandomFrames();

    return result ? 0 : -1;
}
