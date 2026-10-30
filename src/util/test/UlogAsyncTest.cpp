// Tests for the async ulog front end (ulog_async.{h,cpp}).
//
// The consumer thread starts automatically (static constructor), so these
// tests never call ulog_async_start()/ulog_async_stop() except where they
// deliberately exercise the stop/restart path. Every log_*() call -- from the
// main thread or a worker -- is routed through the ring, so the tests flush
// before inspecting the captured output.
//
// stderr (ulog's default sink) is redirected to a temp file around each
// exercise so the rendered output can be inspected.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define DUP _dup
#define DUP2 _dup2
#define FILENO _fileno
#define CLOSE _close
#define READ _read
#define LSEEK _lseek
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define FILENO fileno
#define CLOSE close
#define READ read
#define LSEEK lseek
#endif

#include "../logging/ulog.h"
#include "../logging/ulog_async.h"

namespace
{

std::string captureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);

#if defined(_WIN32)
    char path[L_tmpnam];
    tmpnam(path);
    int fd = _open(path, _O_CREAT | _O_RDWR | _O_BINARY, 0600);
#else
    char path[] = "/tmp/ulog_async_test_XXXXXX";
    int fd = mkstemp(path);
#endif
    if (fd < 0)
    {
        std::cerr << "captureStderr: could not create temp file\n";
        return {};
    }

    int saved = DUP(FILENO(stderr));
    DUP2(fd, FILENO(stderr));

    fn();

    std::fflush(stderr);
    DUP2(saved, FILENO(stderr));
    CLOSE(saved);

    LSEEK(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (;;)
    {
        auto n = READ(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    CLOSE(fd);
    std::remove(path);
    return out;
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

size_t countOccurrences(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
    {
        ++n;
    }
    return n;
}

// Lock hook shared by the consumer-thread tests below. It is installed only
// for the duration of those tests (mode != 0) and must never recurse: the
// hook body itself never logs.
std::atomic<int> g_hookMode{ 0 };   // 0 = inactive, 1 = raise level on first consumer call
std::atomic<int> g_hookFired{ 0 };

void lockHook(bool lock, void* /*arg*/)
{
    if (!lock || g_hookMode.load() == 0)
    {
        return;
    }
    // While the async front end is up, producers only enqueue -- they never
    // reach lock(). A hook invocation with ulog_async_is_active() false is
    // therefore running on the consumer thread.
    if (ulog_async_is_active())
    {
        return;
    }
    if (g_hookMode.load() == 1 && g_hookFired.exchange(1) == 0)
    {
        ulog_set_level(LOG_ERROR);
    }
}
// -------------------------------------------------------------------------

bool testDeferredFormatting()
{
    std::cout << "Test 1 (deferred formatting from a worker thread): ";

    std::string out = captureStderr([]() {
        std::thread rt([]() {
            log_info("int=%d str=%s flt=%.2f", 42, "hello", 3.14159);
            log_warn("widths l=%ld ll=%lld u=%u z=%zu", 7L, 8LL, 9u,
                     static_cast<size_t>(10));
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "int=42 str=hello flt=3.14");
    result &= contains(out, "widths l=7 ll=8 u=9 z=10");
    // File/line of the original call site must survive.
    result &= contains(out, "UlogAsyncTest.cpp:");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testMainThreadRoutedThroughAsync()
{
    std::cout << "Test 2 (main-thread logs go through the async path too): ";

    std::string out = captureStderr([]() {
        log_info("from the main thread %d", 123);
        // Not written inline any more: it only reaches stderr once drained.
        ulog_async_flush();
    });

    bool result = contains(out, "from the main thread 123");
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testNullAndLongString()
{
    std::cout << "Test 3 (NULL and over-long %s are handled): ";

    std::string big(4096, 'A');

    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            const char* np = nullptr;
            log_info("null=[%s]", np);
            log_info("big=[%s]", big.c_str());
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "null=[(null)]");
    // The long string must be truncated, not crash, and be flagged.
    result &= contains(out, "big=[AAAA");
    result &= contains(out, "truncated");
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 400) << "...\n---\n";
    return result;
}

bool testSynchronousFallbackWhenStopped()
{
    std::cout << "Test 4 (stop -> synchronous fallback, start -> async resumes): ";

    // While the consumer is stopped, ulog_async_is_active() is false and
    // ulog_log() formats inline: the line is present with no flush at all.
    std::string stopped = captureStderr([]() {
        ulog_async_stop();
        std::thread w([]() { log_info("sync fallback %d", 7); });
        w.join();
        // deliberately no ulog_async_flush()
    });

    bool result = contains(stopped, "sync fallback 7");

    // Bring the consumer back and confirm async delivery works again.
    std::string resumed = captureStderr([]() {
        ulog_async_start();
        std::thread w([]() { log_info("async resumed %d", 9); });
        w.join();
        ulog_async_flush();
    });

    result &= contains(resumed, "async resumed 9");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result)
    {
        std::cout << "--- stopped ---\n" << stopped
                  << "--- resumed ---\n" << resumed << "---\n";
    }
    return result;
}

// For each producer thread, walks the "prod T seq I" lines in the order they
// appear in the output and checks the I values are strictly increasing -- i.e.
// the consumer never reordered or duplicated a given thread's records.
bool survivorsInOrder(const std::string& out, int threads, int perThread,
                      size_t& totalSurvivors)
{
    totalSurvivors = 0;
    for (int t = 0; t < threads; ++t)
    {
        std::string prefix = "prod " + std::to_string(t) + " seq ";
        int lastSeq = -1;
        for (size_t at = out.find(prefix); at != std::string::npos;
             at = out.find(prefix, at + prefix.size()))
        {
            int seq = std::atoi(out.c_str() + at + prefix.size());
            if (seq <= lastSeq) return false; // reordered or duplicated
            lastSeq = seq;
            ++totalSurvivors;
            if (seq >= perThread) return false; // garbled
        }
    }
    return true;
}

bool testMultiProducerInvariants()
{
    std::cout << "Test 5 (many producers, ring overflows: accounting + ordering hold): ";

    constexpr int kThreads = 6;
    constexpr int kPerThread = 400;
    constexpr int kTotal = kThreads * kPerThread;

    unsigned long before = ulog_async_dropped_count();

    std::string out = captureStderr([&]() {
        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t)
        {
            producers.emplace_back([t]() {
                for (int i = 0; i < kPerThread; ++i)
                {
                    log_info("prod %d seq %d", t, i);
                }
            });
        }
        for (auto& p : producers) p.join();

        ulog_async_flush();
    });

    unsigned long dropped = ulog_async_dropped_count() - before;
    size_t producerLines = countOccurrences(out, "prod ");
    size_t summaryLines = countOccurrences(out, "record(s) dropped");

    size_t survivors = 0;
    bool ordered = survivorsInOrder(out, kThreads, kPerThread, survivors);

    // Core invariants for a lossy-but-honest logger:
    //  * every record is either emitted or counted as dropped, none vanish;
    //  * whatever survives keeps per-thread order;
    //  * the visible producer lines match the survivors we can account for;
    //  * if anything was dropped, the consumer reported it at least once.
    bool result = ordered;
    result &= (producerLines + dropped == static_cast<unsigned long>(kTotal));
    result &= (survivors == producerLines);
    result &= (dropped == 0 || summaryLines >= 1);

    std::cout << (result ? "PASS" : "FAIL")
              << " (emitted=" << producerLines << " dropped=" << dropped
              << " summaries=" << summaryLines << " total=" << kTotal
              << " ordered=" << ordered << ")\n";
    return result;
}

// Logs fmt both through ulog (deferred formatting on the consumer thread) and
// through snprintf() directly, and records what snprintf() produced so the
// test can check ulog rendered the same text. The <> markers keep one case
// from matching inside another.
#define LOG_LIKE_PRINTF(expected, fmt, ...)                         \
    do                                                              \
    {                                                               \
        char e_[512];                                               \
        std::snprintf(e_, sizeof e_, "<" fmt ">", __VA_ARGS__);     \
        (expected).push_back(e_);                                   \
        log_info("<" fmt ">", __VA_ARGS__);                         \
    } while (0)

bool testMatchesPrintf()
{
    std::cout << "Test 6 (every supported conversion renders like printf): ";

    std::vector<std::string> expected;
    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            LOG_LIKE_PRINTF(expected, "100%% of %d%%", 7);
            LOG_LIKE_PRINTF(expected, "c=%c|%-3c|%3c", 'x', 'y', 'z');
            LOG_LIKE_PRINTF(expected, "p=%p", reinterpret_cast<void*>(0x1234));
            LOG_LIKE_PRINTF(expected, "hh=%hhd h=%hd hhu=%hhu hu=%hu", 300, 70000, 300, 70000);
            LOG_LIKE_PRINTF(expected, "hhneg=%hhd hneg=%hd hhx=%hhx hx=%hx", -129, -32769, -1, -1);
            LOG_LIKE_PRINTF(expected, "j=%jd z=%zd t=%td", static_cast<intmax_t>(-5),
                            static_cast<std::ptrdiff_t>(-6), static_cast<std::ptrdiff_t>(-7));
            LOG_LIKE_PRINTF(expected, "lu=%lu llu=%llu ju=%ju zu=%zu tx=%tx", 1UL, 2ULL,
                            static_cast<uintmax_t>(3), static_cast<size_t>(4),
                            static_cast<std::ptrdiff_t>(255));
            LOG_LIKE_PRINTF(expected, "ld=%ld lld=%lld neg", -8L, -9LL);
            LOG_LIKE_PRINTF(expected, "o=%o x=%x X=%X #x=%#x", 8u, 255u, 255u, 255u);
            LOG_LIKE_PRINTF(expected, "flags=%+d|% d|%05d|%-5d|", 3, 3, 3, 3);
            LOG_LIKE_PRINTF(expected, "e=%e g=%g a=%a 5.1f=%5.1f", 1.5, 0.0001, 1.0, 3.14159);
            // Not compared with snprintf(): Windows' C library treats long
            // double as double, but clang/mingw pass an 80-bit long double.
            log_info("<Lf=%Lf>", 2.5L);
            expected.push_back("<Lf=2.500000>");
            LOG_LIKE_PRINTF(expected, "s=%.3s|%10s|%-10s|", "abcdef", "hi", "hi");
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = true;
    for (auto const& e : expected)
    {
        if (!contains(out, e))
        {
            std::cout << "\n    missing: " << e;
            result = false;
        }
    }
    std::cout << (result ? "" : "\n    ") << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testUnsupportedConversions()
{
    std::cout << "Test 7 (unsupported conversions render as <?> and flag truncation): ";

    // None of these may reach a real printf: %n would write through the
    // pointer, and %* / %.* / positional / wide conversions can't be
    // reproduced from the captured arguments. Everything from the first
    // unsupported conversion on renders as <?>.
    int n = 0;
    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            log_info("n: a=%d %n b=%d", 1, &n, 2);
            log_info("star: %*d|%d", 5, 3, 4);
            log_info("dotstar: %.*f", 2, 1.5);
            log_info("positional: %1$d", 6);
            log_info("wide: %ls %lc", L"w", static_cast<wint_t>(L'w'));
            log_info("unknown: %y");
            log_info("trailing %");
            log_info("trailing lenmod %l");
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "n: a=1 <?> b=<?> [ulog_async: truncated]");
    result &= (n == 0);
    result &= contains(out, "star: <?>|<?> [ulog_async: truncated]");
    result &= contains(out, "dotstar: <?> [ulog_async: truncated]");
    result &= contains(out, "positional: <?> [ulog_async: truncated]");
    result &= contains(out, "wide: <?> <?> [ulog_async: truncated]");
    result &= contains(out, "unknown: <?> [ulog_async: truncated]");
    result &= contains(out, "trailing <?> [ulog_async: truncated]");
    result &= contains(out, "trailing lenmod <?> [ulog_async: truncated]");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testOverflow()
{
    std::cout << "Test 8 (argument, message and spec overflow are truncated safely): ";

    // Three 160-byte strings need more argument space than a record has: the
    // third is dropped and the record flagged, but the rest still renders.
    static const std::string a(160, 'a'), b(160, 'b'), c(160, 'c');

    // A message longer than the render buffer (512) is cut at 511 characters.
    static const std::string longFmt = std::string(600, 'x') + "%d";

    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            log_info("args: %s|%s|%s|end", a.c_str(), b.c_str(), c.c_str());
            log_info(longFmt.c_str(), 1);
            // Flags/width longer than ulog copies (24 chars): the value still
            // comes out, just without the full width.
            log_info("prefix: [%0000000000000000000000000007d]", 42);
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "args: " + a + "|" + b + "|<?>|end [ulog_async: truncated]");
    result &= !contains(out, std::string(10, 'c'));
    result &= contains(out, std::string(511, 'x'));
    result &= !contains(out, std::string(512, 'x'));
    result &= contains(out, "prefix: [") && contains(out, "42]");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 2000) << "\n---\n";
    return result;
}

bool testRemainingConversions()
{
    std::cout << "Test 10 (%i, %F, %E, %G and %A render like printf): ";

    std::vector<std::string> expected;
    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            LOG_LIKE_PRINTF(expected, "i=%i|%+5i|%-4i|", -12, 7, 3);
            LOG_LIKE_PRINTF(expected, "F=%F E=%E G=%G A=%A", 1.5, 12345.678, 0.00001234, 1.0);
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = true;
    for (auto const& e : expected)
    {
        if (!contains(out, e))
        {
            std::cout << "\n    missing: " << e;
            result = false;
        }
    }
    std::cout << (result ? "" : "\n    ") << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testArgumentSpaceRunsOut()
{
    std::cout << "Test 11 (an argument of any type that doesn't fit renders as <?>, as do the rest): ";

    // Two 160-character strings (2 + 160 bytes each), seven 8-byte integers
    // and one 4-byte char fill a record's 384 bytes of argument space
    // exactly, so the next argument -- of each type in turn -- doesn't fit.
    static const std::string a(160, 'a'), b(160, 'b');
#define FILL_FMT "%s|%s|%lld|%lld|%lld|%lld|%lld|%lld|%lld|%c|"
#define FILL_ARGS a.c_str(), b.c_str(), 1LL, 2LL, 3LL, 4LL, 5LL, 6LL, 7LL, 'z'
    const std::string filled = a + "|" + b + "|1|2|3|4|5|6|7|z|";

    // A message that doesn't fit the 512-byte render buffer is cut off in
    // the middle of a conversion's output.
    static const std::string longFmt = std::string(505, 'x') + "%lld";

    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            log_info("s64: " FILL_FMT "%d|%d|end", FILL_ARGS, 8, 9);
            log_info("u64: " FILL_FMT "%u|%d|end", FILL_ARGS, 8u, 9);
            log_info("dbl: " FILL_FMT "%f|%d|end", FILL_ARGS, 8.0, 9);
            log_info("chr: " FILL_FMT "%c|%d|end", FILL_ARGS, 'y', 9);
            log_info("ptr: " FILL_FMT "%p|%d|end", FILL_ARGS, (void*)&a, 9);
            log_info("str: " FILL_FMT "%s|%d|end", FILL_ARGS, "eight", 9);
            log_info(longFmt.c_str(), 1234567890123LL);
        });
        rt.join();
        ulog_async_flush();
    });
#undef FILL_FMT
#undef FILL_ARGS

    bool result = true;
    for (const char* type : {"s64", "u64", "dbl", "chr", "ptr", "str"})
    {
        std::string line = std::string(type) + ": " + filled + "<?>|<?>|end [ulog_async: truncated]";
        if (!contains(out, line))
        {
            std::cout << "\n    missing: " << type << ": ...|z|<?>|<?>|end [ulog_async: truncated]";
            result = false;
        }
    }
    result &= contains(out, std::string(505, 'x') + "123456");
    result &= !contains(out, std::string(505, 'x') + "1234567");

    std::cout << (result ? "" : "\n    ") << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 4000) << "\n---\n";
    return result;
}

