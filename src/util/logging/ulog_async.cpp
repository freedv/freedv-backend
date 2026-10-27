//=========================================================================
// Name:            ulog_async.cpp
// Purpose:         Real-time-safe front end for ulog. See ulog_async.h.
//
// Design
// ------
// * The consumer thread starts automatically before main() (a static
//   constructor calls ulog_async_start()) and stops at process exit. Nothing
//   in the application has to be called to enable async logging.
// * ulog_log() (in ulog.c) checks ulog_async_is_active(); while the consumer is
//   running it calls ulog_async_enqueue() from *every* thread instead of
//   formatting/writing inline. The consumer thread's own log calls, and the
//   brief windows before start-up / after shutdown, take ulog's synchronous
//   path.
// * ulog_async_enqueue() captures the call into a fixed-size record: level,
//   file pointer, line, a timestamp, and the *unformatted* arguments
//   serialized into an inline byte buffer (integers/floats by value, strings
//   copied inline). It never allocates, never locks, never touches stdio.
// * Records go into a bounded lock-free MPSC ring (Vyukov bounded queue).
//   Ring full => the record is dropped and an atomic counter is bumped.
// * A dedicated consumer thread waits on a semaphore, pops records, renders
//   them with snprintf(), and hands the finished string to
//   ulog_log_prerendered() which runs ulog's normal output path (stdout
//   callback, extra outputs, file, custom prefix) under the usual lock.
//
// Authors:         Mooneer Salem
// License:
//
//  All rights reserved.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#include "ulog_async.h"

#include "ulog.h"
#include "../Semaphore.h"
#include "../freedv_sanitizers.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

namespace {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Ring depth (must be a power of two).
constexpr std::size_t kQueueLen = 256;

// Bytes of serialized arguments (including inline string copies) per record.
constexpr std::size_t kArgBufLen = 384;

// Largest string (%s) argument captured; longer strings are truncated.
constexpr std::size_t kStrMax = 160;

// Consumer-side render buffer for the finished message text.
constexpr std::size_t kRenderLen = 512;

// Longest flags/width/precision run we copy out of a conversion spec.
constexpr std::size_t kSpecPrefixMax = 24;

static_assert((kQueueLen & (kQueueLen - 1)) == 0, "kQueueLen must be a power of two");

// ---------------------------------------------------------------------------
// Record + ring
// ---------------------------------------------------------------------------

struct Record
{
    const char*   fmt;      // static lifetime (string literal)
    const char*   file;     // static lifetime (__FILE__)
    int           line;
    int           level;
    std::int64_t  tvSec;
    std::int32_t  tvNsec;
    std::uint16_t argLen;   // valid bytes in args
    bool          truncated;
    unsigned char args[kArgBufLen];
};

struct Cell
{
    std::atomic<std::size_t> seq;
    Record rec;
};

// All storage is static: no allocation anywhere on the real-time path.
Cell g_ring[kQueueLen];
std::atomic<std::size_t> g_enqueuePos{0};
std::atomic<std::size_t> g_dequeuePos{0};
std::atomic<bool>        g_ringReady{false};

std::atomic<unsigned long> g_dropped{0};
unsigned long              g_droppedReported = 0; // consumer-only

std::atomic<bool>       g_running{false};
std::atomic<Semaphore*> g_sem{nullptr};
std::thread             g_consumer;

// True while the consumer is parked on the semaphore with nothing left to do,
// i.e. its last drain() pass has fully returned (every fprintf completed). Used
// by ulog_async_flush() so it waits for emission to finish, not just for the
// ring counters to line up.
std::atomic<bool>       g_consumerIdle{false};

// Set only on the consumer thread, so its own log_*() calls (and anything the
// output path logs re-entrantly) take ulog's synchronous path instead of
// enqueueing back onto the ring.
thread_local bool tl_isConsumer = false;

void ringInit()
{
    for (std::size_t i = 0; i < kQueueLen; ++i)
    {
        g_ring[i].seq.store(i, std::memory_order_relaxed);
    }
    g_enqueuePos.store(0, std::memory_order_relaxed);
    g_dequeuePos.store(0, std::memory_order_relaxed);
    g_consumerIdle.store(false, std::memory_order_relaxed);
    g_ringReady.store(true, std::memory_order_release);
}

// Vyukov bounded MPSC: producers contend on g_enqueuePos, the single consumer
// owns g_dequeuePos. Returns false (does not block) when the ring is full.
bool ringPush(const Record& src) FREEDV_NONBLOCKING
{
    Cell* cell;
    std::size_t pos = g_enqueuePos.load(std::memory_order_relaxed);
    for (;;)
    {
        cell = &g_ring[pos & (kQueueLen - 1)];
        std::size_t seq = cell->seq.load(std::memory_order_acquire);
        std::intptr_t diff =
            static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
        if (diff == 0)
        {
            if (g_enqueuePos.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed))
            {
                break;
            }
        }
        else if (diff < 0)
        {
            return false; // full
        }
        else
        {
            pos = g_enqueuePos.load(std::memory_order_relaxed);
        }
    }

