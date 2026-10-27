// Tests for ulog's configuration API: level filtering, quiet mode, the lock
// hook, the custom prefix hook, level strings and ulog_event_to_cstr().
//
// Logging goes through the async front end (started before main()), so each
// test flushes it with ulog_async_flush() before inspecting captured stderr.
// Every test restores the default configuration before returning.

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>

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
#include <fcntl.h>
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
#include "../ulog_logger.h"
#include <websocketpp/concurrency/basic.hpp>

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

// Runs fn with stderr redirected to a temp file, flushing the async logger
// before restoring stderr, and returns what was written.
std::string captureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
#if defined(_WIN32)
    char path[L_tmpnam];
    tmpnam(path);
    int fd = _open(path, _O_CREAT | _O_RDWR | _O_BINARY, 0600);
#else
    char path[] = "/tmp/ulog_config_test_XXXXXX";
    int fd = mkstemp(path);
#endif
    if (fd < 0)
    {
        return "<capture failed>";
    }
    int saved = DUP(FILENO(stderr));
    DUP2(fd, FILENO(stderr));

    fn();
    ulog_async_flush();

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

bool testLevelStrings()
{
    std::cout << "Test 1 (level strings): ";

    bool result = CHECK(std::string(ulog_get_level_string(LOG_TRACE)) == "TRACE");
    result &= CHECK(std::string(ulog_get_level_string(LOG_DEBUG)) == "DEBUG");
    result &= CHECK(std::string(ulog_get_level_string(LOG_INFO)) == "INFO");
    result &= CHECK(std::string(ulog_get_level_string(LOG_WARN)) == "WARN");
    result &= CHECK(std::string(ulog_get_level_string(LOG_ERROR)) == "ERROR");
    result &= CHECK(std::string(ulog_get_level_string(LOG_FATAL)) == "FATAL");
    return report(result);
}

bool testSetLevel()
{
    std::cout << "Test 2 (set_level filters lower levels): ";

    std::string out = captureStderr([]() {
        ulog_set_level(LOG_WARN);
        log_debug("level-test debug");
        log_info("level-test info");
        log_warn("level-test warn");
        log_error("level-test error");
        ulog_async_flush();
        ulog_set_level(LOG_TRACE);
        log_trace("level-test trace after restore");
    });

    bool result = CHECK(!contains(out, "level-test debug"));
    result &= CHECK(!contains(out, "level-test info"));
    result &= CHECK(contains(out, "WARN"));
    result &= CHECK(contains(out, "level-test warn"));
    result &= CHECK(contains(out, "level-test error"));
    result &= CHECK(contains(out, "level-test trace after restore"));
    return report(result);
}

bool testSetQuiet()
{
    std::cout << "Test 3 (quiet mode suppresses output): ";

    std::string out = captureStderr([]() {
        ulog_set_quiet(true);
        log_error("quiet-test hidden");
        ulog_async_flush();
        ulog_set_quiet(false);
        log_error("quiet-test shown");
    });

    bool result = CHECK(!contains(out, "quiet-test hidden"));
    result &= CHECK(contains(out, "quiet-test shown"));
    return report(result);
}

struct LockState
{
    std::atomic<int> locks{0};
    std::atomic<int> unlocks{0};
    std::atomic<int> depth{0};
    std::atomic<bool> unbalanced{false};
};

void countingLock(bool lock, void* arg)
{
    auto state = static_cast<LockState*>(arg);
    if (lock)
    {
        state->locks++;
        if (state->depth.fetch_add(1) != 0) state->unbalanced = true;
    }
    else
    {
        state->unlocks++;
        if (state->depth.fetch_sub(1) != 1) state->unbalanced = true;
    }
}

bool testSetLock()
{
    std::cout << "Test 4 (lock hook brackets every emitted line): ";

    LockState state;
    std::string out = captureStderr([&]() {
        ulog_set_lock(countingLock, &state);
        log_info("lock-test 1");
        log_info("lock-test 2");
        log_info("lock-test 3");
        ulog_async_flush();
        ulog_set_lock(nullptr, nullptr);
    });

    bool result = CHECK(contains(out, "lock-test 3"));
    result &= CHECK(state.locks >= 3);
    result &= CHECK(state.locks == state.unlocks);
    result &= CHECK(!state.unbalanced);

    // With the hook removed, logging no longer calls it.
    int before = state.locks;
    captureStderr([]() { log_info("lock-test after removal"); });
    result &= CHECK(state.locks == before);
    return report(result);
}

void testPrefix(ulog_Event* ev, char* prefix, size_t prefixSize)
{
    snprintf(prefix, prefixSize, " [pfx:%s]", ulog_get_level_string(ev->level));
}

bool testPrefixFn()
{
    std::cout << "Test 5 (custom prefix hook): ";

    std::string out = captureStderr([]() {
        ulog_set_prefix_fn(testPrefix);
        log_warn("prefix-test warn");
        ulog_async_flush();
        ulog_set_prefix_fn(nullptr);
        log_warn("prefix-test plain");
    });

    bool result = CHECK(contains(out, "[pfx:WARN]"));
    result &= CHECK(contains(out, "prefix-test warn"));
    // After clearing the hook, no prefix.
    auto plainLine = out.substr(out.find("prefix-test plain") == std::string::npos ? 0 : out.rfind('\n', out.find("prefix-test plain")) + 1);
    result &= CHECK(contains(out, "prefix-test plain"));
    result &= CHECK(!contains(plainLine.substr(0, plainLine.find('\n')), "[pfx:"));
    return report(result);
}