void enqueueDirect(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ulog_async_enqueue(LOG_INFO, __FILE__, __LINE__, fmt, ap);
    va_end(ap);
}

bool testLifecycleEdges()
{
    std::cout << "Test 9 (start/stop/flush are idempotent; enqueue while stopped counts a drop): ";

    bool result = true;
    std::string out = captureStderr([&]() {
        // Already running: a second start is a no-op and logging still works.
        ulog_async_start();
        log_info("after double start %d", 1);
        ulog_async_flush();

        ulog_async_stop();
        ulog_async_stop(); // no-op
        ulog_async_flush(); // no-op while stopped; must not hang

        // Enqueueing directly while stopped can't be delivered: it's counted.
        unsigned long before = ulog_async_dropped_count();
        enqueueDirect("never delivered %d", 2);
        result &= (ulog_async_dropped_count() == before + 1);

        ulog_async_start();
        log_info("after restart %d", 3);
        ulog_async_flush();
    });

    result &= contains(out, "after double start 1");
    result &= !contains(out, "never delivered");
    result &= contains(out, "after restart 3");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

// The lock hook runs on the consumer thread when it emits (covers the
// consumer-side branch of ulog_async_is_active()), and a level change made
// from the hook mid-drain must suppress the remaining queued records.
bool testConsumerSideLockHook()
{
    std::cout << "Test 12 (lock hook on consumer thread; mid-drain level change filters rest): ";

    ulog_set_level(LOG_INFO);
    g_hookFired.store(0);
    g_hookMode.store(1);
    ulog_set_lock(lockHook, nullptr);

    std::string out = captureStderr([&]() {
        log_info("hook-passes %d", 1);
        log_info("hook-filtered %d", 2);
        log_info("hook-filtered %d", 3);
        ulog_async_flush();
    });

    ulog_set_lock(nullptr, nullptr);
    g_hookMode.store(0);
    ulog_set_level(LOG_INFO);

    bool result = contains(out, "hook-passes 1") &&
                  !contains(out, "hook-filtered 2") &&
                  !contains(out, "hook-filtered 3") &&
                  g_hookFired.load() == 1;

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

// Conversion forms the walker must classify but may not all get exercised by
// the printf-parity tests: %c and %p, an unterminated '*' spec (mid-format and
// at end of format -- the spec-length walker has to consume to end-of-string),
// a trailing %n, and a multi-digit precision.
bool testConversionClassificationEdges()
{
    std::cout << "Test 13 (%c, %p, unterminated '*' specs, trailing %n, multi-digit precision): ";

    int probe = 0;
    char expectedPtr[64];
    snprintf(expectedPtr, sizeof(expectedPtr), "ptr %p end", (void*)&probe); // %p's form is the platform's
    std::string out = captureStderr([&]() {
        log_info("chr %c end", 'A');
        log_info("ptr %p end", &probe);
        log_info("star at end %5*");
        log_info("star mid %5* %d end", 7);
        log_info("dotstar at end %.*");
        log_info("trailing-n %n");
        log_info("precision %23.04lld end", 1234567890123LL);
        log_info("misc %zu %jd %zd %u %hhd %c end", (size_t)11, (intmax_t)12, (ptrdiff_t)13, 14u, 15, 'Z');
        ulog_async_flush();
    });

    bool result = contains(out, "chr A end") &&
                  contains(out, expectedPtr) &&
                  contains(out, "star at end <?>") &&
                  contains(out, "star mid <?>") &&
                  contains(out, "dotstar at end <?>") &&
                  contains(out, "trailing-n <?>") &&
                  contains(out, "1234567890123") &&
                  contains(out, "misc 11 12 13 14 15 Z end");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

// An unsupported conversion and the truncation marker that land at the very
// end of a render buffer must not exceed it: the "doesn't fit" sides of both
// appends get exercised, and the record must still be emitted safely.
bool testCapBoundaryAppend()
{
    std::cout << "Test 14 (unsupported conversion and truncation marker at render cap): ";

    const std::size_t kStr = 160; // mirrors the private kStrMax cap in ulog_async.cpp
    std::string probeFmt = "probeF" + std::string(505, 'q') + "%n";
    unsigned long before = ulog_async_dropped_count();

    // Three cap-boundary shapes:
    //  a) long literal + %s: the conversion output runs exactly to the cap
    //     and is cut, with no trailing conversion;
    //  b) long literal + %s + %n: the <?> append lands when the buffer is
    //     already full and must be refused;
    //  c) long %s at a small offset: the <?> append lands exactly at the cap
    //     boundary and is refused too.
    // The truncated flag makes the consumer also try the fixed marker after
    // a full buffer, which must be refused as well. None of these may crash
    // or write past the buffer.
    std::string out = captureStderr([&]() {
        log_info("probeA %s %s %n", std::string(kStr, 'x').c_str(), std::string(140, 'y').c_str());
        log_info("probeB %s %n", std::string(kStr - 60, 'z').c_str());
        log_info("probeC %s %n", std::string(430, 'w').c_str());
        log_info("probeD %s %n %s", std::string(370, 'v').c_str(), std::string(320, 'b').c_str());
        log_info("probeE %s %lld", std::string(kStr, 'x').c_str(), 7LL);
        log_info(probeFmt.c_str());
        ulog_async_flush();
    });

    bool result = contains(out, "probeA") &&
                  contains(out, "probeB") &&
                  contains(out, "probeC") &&
                  contains(out, "probeD") &&
                  contains(out, "probeE") &&
                  contains(out, "probeF") &&
                  ulog_async_dropped_count() == before;

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 600) << "---\n";
    return result;
}

// flush() polls (empty ring && consumer idle) for up to 2 s and gives up;
// with a producer logging nonstop it must still return rather than hang.
// (Whether it uses up its whole budget depends on whether the consumer ever
// catches up, which is down to scheduling, so that isn't asserted.)
bool testFlushPollBudgetExhaustion()
{
    std::cout << "Test 15 (flush returns while a producer logs nonstop): ";

    unsigned long before = ulog_async_dropped_count();
    auto t0 = std::chrono::steady_clock::now();

    std::atomic<bool> churn{ true };
    std::atomic<bool> warmed{ false };
    std::thread producer([&]() {
        // Warm the ring first so flush() cannot observe an idle consumer
        // before the churn thread has even started pushing.
        for (int i = 0; i < 600; i++)
        {
            log_info("warm %d", 1);
        }
        warmed.store(true, std::memory_order_release);
        while (churn.load(std::memory_order_relaxed))
        {
            log_info("churn %d", 1);
        }
    });

    while (!warmed.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    ulog_async_flush(); // must return on its own even though the ring never quiets

    churn.store(false);
    producer.join();
    ulog_async_flush(); // second flush now that the churn stopped: drains fully

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    unsigned long after = ulog_async_dropped_count();

    bool result = elapsed < 15000;

    std::cout << (result ? "PASS" : "FAIL")
              << " (elapsed=" << elapsed << "ms dropped+=" << (after - before) << ")\n";
    return result;
}

// Precision run with no digits: when the character right after the '.' of
// a precision is below '0' (a '!' or another '%'), the precision digit scan
// ends on its very first test -- the only exit besides running off the
// digits. Neither character is a valid conversion, so both records render
// as <?> with the truncation flag, deterministically.
bool testEmptyPrecision()
{
    std::cout << "Test 16 (precision '.' with no digits ends the spec scan): ";

    std::string out = captureStderr([&]() {
        log_info("dotbang: %.! end");
        log_info("dotdot: %.% end");
        ulog_async_flush();
    });

    bool result = contains(out, "dotbang: <?> end [ulog_async: truncated]") &&
                  contains(out, "dotdot: <?> end [ulog_async: truncated]");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

} // namespace

int main()
{
    bool result = true;

    result &= testDeferredFormatting();
    result &= testMainThreadRoutedThroughAsync();
    result &= testNullAndLongString();
    result &= testSynchronousFallbackWhenStopped();
    result &= testMultiProducerInvariants();
    result &= testMatchesPrintf();
    result &= testUnsupportedConversions();
    result &= testOverflow();
    result &= testLifecycleEdges();
    result &= testRemainingConversions();
    result &= testArgumentSpaceRunsOut();
    result &= testConsumerSideLockHook();
    result &= testConversionClassificationEdges();
    result &= testCapBoundaryAppend();
    result &= testFlushPollBudgetExhaustion();
    result &= testEmptyPrecision();

    return result ? 0 : -1;
}
