//=========================================================================
// Name:            GenericFIFO.h
// Purpose:         Generic version of codec2_fifo that can handle any type
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

#ifndef GENERIC_FIFO_H
#define GENERIC_FIFO_H

#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <new>

#if defined(_MSC_VER)
#include <intrin.h> // _mm_pause() / __yield() in GenericFIFO::pauseCpu_()
#endif // defined(_MSC_VER)

#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_constructive_interference_size;
    using std::hardware_destructive_interference_size;
#else
    // 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │ ...
    constexpr std::size_t hardware_constructive_interference_size = 64;
    constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

template<typename T>
class GenericFIFO
{
public:
    GenericFIFO(int len, T* inBuf = nullptr);
    GenericFIFO(const GenericFIFO<T>& rhs) = delete;
    GenericFIFO(GenericFIFO<T>&& rhs) noexcept;
    virtual ~GenericFIFO() noexcept;

    int write(T* data, int len) noexcept;
    int read(T* result, int len) noexcept;

    int numUsed() const noexcept;
    int numFree() const noexcept;
    int capacity() const noexcept;

    // Empties the FIFO. Can be called from any thread, including while the
    // producer and consumer are using it: a write() or read() already in
    // progress finishes first (reset() waits for it, which takes at most one
    // copy), and one that starts while the reset is in progress fails
    // (returns -1) rather than waiting. write() and read() never block.
    void reset() noexcept;

private:
    T* buf;

    // pin/pout each pack a 32-bit ring index together with a 32-bit
    // generation number into a single atomic word. reset() bumps the
    // generation as part of the same update that rewinds both indices to 0.
    // This lets write()/read() commit with a single compare_exchange that
    // verifies *both* "the index I started from is unchanged" and "no
    // reset() happened in the meantime" atomically -- a plain pointer/index
    // CAS alone can't distinguish "nothing changed" from "a reset happened
    // and coincidentally left the index at the same value", since reset()
    // always rewinds to the same index (0). The generation half of the
    // packed value only ever increases, so that coincidence can no longer
    // fool the commit.
    using Pos = std::uint64_t;
    static Pos makePos_(std::uint32_t generation, std::uint32_t index) noexcept
    {
        return (static_cast<Pos>(generation) << 32) | static_cast<Pos>(index);
    }
    static std::uint32_t indexOf_(Pos p) noexcept { return static_cast<std::uint32_t>(p & 0xFFFFFFFFu); }
    static std::uint32_t generationOf_(Pos p) noexcept { return static_cast<std::uint32_t>(p >> 32); }

    // Returns false (and leaves `used` untouched) if `pinVal`/`poutVal` come
    // from different generations -- meaning a reset() landed between the two
    // independent loads that produced them, so any "used" count computed
    // from them would be meaningless. Callers must treat that as "reload
    // and try again", never as "0 used".
    bool computeUsed_(Pos pinVal, Pos poutVal, unsigned int& used) const noexcept
    {
        if (generationOf_(pinVal) != generationOf_(poutVal))
        {
            return false;
        }

        int in = static_cast<int>(indexOf_(pinVal));
        int out = static_cast<int>(indexOf_(poutVal));
        int diff = in - out;
        if (diff < 0)
        {
            diff += nelem;
        }
        used = static_cast<unsigned int>(diff);
        return true;
    }

#if !defined(__clang__) && defined(__cpp_lib_hardware_interference_size)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif // !defined(__clang__)
    // Each side's active flag shares a cache line with the cursor that side
    // already writes, so setting it costs no extra cache-line traffic.
    alignas(hardware_destructive_interference_size) std::atomic<Pos> pin_;
    Pos poutCache_;
    std::atomic<bool> writerActive_; // set for the duration of write()
    alignas(hardware_destructive_interference_size) std::atomic<Pos> pout_;
    Pos pinCache_;
    std::atomic<bool> readerActive_; // set for the duration of read()

    int nelem;
    bool ownBuffer_;

    // Set by reset() for as long as it's resetting; also serializes
    // concurrent reset() calls so they can't interleave their stores.
    // Kept on its own cache line to avoid false-sharing with pin_/pout_.
    //
    // A read()/write() and a reset() must never overlap: a copy in progress
    // when the ring is rewound would race with the copies after it into
    // the same slots. Each side announces itself before checking for the
    // other (read()/write() set their active flag and then check
    // resetting_; reset() sets resetting_ and then checks both flags), all
    // seq_cst, so at least one of them always sees the other: either the
    // read()/write() backs out, or reset() waits for it to finish.
    alignas(hardware_destructive_interference_size) std::atomic<bool> resetting_;
#if !defined(__clang__)
#pragma GCC diagnostic pop
#endif // !defined(__clang__) && defined(__cpp_lib_hardware_interference_size)

    bool canWrite_(int len, Pos& outStart) noexcept;
    bool canRead_(int len, Pos& outStart) noexcept;
    int writeImpl_(T* data, int len) noexcept;
    int readImpl_(T* result, int len) noexcept;