// Builds a ulog_Event with a real va_list (as the logging path does) and
// renders it with ulog_event_to_cstr().
int renderEvent(char* out, size_t outSize, struct tm* time, int level, const char* fmt, ...)
{
    ulog_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.message = fmt;
    ev.file = "somefile.cpp";
    ev.line = 42;
    ev.level = level;
#if FEATURE_TIME
    ev.time = time;
#ifdef ULOG_ASYNC
    ev.time_is_borrowed = true;
#endif
#else
    (void)time;
#endif
    va_start(ev.message_format_args, fmt);
    int rv = ulog_event_to_cstr(&ev, out, outSize);
    va_end(ev.message_format_args);
    return rv;
}

bool testEventToCstr()
{
    std::cout << "Test 6 (ulog_event_to_cstr renders and truncates safely): ";

    struct tm tmBuf;
    memset(&tmBuf, 0, sizeof(tmBuf));
    tmBuf.tm_hour = 12;
    tmBuf.tm_min = 34;
    tmBuf.tm_sec = 56;

    char out[256];
    memset(out, 'X', sizeof(out));
    bool result = CHECK(renderEvent(out, sizeof(out), &tmBuf, LOG_WARN, "value=%d name=%s", 7, "abc") == 0);
    std::string rendered(out, strnlen(out, sizeof(out)));
    result &= CHECK(strnlen(out, sizeof(out)) < sizeof(out));
    result &= CHECK(contains(rendered, "WARN"));
    result &= CHECK(contains(rendered, "somefile.cpp:42:"));
    result &= CHECK(contains(rendered, "value=7 name=abc"));
    // No color codes or trailing newline in the string form.
    result &= CHECK(!contains(rendered, "\x1b["));
    result &= CHECK(rendered.empty() || rendered.back() != '\n');
#if FEATURE_TIME
    result &= CHECK(contains(rendered, "12:34:56"));
#endif

    // A buffer too small for the message must still come back as a
    // NUL-terminated string within the buffer, so callers can print it.
    char small[16];
    memset(small, 'X', sizeof(small));
    renderEvent(small, sizeof(small), &tmBuf, LOG_WARN, "a long message that does not fit %d", 12345);
    result &= CHECK(strnlen(small, sizeof(small)) < sizeof(small));
    if (strnlen(small, sizeof(small)) == sizeof(small))
    {
        std::cout << "[16-byte buffer not NUL-terminated: \"" << std::string(small, sizeof(small)) << "\"] ";
    }

    return report(result);
}

// The websocketpp->ulog adapter (src/util/ulog_logger.h) is the logger type
// SocketIoClient's custom_config installs. Exercise its channel bookkeeping
// directly: enable/disable channels, the set_channels(none) shortcut (which
// must clear everything and enable nothing), and the write() gate.
bool testWebsocketppUlogAdapter()
{
    std::cout << "Test 7 (websocketpp ulog logger adapter channels): ";

    websocketpp::log::ulog<websocketpp::concurrency::basic,
        websocketpp::log::alevel> logger;

    logger.set_channels(websocketpp::log::alevel::connect);
    bool result = CHECK(logger.static_test(websocketpp::log::alevel::connect));
    result &= CHECK(logger.dynamic_test(websocketpp::log::alevel::connect));
    result &= CHECK(!logger.dynamic_test(websocketpp::log::alevel::fail));

    std::string captured = captureStderr([&]() {
        logger.write(websocketpp::log::alevel::connect, std::string("open"));
        logger.write(websocketpp::log::alevel::fail, "must not appear");
    });
    result &= CHECK(contains(captured, "[connect] open"));
    result &= CHECK(!contains(captured, "must not appear"));

    logger.set_channels(websocketpp::log::alevel::none);
    result &= CHECK(!logger.dynamic_test(websocketpp::log::alevel::connect));

    logger.set_channels(websocketpp::log::alevel::all);
    result &= CHECK(logger.dynamic_test(websocketpp::log::alevel::fail));
    logger.clear_channels(websocketpp::log::alevel::all);
    result &= CHECK(!logger.dynamic_test(websocketpp::log::alevel::fail));

    return report(result);
}

// Edge inputs to ulog_event_to_cstr(): a zero-size buffer and one so large
// fmemopen() cannot back it must both fail cleanly without touching the
// caller's buffer.
bool testEventToCstrEdges()
{
    std::cout << "Test 8 (ulog_event_to_cstr rejects zero/huge buffers): ";

    struct tm tmBuf;
    memset(&tmBuf, 0, sizeof(tmBuf));
    tmBuf.tm_hour = 1;
    tmBuf.tm_min = 2;
    tmBuf.tm_sec = 3;

    char out[8];
    memset(out, 'X', sizeof(out));

    bool result = CHECK(renderEvent(out, 0, &tmBuf, LOG_WARN, "no room") == -1);
    result &= CHECK(out[0] == 'X' && out[sizeof(out) - 1] == 'X');

    memset(out, 'X', sizeof(out));
    result &= CHECK(renderEvent(out, (size_t)-1, &tmBuf, LOG_WARN, "huge %d", 1) == -1);
    result &= CHECK(out[0] == 'X' && out[sizeof(out) - 1] == 'X');

    return report(result);
}

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testLevelStrings();
    result &= testSetLevel();
    result &= testSetQuiet();
    result &= testSetLock();
    result &= testPrefixFn();
    result &= testEventToCstr();
    result &= testWebsocketppUlogAdapter();
    result &= testEventToCstrEdges();

    return result ? 0 : -1;
}
