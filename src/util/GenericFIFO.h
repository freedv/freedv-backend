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
    alignas(hardware_destructive_interference_size) std::atomic<Pos> pin_;
    Pos poutCache_;
    alignas(hardware_destructive_interference_size) std::atomic<Pos> pout_;
    Pos pinCache_;

    int nelem;
    bool ownBuffer_;

    // Serializes concurrent reset() calls so two threads calling reset()
    // simultaneously can't interleave their independent stores.
    // Kept on its own cache line to avoid false-sharing with pin_/pout_.
    alignas(hardware_destructive_interference_size) std::atomic<bool> resetting_;
#if !defined(__clang__)
#pragma GCC diagnostic pop
#endif // !defined(__clang__) && defined(__cpp_lib_hardware_interference_size)

    bool canWrite_(int len, Pos& outStart) noexcept;
    bool canRead_(int len, Pos& outStart) noexcept;
};

template<typename T>
GenericFIFO<T>::GenericFIFO(int len, T* inBuf)
    : buf(inBuf)
    , pin_(makePos_(0, 0))
    , poutCache_(makePos_(0, 0))
    , pout_(makePos_(0, 0))
    , pinCache_(makePos_(0, 0))
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
    , pout_(rhs.pout_.load())
    , pinCache_(rhs.pinCache_)
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
    // Serialise concurrent reset() calls.
    bool expected = false;
    while (!resetting_.compare_exchange_weak(
               expected, true,
               std::memory_order_acquire,
               std::memory_order_relaxed))
    {
        expected = false;
    }

    // Bump the generation as part of rewinding both indices to 0. Any
    // writer/reader whose in-flight write()/read() started before this
    // point will have its commit CAS fail (see write()/read()), since its
    // captured Pos can never match a Pos from a newer generation.
    Pos oldPin = pin_.load(std::memory_order_acquire);
    Pos freshPos = makePos_(generationOf_(oldPin) + 1, 0);

    pin_.store(freshPos, std::memory_order_seq_cst);
    pout_.store(freshPos, std::memory_order_seq_cst);

    resetting_.store(false, std::memory_order_release);
}

template<typename T>
bool GenericFIFO<T>::canWrite_(int len, Pos& outStart) noexcept
{
    if (resetting_.load(std::memory_order_acquire)) return false;

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
    if (resetting_.load(std::memory_order_acquire)) return false;

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
    // started from. In normal SPSC operation we're the sole writer, so the
    // only thing that can move `pin_` out from under us is a concurrent
    // reset(), which always changes the generation -- so this can never be
    // fooled by a coincidental index match the way a bare-pointer CAS could.
    // If it fails, abandon this write rather than clobbering the freshly
    // reset FIFO, consistent with reset()'s "a lost write beats a corrupted
    // FIFO" policy.
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