    // Copy only the header plus the used argument bytes.
    const std::size_t head = offsetof(Record, args);
    std::memcpy(&cell->rec, &src, head + src.argLen);
    cell->rec.argLen = src.argLen;

    cell->seq.store(pos + 1, std::memory_order_release);
    return true;
}

bool ringPop(Record& dst)
{
    Cell* cell;
    std::size_t pos = g_dequeuePos.load(std::memory_order_relaxed);
    for (;;)
    {
        cell = &g_ring[pos & (kQueueLen - 1)];
        std::size_t seq = cell->seq.load(std::memory_order_acquire);
        std::intptr_t diff =
            static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
        if (diff == 0)
        {
            if (g_dequeuePos.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed))
            {
                break;
            }
        }
        else if (diff < 0)
        {
            return false; // empty
        }
        else
        {
            pos = g_dequeuePos.load(std::memory_order_relaxed);
        }
    }

    const std::size_t head = offsetof(Record, args);
    std::memcpy(&dst, &cell->rec, head + cell->rec.argLen);
    dst.argLen = cell->rec.argLen;

    cell->seq.store(pos + kQueueLen, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// printf format-string walker (shared by producer and consumer)
// ---------------------------------------------------------------------------

enum ArgKind
{
    AK_NONE,        // "%%"
    AK_S64,         // any signed integer conversion
    AK_U64,         // any unsigned integer conversion
    AK_DBL,         // any floating conversion
    AK_CHR,         // %c
    AK_STR,         // %s
    AK_PTR,         // %p
    AK_UNSUPPORTED  // %n, %*, %.*, positional, %ls/%lc, trailing '%'
};

enum LenMod { LM_NONE, LM_h, LM_hh, LM_l, LM_ll, LM_j, LM_z, LM_t, LM_L };

struct Spec
{
    ArgKind kind;
    LenMod  lenmod;
    char    conv;                       // conversion character
    std::size_t totalLen;               // chars from '%' through conv inclusive
    char    prefix[kSpecPrefixMax + 1]; // flags/width/precision, NUL-terminated
};

// For a spec we can't reproduce, finds the end of the whole conversion
// (flags, width, precision, '*', '$', length modifier and the conversion
// character), so none of it leaks into the output as literal text.
std::size_t unsupportedSpecLen(const char* start, const char* p) FREEDV_NONBLOCKING
{
    while (*p != '\0' && std::strchr("0123456789.*$-+ #hljztL", *p) != nullptr)
    {
        ++p;
    }
    if (*p != '\0')
    {
        ++p; // conversion character
    }
    return static_cast<std::size_t>(p - start);
}

// p points at a '%'. Fills out; returns nothing (out.kind == AK_UNSUPPORTED on
// anything we cannot safely reproduce later).
void parseSpec(const char* p, Spec& out) FREEDV_NONBLOCKING
{
    const char* start = p;
    ++p; // skip '%'

    out.lenmod = LM_NONE;
    out.conv   = '\0';
    out.prefix[0] = '\0';

    if (*p == '%')
    {
        out.kind = AK_NONE;
        out.totalLen = 2;
        return;
    }

    const char* prefixStart = p;

    // flags
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
    {
        ++p;
    }

    // width
    if (*p == '*')
    {
        out.kind = AK_UNSUPPORTED;
        out.totalLen = unsupportedSpecLen(start, p);
        return;
    }
    while (*p >= '0' && *p <= '9')
    {
        ++p;
    }
    if (*p == '$')
    {
        // positional argument
        out.kind = AK_UNSUPPORTED;
        out.totalLen = unsupportedSpecLen(start, p);
        return;
    }

    // precision
    if (*p == '.')
    {
        ++p;
        if (*p == '*')
        {
            out.kind = AK_UNSUPPORTED;
            out.totalLen = unsupportedSpecLen(start, p);
            return;
        }
        while (*p >= '0' && *p <= '9')
        {
            ++p;
        }
    }

    // copy the flags/width/precision run
    std::size_t prefixLen = static_cast<std::size_t>(p - prefixStart);
    if (prefixLen > kSpecPrefixMax)
    {
        prefixLen = kSpecPrefixMax;
    }
    std::memcpy(out.prefix, prefixStart, prefixLen);
    out.prefix[prefixLen] = '\0';

    // length modifier
    if (*p == 'h')
    {
        ++p;
        if (*p == 'h') { ++p; out.lenmod = LM_hh; }
        else           { out.lenmod = LM_h; }
    }
    else if (*p == 'l')
    {
        ++p;
        if (*p == 'l') { ++p; out.lenmod = LM_ll; }
        else           { out.lenmod = LM_l; }
    }
    else if (*p == 'j') { ++p; out.lenmod = LM_j; }
    else if (*p == 'z') { ++p; out.lenmod = LM_z; }
    else if (*p == 't') { ++p; out.lenmod = LM_t; }
    else if (*p == 'L') { ++p; out.lenmod = LM_L; }

    char c = *p;
    if (c == '\0')
    {
        out.kind = AK_UNSUPPORTED;
        out.totalLen = static_cast<std::size_t>(p - start);
        return;
    }
    out.conv = c;
    out.totalLen = static_cast<std::size_t>(p - start) + 1;

    switch (c)
    {
        case 'd':
        case 'i':
            out.kind = AK_S64;
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            out.kind = AK_U64;
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            out.kind = AK_DBL;
            break;
        case 'c':
            out.kind = (out.lenmod == LM_l) ? AK_UNSUPPORTED : AK_CHR;
            break;
        case 's':
            out.kind = (out.lenmod == LM_l) ? AK_UNSUPPORTED : AK_STR;
            break;
        case 'p':
            out.kind = AK_PTR;
            break;
        case 'n':
        default:
            out.kind = AK_UNSUPPORTED;
            break;
    }
}

// ---------------------------------------------------------------------------
// Producer: serialize arguments
// ---------------------------------------------------------------------------

struct BlobWriter
{
    unsigned char* buf;
    std::size_t    cap;
    std::size_t    len;
    bool           overflow;

    bool put(const void* p, std::size_t n) FREEDV_NONBLOCKING
    {
        if (len + n > cap)
        {
            overflow = true;
            return false;
        }
        std::memcpy(buf + len, p, n);
        len += n;
        return true;
    }
};

void serializeArgs(const char* fmt, va_list ap, Record& rec) FREEDV_NONBLOCKING
{
    BlobWriter w{rec.args, kArgBufLen, 0, false};
    rec.truncated = false;

    for (const char* p = fmt; *p != '\0'; )
    {
        if (*p != '%')
        {
            ++p;
            continue;
        }

        Spec s;
        parseSpec(p, s);
        p += s.totalLen;

        // All kind dispatch lives in the switch below. With AK_NONE and
        // AK_UNSUPPORTED as explicit cases it covers every ArgKind value
        // parseSpec can return -- which is also why the former `if` guards
        // here and the former `default:` arm further down were dead.
        bool ok = true;
        switch (s.kind)
        {
            case AK_NONE:
                break; // "%%" carries no argument; ok stays true
            case AK_UNSUPPORTED:
                // The `if (!ok)` path below flags the record and stops the
                // format scan -- the same effect as the old early break.
                ok = false;
                break;
            case AK_S64:
            {
                long long v;
                switch (s.lenmod)
                {
                    case LM_l:  v = static_cast<long long>(va_arg(ap, long)); break;
                    case LM_ll: v = va_arg(ap, long long); break;
                    case LM_j:  v = static_cast<long long>(va_arg(ap, intmax_t)); break;
                    case LM_z:  v = static_cast<long long>(va_arg(ap, std::size_t)); break;
                    case LM_t:  v = static_cast<long long>(va_arg(ap, std::ptrdiff_t)); break;
                    // As printf does, narrow h/hh arguments (passed as int).
                    case LM_hh: v = static_cast<signed char>(va_arg(ap, int)); break;
                    case LM_h:  v = static_cast<short>(va_arg(ap, int)); break;
                    default:    v = static_cast<long long>(va_arg(ap, int)); break;
                }
                ok = w.put(&v, sizeof v);
                break;
            }
            case AK_U64:
            {
                unsigned long long v;
                switch (s.lenmod)
                {
                    case LM_l:  v = static_cast<unsigned long long>(va_arg(ap, unsigned long)); break;
                    case LM_ll: v = va_arg(ap, unsigned long long); break;
                    case LM_j:  v = static_cast<unsigned long long>(va_arg(ap, uintmax_t)); break;
                    case LM_z:  v = static_cast<unsigned long long>(va_arg(ap, std::size_t)); break;
                    case LM_t:  v = static_cast<unsigned long long>(va_arg(ap, std::size_t)); break;
                    case LM_hh: v = static_cast<unsigned char>(va_arg(ap, unsigned int)); break;
                    case LM_h:  v = static_cast<unsigned short>(va_arg(ap, unsigned int)); break;
                    default:    v = static_cast<unsigned long long>(va_arg(ap, unsigned int)); break;
                }
                ok = w.put(&v, sizeof v);
                break;
            }
            case AK_DBL:
            {
                double v = (s.lenmod == LM_L)
                               ? static_cast<double>(va_arg(ap, long double))
                               : va_arg(ap, double);
                ok = w.put(&v, sizeof v);
                break;
            }
            case AK_CHR:
            {
                int v = va_arg(ap, int);
                ok = w.put(&v, sizeof v);
                break;
            }
            case AK_PTR:
            {
                void* v = va_arg(ap, void*);
                ok = w.put(&v, sizeof v);
                break;
            }
            case AK_STR:
            {
                const char* str = va_arg(ap, const char*);
                std::uint16_t n;
                if (str == nullptr)
                {
                    n = 0xFFFF; // NULL sentinel
                    ok = w.put(&n, sizeof n);
                }
                else
                {
                    std::size_t sl = 0;
                    while (sl < kStrMax && str[sl] != '\0')
                    {
                        ++sl;
                    }
                    if (str[sl] != '\0')
                    {
                        rec.truncated = true; // source string was longer than kStrMax
                    }
                    n = static_cast<std::uint16_t>(sl);
                    ok = w.put(&n, sizeof n) && w.put(str, sl);
                }
                break;
            }
        }

        if (!ok)
        {
            rec.truncated = true;
            break;
        }
    }

    rec.argLen = static_cast<std::uint16_t>(w.len);
    if (w.overflow)
    {
        rec.truncated = true;
    }
}

// ---------------------------------------------------------------------------
// Consumer: render
// ---------------------------------------------------------------------------

struct BlobReader
{
    const unsigned char* buf;
    std::size_t          len;
    std::size_t          pos;

    bool get(void* p, std::size_t n)
    {
        if (pos + n > len)
        {
            return false;
        }
        std::memcpy(p, buf + pos, n);
        pos += n;
        return true;
    }
    const unsigned char* peek(std::size_t n)
    {
        if (pos + n > len)
        {
            return nullptr;
        }
        const unsigned char* r = buf + pos;
        pos += n;
        return r;
    }
};

void appendLiteral(char* out, std::size_t cap, std::size_t& used, char ch)
{
    if (used + 1 < cap)
    {
        out[used++] = ch;
        out[used] = '\0';
    }
}

void renderMessage(const Record& rec, char* out, std::size_t cap)
{
    std::size_t used = 0;
    out[0] = '\0';

    BlobReader r{rec.args, rec.argLen, 0};
    bool argsExhausted = false;

    for (const char* p = rec.fmt; *p != '\0'; )
    {
        if (*p != '%')
        {
            appendLiteral(out, cap, used, *p);
            ++p;
            continue;
        }

        Spec s;
        parseSpec(p, s);
        p += s.totalLen;

        // Rebuild a single-conversion spec: '%' + prefix + normalized length
        // modifier + conversion char. Every integer is widened to long long,
        // so "ll" makes the snprintf call portable regardless of the original
        // z/j/t/l modifier.
        char spec[kSpecPrefixMax + 8];
        // used <= cap - 1 at every point here: renderMessage starts used at
        // 0 and every update (appendLiteral's guarded store, the fit-checked
        // `used += n` below, and the truncation clamp beside it) keeps used
        // at most cap - 1, so the ternary's ": 0" arm was unreachable.
        std::size_t remain = cap - used;
        std::size_t n = 0;

        switch (s.kind)
        {
            case AK_NONE:
                // A literal "%%" prints a percent even once arguments have
                // run out; it consumes none.
                appendLiteral(out, cap, used, '%');
                continue;
            case AK_UNSUPPORTED:
                // Not reproducible from the captured arguments: mark the
                // record exhausted so this conversion, and every later one,
                // takes the shared "<?>" append below.
                argsExhausted = true;
                break;
            case AK_S64:
            {
                long long v = 0;
                if (!r.get(&v, sizeof v)) { argsExhausted = true; break; }
                std::snprintf(spec, sizeof spec, "%%%sll%c", s.prefix, s.conv);
                n = std::snprintf(out + used, remain, spec, v);
                break;
            }
            case AK_U64:
            {
                unsigned long long v = 0;
                if (!r.get(&v, sizeof v)) { argsExhausted = true; break; }
                std::snprintf(spec, sizeof spec, "%%%sll%c", s.prefix, s.conv);
                n = std::snprintf(out + used, remain, spec, v);
                break;
            }
            case AK_DBL:
            {
                double v = 0.0;
                if (!r.get(&v, sizeof v)) { argsExhausted = true; break; }
                std::snprintf(spec, sizeof spec, "%%%s%c", s.prefix, s.conv);
                n = std::snprintf(out + used, remain, spec, v);
                break;
            }
            case AK_CHR:
            {
                int v = 0;
                if (!r.get(&v, sizeof v)) { argsExhausted = true; break; }
                std::snprintf(spec, sizeof spec, "%%%s%c", s.prefix, s.conv);
                n = std::snprintf(out + used, remain, spec, v);
                break;
            }
            case AK_PTR:
            {
                void* v = nullptr;
                if (!r.get(&v, sizeof v)) { argsExhausted = true; break; }
                std::snprintf(spec, sizeof spec, "%%%sp", s.prefix);
                n = std::snprintf(out + used, remain, spec, v);
                break;
            }
            case AK_STR:
            {
                std::uint16_t sl = 0;
                if (!r.get(&sl, sizeof sl)) { argsExhausted = true; break; }
                char tmp[kStrMax + 1];
                const char* strArg;
                if (sl == 0xFFFF)
                {
                    strArg = "(null)";
                }
                else
                {
                    const unsigned char* sp = r.peek(sl);
                    if (sp == nullptr) { argsExhausted = true; break; }
                    std::memcpy(tmp, sp, sl);
                    tmp[sl] = '\0';
                    strArg = tmp;
                }
                std::snprintf(spec, sizeof spec, "%%%ss", s.prefix);
                n = std::snprintf(out + used, remain, spec, strArg);
                break;
            }
        }

        if (argsExhausted)
        {
            // This argument didn't fit in the record: mark it like every
            // conversion after it rather than leaving it out silently.
            n = std::snprintf(out + used, remain, "%s", "<?>");
        }

        if (n > 0 && used + n < cap)
        {
            used += n;
        }
        else if (n > 0)
        {
            used = cap - 1; // truncated
        }
    }

    if (rec.truncated)
    {
        // Both the `cap > used ?` ternary and the `n > 0` test were dead:
        // used never exceeds cap - 1 (see the remain computation above),
        // and the format here is a pure literal, so snprintf() always
        // returns the message length -- a positive count.
        std::size_t n = std::snprintf(out + used, cap - used,
                                     " [ulog_async: truncated]");
        if (used + n < cap) used += n;
    }
    (void)used;
}

// ---------------------------------------------------------------------------
// Consumer thread
// ---------------------------------------------------------------------------

void nowTimespec(std::int64_t& sec, std::int32_t& nsec) FREEDV_NONBLOCKING
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    // clock_gettime() reads a monotonic/vDSO clock on all supported
    // platforms: no syscall on the fast path, no allocation, no lock.
    // Deliberately not timespec_get(): on Windows, it's only declared once
    // mingw-w64's headers see _UCRT defined, and satisfying it then pulls in
    // libucrtbase.a's implementation alongside the classic msvcrt-targeted
    // rest of the toolchain. That mixing is not safe -- ucrtbase's internal
    // CRT-lock/stream-table plumbing (e.g. what backs __acrt_iob_func) isn't
    // ABI-compatible with msvcrt's, and statically linking both into one
    // binary let UCRT-side internals call into msvcrt's incompatible _lock()
    // with a garbage index, corrupting the vtable-dispatch call in the very
    // next indirect call and crashing. clock_gettime() avoids the whole
    // problem: mingw-w64 provides it via winpthreads on the classic msvcrt
    // target with no UCRT involved at all.
    // Wrapped as verified-safe so RTSan keeps runtime checks without
    // flagging the interceptor.
    FREEDV_BEGIN_VERIFIED_SAFE
    clock_gettime(CLOCK_REALTIME, &ts);
    FREEDV_END_VERIFIED_SAFE
    sec = static_cast<std::int64_t>(ts.tv_sec);
    nsec = static_cast<std::int32_t>(ts.tv_nsec);
}

