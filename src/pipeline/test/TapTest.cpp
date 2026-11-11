// Tests for TapStep: the audio passes through untouched, the tapped step
// receives the same audio (in order) on TapStep's own thread, a tapped step
// that falls behind loses whole 10 ms blocks rather than stalling the caller
// (it runs on the real-time audio path), and destruction hands over what's
// still buffered, joins the thread and frees the tapped step.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "TapStep.h"
#include "PipelineTestCommon.h"

using namespace std::chrono_literals;

namespace
{

constexpr int SAMPLE_RATE = 8000;
constexpr int BLOCK = SAMPLE_RATE / 100; // TapStep hands the tap 10 ms at a time

// Records everything the tap gives it.
class RecordingStep : public IPipelineStep
{
public:
    RecordingStep(
        std::chrono::milliseconds delayPerCall = 0ms, std::atomic<bool>* destroyed = nullptr,
        std::vector<short>* samplesAtDestruction = nullptr)
        : delayPerCall_(delayPerCall)
        , destroyed_(destroyed)
        , samplesAtDestruction_(samplesAtDestruction)
    {
    }

    virtual ~RecordingStep()
    {
        if (samplesAtDestruction_ != nullptr)
        {
            *samplesAtDestruction_ = samples_;
        }
        if (destroyed_ != nullptr)
        {
            *destroyed_ = true;
        }
    }

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return SAMPLE_RATE; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return SAMPLE_RATE; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        FREEDV_BEGIN_VERIFIED_SAFE
        if (delayPerCall_ > 0ms)
        {
            std::this_thread::sleep_for(delayPerCall_);
        }
        {
            std::unique_lock<std::mutex> lk(mutex_);
            samples_.insert(samples_.end(), inputSamples, inputSamples + numInputSamples);
            callSizes_.push_back(numInputSamples);
        }
        cv_.notify_all();
        FREEDV_END_VERIFIED_SAFE

        *numOutputSamples = numInputSamples;
        return inputSamples;
    }

    // Waits until at least `count` samples have arrived; returns a copy.
    std::vector<short> waitForSamples(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, timeout, [&]() { return samples_.size() >= count; });
        return samples_;
    }

    std::vector<int> callSizes()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return callSizes_;
    }

private:
    std::chrono::milliseconds delayPerCall_;
    std::atomic<bool>* destroyed_;
    std::vector<short>* samplesAtDestruction_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<short> samples_;
    std::vector<int> callSizes_;
};

// Sample n has the value n, so the tap's output shows exactly which input
// samples it got.
std::vector<short> makeRamp(int count)
{
    std::vector<short> ramp(count);
    for (int i = 0; i < count; i++)
    {
        ramp[i] = (short)i;
    }
    return ramp;
}

bool passThroughUnchanged()
{
    TapStep tapStep(SAMPLE_RATE, new RecordingStep());

    auto input = makeRamp(BLOCK * 3);
    int outputSamples = 0;
    short* result = tapStep.execute(input.data(), input.size(), &outputSamples);
    if (outputSamples != (int)input.size())
    {
        std::cerr << "[outputSamples " << outputSamples << " != " << input.size() << "]...";
        return false;
    }
    if (result != input.data())
    {
        std::cerr << "[output isn't the input buffer]...";
        return false;
    }
    for (size_t i = 0; i < input.size(); i++)
    {
        if (input[i] != (short)i)
        {
            std::cerr << "[input modified at " << i << "]...";
            return false;
        }
    }
    return true;
}

bool tapReceivesAudioInOrder()
{
    // One second of audio at roughly real-time pace (20 ms chunks), which the
    // tap thread easily keeps up with: it must see exactly the input.
    auto* recorder = new RecordingStep();
    TapStep tapStep(SAMPLE_RATE, recorder);

    auto input = makeRamp(SAMPLE_RATE);
    for (int pos = 0; pos < SAMPLE_RATE; pos += 2 * BLOCK)
    {
        int outputSamples = 0;
        tapStep.execute(input.data() + pos, 2 * BLOCK, &outputSamples);
        std::this_thread::sleep_for(2ms);
    }

    // The tap thread is woken for every whole block, so all of it arrives.
    const size_t expectAtLeast = SAMPLE_RATE;
    auto got = recorder->waitForSamples(expectAtLeast, 5s);
    if (got.size() != expectAtLeast)
    {
        std::cerr << "[tap got only " << got.size() << " of " << SAMPLE_RATE << " samples]...";
        return false;
    }
    for (size_t i = 0; i < got.size(); i++)
    {
        if (got[i] != (short)i)
        {
            std::cerr << "[tap sample " << i << " is " << got[i] << "]...";
            return false;
        }
    }
    for (int size : recorder->callSizes())
    {
        if (size != BLOCK)
        {
            std::cerr << "[tap called with " << size << " samples, expected " << BLOCK << "]...";
            return false;
        }
    }
    return true;
}