    // Marks a read() or write() as in progress. Returns false (with the flag
    // cleared again) if a reset() is in progress, in which case the caller
    // must back out without touching the ring.
    bool beginAccess_(std::atomic<bool>& active) noexcept
    {
        active.store(true, std::memory_order_seq_cst);
        if (resetting_.load(std::memory_order_seq_cst))
        {
            active.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    // Spin-wait hint for reset(). Not std::this_thread::yield(), which is a
    // system call, since reset() can be called from real-time threads.
    static void pauseCpu_() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(_M_X64) || defined(_M_IX86)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#elif defined(_M_ARM64) || defined(_M_ARM)
        __yield();
#endif
    }
};

template<typename T>
GenericFIFO<T>::GenericFIFO(int len, T* inBuf)
    : buf(inBuf)
    , pin_(makePos_(0, 0))
    , poutCache_(makePos_(0, 0))
    , writerActive_(false)
    , pout_(makePos_(0, 0))
    , pinCache_(makePos_(0, 0))
    , readerActive_(false)
    , nelem(len)
    , ownBuffer_(inBuf == nullptr ? true : false)
    , resetting_(false)
{
    if (ownBuffer_)
    {
        buf = new T[nelem];
        assert(buf != nullptr);
    }
}

template<typename T>
GenericFIFO<T>::~GenericFIFO() noexcept
{
    if (buf != nullptr && ownBuffer_)
    {
        delete[] buf;
    }
}

template<typename T>
GenericFIFO<T>::GenericFIFO(GenericFIFO<T>&& rhs) noexcept
    : buf(rhs.buf)
    , pin_(rhs.pin_.load())
    , poutCache_(rhs.poutCache_)
    , writerActive_(false)
    , pout_(rhs.pout_.load())
    , pinCache_(rhs.pinCache_)
    , readerActive_(false)
    , nelem(rhs.nelem)
    , ownBuffer_(rhs.ownBuffer_)
    , resetting_(false)
{
    rhs.buf = nullptr;
    rhs.pin_ = makePos_(0, 0);
    rhs.poutCache_ = makePos_(0, 0);
    rhs.pout_ = makePos_(0, 0);
    rhs.pinCache_ = makePos_(0, 0);
    rhs.nelem = 0;
}

template<typename T>
void GenericFIFO<T>::reset() noexcept
{
    // Serialise concurrent reset() calls. Setting resetting_ also stops any
    // new read()/write() from starting (see beginAccess_()); seq_cst pairs
    // with theirs.
    bool expected = false;
    while (!resetting_.compare_exchange_weak(
               expected, true,
               std::memory_order_seq_cst,
               std::memory_order_relaxed))
    {
        expected = false;
        pauseCpu_();
    }

    // Wait for a read()/write() already in progress to finish, so that
    // rewinding the ring can't race with its copy. Each holds its flag for
    // one bounded copy and never waits on anything, so this is short.
    while (writerActive_.load(std::memory_order_seq_cst) ||
           readerActive_.load(std::memory_order_seq_cst))
    {
        pauseCpu_();
    }

    // Bump the generation as part of rewinding both indices to 0, so that
    // the producer's and consumer's cached copies of the other's cursor
    // (pinCache_/poutCache_) are seen to be stale (see computeUsed_()).
    Pos oldPin = pin_.load(std::memory_order_acquire);
    Pos freshPos = makePos_(generationOf_(oldPin) + 1, 0);

    pin_.store(freshPos, std::memory_order_seq_cst);
    pout_.store(freshPos, std::memory_order_seq_cst);

    resetting_.store(false, std::memory_order_release);
}

template<typename T>
bool GenericFIFO<T>::canWrite_(int len, Pos& outStart) noexcept
{
    // Own cursor: only the producer writes pin_, so this load synchronises with nothing
    // and needs no ordering of its own. The acquire that matters on this side is the
    // pout_ load below.
    Pos ppin = pin_.load(std::memory_order_relaxed);
    unsigned int used = 0;

    bool ok = computeUsed_(ppin, poutCache_, used);
    if (!ok || (nelem - (int)used - 1) < len)
    {
        // Stale or insufficient -- reload poutCache_ and test again.
        poutCache_ = pout_.load(std::memory_order_acquire);
        ok = computeUsed_(ppin, poutCache_, used);
        if (!ok || (nelem - (int)used - 1) < len)
        {
            return false;
        }
    }

    outStart = ppin;
    return true;
}

template<typename T>
bool GenericFIFO<T>::canRead_(int len, Pos& outStart) noexcept
{
    // Own cursor: only the consumer writes pout_, so this load synchronises with nothing.
    // The acquire that matters on this side is the pin_ load below, which is what makes
    // the producer's buffer writes visible.
    Pos ppout = pout_.load(std::memory_order_relaxed);
    unsigned int used = 0;

    bool ok = computeUsed_(pinCache_, ppout, used);
    if (!ok || used < (unsigned)len)
    {
        // Stale or insufficient -- reload pinCache_ and test again.
        pinCache_ = pin_.load(std::memory_order_acquire);
        ok = computeUsed_(pinCache_, ppout, used);
        if (!ok || used < (unsigned)len)
        {
            return false;
        }
    }

    outStart = ppout;
    return true;
}

template<typename T>
int GenericFIFO<T>::write(T* data, int len) noexcept
{
    assert(data != NULL);

    // A negative length passes canWrite_()'s "is there room" test and then
    // becomes a huge std::copy_n count (the (uint32_t) cast in writeImpl_),
    // which reads well past the caller's buffer. Reject it instead.
    if (len < 0)
    {
        return -1;
    }

    if (!beginAccess_(writerActive_))
    {
        return -1;
    }
    int result = writeImpl_(data, len);
    writerActive_.store(false, std::memory_order_release);
    return result;
}

template<typename T>
int GenericFIFO<T>::writeImpl_(T* data, int len) noexcept
{
    Pos startPos;
    if (!canWrite_(len, startPos))
    {
        return -1;
    }

    std::uint32_t gen = generationOf_(startPos);
    std::uint32_t idx = indexOf_(startPos);
    int numToCopy = std::min((std::uint32_t)len, nelem - idx);
    std::copy_n(data, numToCopy, &buf[idx]);
    len -= numToCopy;
    idx += numToCopy;
    data += numToCopy;
    if (idx >= (std::uint32_t)nelem)
    {
        std::copy_n(data, len, buf);
        idx = len;
    }

    // Commit only if `pin_` still holds exactly the (generation, index) we
    // started from. We're the sole writer and reset() waits for write() to
    // finish (see beginAccess_()), so nothing should have moved `pin_`; the
    // generation-checked CAS just keeps a broken caller (e.g. two producers)
    // from corrupting the FIFO, abandoning the write instead.
    Pos expected = startPos;
    Pos newPos = makePos_(gen, idx);
    if (!pin_.compare_exchange_strong(expected, newPos, std::memory_order_release, std::memory_order_relaxed))
    {
        return -1;
    }

    return 0;
}

template<typename T>
int GenericFIFO<T>::read(T* result, int len) noexcept
{
    assert(result != NULL);

    // See write(): a negative length is a caller error, not a request to read
    // (canRead_() happens to reject it, but don't rely on that).
    if (len < 0)
    {
        return -1;
    }

    if (!beginAccess_(readerActive_))
    {
        return -1;
    }
    int ret = readImpl_(result, len);
    readerActive_.store(false, std::memory_order_release);
    return ret;
}

template<typename T>
int GenericFIFO<T>::readImpl_(T* result, int len) noexcept
{
    Pos startPos;
    if (!canRead_(len, startPos))
    {
        return -1;
    }

    std::uint32_t gen = generationOf_(startPos);
    std::uint32_t idx = indexOf_(startPos);
    int numToCopy = std::min((std::uint32_t)len, nelem - idx);
    std::copy_n(&buf[idx], numToCopy, result);
    result += numToCopy;
    idx += numToCopy;
    len -= numToCopy;
    if (idx >= (std::uint32_t)nelem)
    {
        std::copy_n(buf, len, result);
        idx = len;
    }

    // See write() -- commit only if `pout_` still holds exactly the
    // (generation, index) canRead_() handed us.
    Pos expected = startPos;
    Pos newPos = makePos_(gen, idx);
    if (!pout_.compare_exchange_strong(expected, newPos, std::memory_order_release, std::memory_order_relaxed))
    {
        return -1;
    }

    return 0;
}

template<typename T>
int GenericFIFO<T>::numUsed() const noexcept
{
    Pos ppin = pin_.load(std::memory_order_acquire);
    Pos ppout = pout_.load(std::memory_order_acquire);
    unsigned int used = 0;

    // If a reset() landed exactly between these two independent loads,
    // they'll disagree on generation; report the FIFO as momentarily empty
    // rather than computing nonsense from mismatched generations. This is a
    // narrow, transient window on a diagnostic/sizing method, not a
    // correctness-critical path (write()/read() use the generation-checked
    // commit above, not this method).
    if (!computeUsed_(ppin, ppout, used))
    {
        return 0;
    }

    return (int)used;
}

template<typename T>
int GenericFIFO<T>::numFree() const noexcept
{
    // available storage is one less than nshort as prd == pwr
    // is reserved for empty rather than full

    return nelem - numUsed() - 1;
}

template<typename T>
int GenericFIFO<T>::capacity() const noexcept
{
    return nelem - 1;
}

template<typename T, int StaticSize>
class PreAllocatedFIFO : public GenericFIFO<T>
{
public:
    PreAllocatedFIFO();
    virtual ~PreAllocatedFIFO() = default;

    // No move/copies possible.
    PreAllocatedFIFO(const PreAllocatedFIFO<T, StaticSize>& rhs) = delete;
    PreAllocatedFIFO(PreAllocatedFIFO<T, StaticSize>&& rhs) = delete;

private:
    T ourBuf[StaticSize];
};

template<typename T, int StaticSize>
PreAllocatedFIFO<T, StaticSize>::PreAllocatedFIFO()
    : GenericFIFO<T>(StaticSize, ourBuf)
{
    // empty
}

#endif // GENERIC_FIFO_H