void emitRecord(const Record& rec)
{
    char msg[kRenderLen];
    renderMessage(rec, msg, sizeof msg);
    ulog_log_prerendered(rec.level, rec.file, rec.line,
                         static_cast<long>(rec.tvSec),
                         static_cast<long>(rec.tvNsec), msg);
}

void drain()
{
    Record rec;
    while (ringPop(rec))
    {
        emitRecord(rec);
    }

    unsigned long dropped = g_dropped.load(std::memory_order_relaxed);
    if (dropped != g_droppedReported)
    {
        char msg[96];
        std::snprintf(msg, sizeof msg,
                      "ulog_async: %lu realtime log record(s) dropped",
                      dropped - g_droppedReported);
        std::int64_t sec;
        std::int32_t nsec;
        nowTimespec(sec, nsec);
        ulog_log_prerendered(LOG_WARN, "ulog_async.cpp", __LINE__,
                             static_cast<long>(sec), static_cast<long>(nsec),
                             msg);
        g_droppedReported = dropped;
    }
}

void consumerMain()
{
    tl_isConsumer = true;

    Semaphore* sem = g_sem.load(std::memory_order_acquire);
    while (g_running.load(std::memory_order_acquire))
    {
        drain();

        // Mark idle only once drain() has fully returned, so a flusher that
        // observes (ring empty && idle) knows every record's fprintf is done.
        g_consumerIdle.store(true, std::memory_order_release);
        if (sem != nullptr)
        {
            sem->waitFor(100); // ms; also a periodic safety drain
        }
        g_consumerIdle.store(false, std::memory_order_release);
    }
    drain(); // final sweep
}

} // namespace