bool slowTapDropsWholeBlocksWithoutBlocking()
{
    // A tap 5x slower than real time, fed 2 s of audio as fast as possible:
    // TapStep's 250 ms buffer overflows. execute() must not wait for the tap
    // (it's on the audio thread), and what the tap does get must be whole,
    // in-order 10 ms blocks of the input -- never torn or reordered.
    auto* recorder = new RecordingStep(50ms);
    TapStep tapStep(SAMPLE_RATE, recorder);

    const int total = 2 * SAMPLE_RATE;
    auto input = makeRamp(total);
    auto start = std::chrono::steady_clock::now();
    for (int pos = 0; pos < total; pos += BLOCK)
    {
        int outputSamples = 0;
        tapStep.execute(input.data() + pos, BLOCK, &outputSamples);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > 200ms)
    {
        std::cerr << "[execute() took "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms for 2 s of audio; it must not wait for the tap]...";
        return false;
    }

    // Let the tap work through what it buffered (at most 250 ms of audio).
    std::this_thread::sleep_for(2s);
    auto got = recorder->waitForSamples(0, 0ms);
    if (got.empty() || got.size() >= (size_t)total || got.size() % BLOCK != 0)
    {
        std::cerr << "[tap got " << got.size() << " samples; expected some, but fewer than "
                  << total << ", in whole blocks]...";
        return false;
    }

    int lastBlock = -1;
    for (size_t b = 0; b < got.size() / BLOCK; b++)
    {
        int first = got[b * BLOCK];
        if (first % BLOCK != 0 || first / BLOCK <= lastBlock)
        {
            std::cerr << "[tap block " << b << " starts at sample " << first << "]...";
            return false;
        }
        for (int i = 0; i < BLOCK; i++)
        {
            if (got[b * BLOCK + i] != first + i)
            {
                std::cerr << "[tap block " << b << " is torn]...";
                return false;
            }
        }
        lastBlock = first / BLOCK;
    }
    return true;
}

// Feeds `count` samples to a TapStep in one go and destroys it straight away.
// Returns what the tap had been given by the time it was itself destroyed.
std::vector<short> tapOutputForBurst(int count, std::chrono::milliseconds delayPerCall = 0ms)
{
    std::vector<short> got;
    {
        TapStep tapStep(SAMPLE_RATE, new RecordingStep(delayPerCall, nullptr, &got));
        auto input = makeRamp(count);
        int outputSamples = 0;
        tapStep.execute(input.data(), input.size(), &outputSamples);
    }
    return got;
}

bool isRampOfLength(std::vector<short> const& got, int expected)
{
    if (got.size() != (size_t)expected)
    {
        std::cerr << "[tap got " << got.size() << " samples, expected " << expected << "]...";
        return false;
    }
    for (size_t i = 0; i < got.size(); i++)
    {
        if (got[i] != (short)i)
        {
            std::cerr << "[tap sample " << i << " is " << got[i] << "]...";
            return false;
        }
    }
    return true;
}

bool burstThatFitsInBufferIsNotLost()
{
    // Audio doesn't always arrive at a steady pace (e.g. after the caller has
    // itself been held up). Whatever fits in TapStep's 250 ms buffer must
    // reach the tap however late the tap thread gets to run -- here, possibly
    // not until TapStep is destroyed -- even if the tap is slow.
    const int burst = 24 * BLOCK; // 240 ms
    return
        isRampOfLength(tapOutputForBurst(burst), burst) &&
        isRampOfLength(tapOutputForBurst(burst, 2ms), burst);
}

bool destructionHandsOverWholeBlocksOnly()
{
    // The tap is only ever given whole 10 ms blocks, so a partial block left
    // over at destruction isn't handed over.
    return
        isRampOfLength(tapOutputForBurst(BLOCK), BLOCK) &&
        isRampOfLength(tapOutputForBurst(3 * BLOCK + BLOCK / 2), 3 * BLOCK) &&
        isRampOfLength(tapOutputForBurst(BLOCK / 2), 0);
}

bool destructionJoinsAndFreesTap()
{
    // Destroying a TapStep with audio still buffered (and its thread possibly
    // mid-call) must stop the thread and delete the tapped step.
    std::atomic<bool> destroyed(false);
    {
        TapStep tapStep(SAMPLE_RATE, new RecordingStep(5ms, &destroyed));
        auto input = makeRamp(SAMPLE_RATE / 5);
        int outputSamples = 0;
        tapStep.execute(input.data(), input.size(), &outputSamples);
    }
    if (!destroyed)
    {
        std::cerr << "[tapped step not deleted]...";
        return false;
    }
    return true;
}