// ===========================================================================
// Public C API
// ===========================================================================

extern "C" bool ulog_async_is_active(void)
{
    // g_ringReady goes true at the tail of ringInit() and false only after the
    // consumer has been joined, so it brackets exactly the interval in which
    // enqueueing is safe and will be drained.
    return g_ringReady.load(std::memory_order_acquire) && !tl_isConsumer;
}

extern "C" void ulog_async_start(void)
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true))
    {
        return; // already running
    }

    g_droppedReported = g_dropped.load(std::memory_order_relaxed);

    // Publish the semaphore before the ring is marked ready so a producer that
    // observes g_ringReady already has something to signal.
    Semaphore* sem = new Semaphore();
    g_sem.store(sem, std::memory_order_release);

    ringInit(); // sets g_ringReady last

    // Start the consumer last; it drains whatever is already queued on entry.
    g_consumer = std::thread(consumerMain);
}

extern "C" void ulog_async_stop(void)
{
    bool expected = true;
    if (!g_running.compare_exchange_strong(expected, false))
    {
        return; // not running
    }

    Semaphore* sem = g_sem.load(std::memory_order_acquire);
    if (sem != nullptr)
    {
        sem->signal();
    }
    if (g_consumer.joinable())
    {
        g_consumer.join();
    }

    // The consumer did a final sweep before exiting, but ulog_async_is_active()
    // still routed callers here until now; catch anything that landed in the
    // ring after that sweep before we close the door.
    drain();

    g_sem.store(nullptr, std::memory_order_release);
    delete sem;
    g_ringReady.store(false, std::memory_order_release);
}