bool destructionWhileDroppingReportsDrops()
{
    // Two overfull bursts against a fast tap, separated by a pause long
    // enough for the tap thread to catch up and emit its report pair: the
    // first report notes that the tap has started dropping (dropping_ goes
    // true) and the catch-up report notes that it ended. TapStep is then
    // destroyed mid-backlog, so ~TapStep logs the lost audio instead of
    // silently swallowing it.
    std::atomic<bool> destroyed(false);
    {
        TapStep tapStep(SAMPLE_RATE, new RecordingStep(1ms, &destroyed));
        auto input = makeRamp(27 * BLOCK);
        int outputSamples = 0;
        int outputSamples2 = 0;
        tapStep.execute(input.data(), input.size(), &outputSamples);
        // 27 blocks drain from the 250 ms buffer in ~24 ms at 1 block/ms;
        // 300 ms is a wide margin for the tap thread to catch up and emit
        // its first drop report.
        std::this_thread::sleep_for(300ms);
        tapStep.execute(input.data(), input.size(), &outputSamples2);
        if (outputSamples != 27 * BLOCK)
        {
            std::cerr << "[outputSamples " << outputSamples << " != " << 27 * BLOCK << "]...";
            return false;
        }
        if (outputSamples2 != 27 * BLOCK)
        {
            std::cerr << "[outputSamples2 " << outputSamples2 << " != " << 27 * BLOCK << "]...";
            return false;
        }
    }
    if (!destroyed)
    {
        std::cerr << "[tapped step not deleted]...";
        return false;
    }
    return true;
}

bool dropStatePersistsAcrossConsecutiveOverflowReports()
{
    // A 50 ms/block slow tap: execute() #1 overflows (the ring fits
    // nelem-1 = 1999 samples = 24 whole blocks, so 3 of 27 are dropped), and
    // while the tap is still chewing through the 24-block backlog its first
    // drop report fires (dropping_ goes true). Only then does execute() #2
    // overflow, so its drop is reported while TapStep is *already* dropping
    // -- the second operand-false side of `if (!dropping_)` (the
    // drop-continuing report). execute() #3 then feeds a clean block: its
    // report has no drops while dropping_ is still true, so the drop state
    // ends.
    auto* recorder = new RecordingStep(50ms);
    TapStep tapStep(SAMPLE_RATE, recorder);
    auto input = makeRamp(27 * BLOCK);
    int out1 = 0, out2 = 0, out3 = 0;

    tapStep.execute(input.data(), input.size(), &out1);   // 24 buffered, 3 dropped
    // The report only runs after the whole backlog drains (24 blocks x 50ms);
    // 24 delivered blocks means the first report has fired (or fires within
    // microseconds) and the tap thread is parked in sem_.wait() -- no drops==0
    // report can interleave before execute() #2 wakes it again.
    auto got = recorder->waitForSamples(24 * BLOCK, 30s);
    if (got.size() != (size_t)24 * BLOCK)
    {
        std::cerr << "[expected exactly 24 blocks before second overflow, got "
                  << got.size() / BLOCK << "]...";
        return false;
    }
    std::this_thread::sleep_for(100ms); // margin past the first drop report

    tapStep.execute(input.data(), input.size(), &out2);   // overflow #2 while dropping_==true
    // execute() #3's block only fits once the tap has drained at least one
    // block of the second backlog (free space is 1999 - 24*80 = 79 samples
    // until then), so gate on 25 delivered blocks before feeding it.
    got = recorder->waitForSamples(25 * BLOCK, 30s);
    if (got.size() != (size_t)25 * BLOCK)
    {
        std::cerr << "[tap did not start draining the second backlog, got "
                  << got.size() / BLOCK << "]...";
        return false;
    }
    tapStep.execute(input.data(), BLOCK, &out3);          // clean wake-up -> drop state ends
    if (out1 != 27 * BLOCK || out2 != 27 * BLOCK || out3 != BLOCK)
    {
        std::cerr << "[pass-through broken: " << out1 << "/" << out2 << "/" << out3 << "]...";
        return false;
    }

    // 55 blocks fed, 6 dropped (3 per overflowing execute): the tap must end
    // with exactly 49 whole blocks, proving both overflow reports and the
    // terminating no-drop report ran in order.
    got = recorder->waitForSamples(49 * BLOCK, 30s);
    if (got.size() != (size_t)49 * BLOCK)
    {
        std::cerr << "[tap got " << got.size() / BLOCK << " blocks, expected 49]...";
        return false;
    }
    for (int size : recorder->callSizes())
    {
        if (size != BLOCK)
        {
            std::cerr << "[tap called with " << size << " samples, expected " << BLOCK << "]...";
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    TEST_CASE(passThroughUnchanged);
    TEST_CASE(tapReceivesAudioInOrder);
    TEST_CASE(slowTapDropsWholeBlocksWithoutBlocking);
    TEST_CASE(burstThatFitsInBufferIsNotLost);
    TEST_CASE(destructionHandsOverWholeBlocksOnly);
    TEST_CASE(destructionJoinsAndFreesTap);
    TEST_CASE(destructionWhileDroppingReportsDrops);
    TEST_CASE(dropStatePersistsAcrossConsecutiveOverflowReports);
}