extern "C" void ulog_async_flush(void)
{
    if (!g_running.load(std::memory_order_acquire))
    {
        return;
    }

    Semaphore* sem = g_sem.load(std::memory_order_acquire);
    for (int i = 0; i < 2000; ++i)
    {
        if (sem != nullptr)
        {
            sem->signal();
        }
        // Done only when the ring is empty *and* the consumer has parked after
        // a completed drain pass -- otherwise a record can be popped (counters
        // level) while its fprintf is still in flight, and a caller that reads
        // the log right after flush() would miss the tail of it.
        bool ringEmpty = g_enqueuePos.load(std::memory_order_acquire) ==
                         g_dequeuePos.load(std::memory_order_acquire);
        if (ringEmpty && g_consumerIdle.load(std::memory_order_acquire))
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

extern "C" unsigned long ulog_async_dropped_count(void)
{
    return g_dropped.load(std::memory_order_relaxed);
}

extern "C" void ulog_async_enqueue(int level, const char* file, int line,
                                   const char* fmt, va_list args)
{
    if (!g_ringReady.load(std::memory_order_acquire))
    {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Record rec;
    rec.fmt   = fmt;
    rec.file  = file;
    rec.line  = line;
    rec.level = level;
    nowTimespec(rec.tvSec, rec.tvNsec);

    va_list ap;
    va_copy(ap, args);
    serializeArgs(fmt, ap, rec);
    va_end(ap);

    if (!ringPush(rec))
    {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Semaphore* sem = g_sem.load(std::memory_order_acquire);
    if (sem != nullptr)
    {
        sem->signal();
    }
}

// ===========================================================================
// Automatic lifecycle
// ===========================================================================

namespace {

// Brings the consumer thread up before main() and takes it down (draining the
// ring) at process exit, so applications never call ulog_async_start() /
// ulog_async_stop() themselves. Defined last in the translation unit so every
// ring global above is already constructed when the constructor runs.
struct AsyncLifecycle
{
    AsyncLifecycle()  { ulog_async_start(); }
    ~AsyncLifecycle() { ulog_async_stop(); }
};

AsyncLifecycle g_asyncLifecycle;

} // namespace
