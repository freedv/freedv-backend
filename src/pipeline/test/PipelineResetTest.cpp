// Checks that reset() returns pipeline steps to their freshly constructed
// state: a step that has processed audio and then been reset must produce
// bit-identical output to a new step fed the same input. Any internal state
// that reset() forgets (filter history, gain, buffered samples, ...) shows up
// as a mismatch.
//
// AgcStep is the exception: its gain and loudness history deliberately
// survive a reset (see AgcStep::reset() and AgcStepTest), so it's only
// checked for dropping buffered input.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

#include "AgcStep.h"
#include "AudioPipeline.h"
#include "BandwidthExpandStep.h"
#include "EitherOrStep.h"
#include "LevelAdjustStep.h"
#include "ResampleStep.h"
#include "RNNoiseStep.h"
#include "PipelineTestCommon.h"

namespace {

// Deterministic test signal: a tone plus a pseudo-random component, so
// stateful steps (AGC, noise suppression, resampler filters) have plenty of
// history to carry over if reset() misses something.
std::vector<short> makeSignal(int sampleRate, float seconds, float toneHz, float amplitude, unsigned seed)
{
    std::vector<short> result((size_t)(sampleRate * seconds));
    unsigned state = seed;
    for (size_t n = 0; n < result.size(); n++)
    {
        state = state * 1664525u + 1013904223u;
        float noise = ((float)(state >> 16) / 65535.0f - 0.5f) * 0.2f;
        float value = amplitude * (sinf(6.2831853f * toneHz * n / sampleRate) + noise);
        result[n] = (short)std::max(-32767.0f, std::min(32767.0f, value));
    }
    return result;
}

// Feeds the signal through the step in 10 ms chunks, the way the real-time
// pipeline does, and returns everything the step produced.
std::vector<short> run(IPipelineStep& step, const std::vector<short>& input)
{
    std::vector<short> output;
    int chunk = step.getInputSampleRate() / 100;
    std::vector<short> buf(chunk);
    for (size_t offset = 0; offset + chunk <= input.size(); offset += chunk)
    {
        std::copy(input.begin() + offset, input.begin() + offset + chunk, buf.begin());
        int numOut = 0;
        short* out = step.execute(buf.data(), chunk, &numOut);
        output.insert(output.end(), out, out + numOut);
    }
    return output;
}

// Returns true if reset() makes `factory()`'s step behave like a fresh one.
bool resetMatchesFresh(std::function<IPipelineStep*()> const& factory)
{
    std::unique_ptr<IPipelineStep> fresh(factory());
    std::unique_ptr<IPipelineStep> used(factory());

    int sampleRate = fresh->getInputSampleRate();
    // A: loud, long enough to fill every internal window (e.g. ebur128's
    // 400 ms momentary loudness window and the AGC's gain ramp).
    auto before = makeSignal(sampleRate, 2.0f, 440.0f, 20000.0f, 1);
    // B: much quieter, different tone, so leftover state from A changes the
    // result.
    auto after = makeSignal(sampleRate, 1.0f, 1000.0f, 1000.0f, 2);

    auto expected = run(*fresh, after);

    run(*used, before);
    used->reset();
    auto actual = run(*used, after);

    if (expected.size() != actual.size())
    {
        std::cerr << "[sample count " << actual.size() << " != " << expected.size() << "]...";
        return false;
    }
    size_t numDiffering = 0;
    size_t firstDiff = 0;
    size_t lastDiff = 0;
    int maxDiff = 0;
    for (size_t i = 0; i < expected.size(); i++)
    {
        int diff = std::abs((int)expected[i] - (int)actual[i]);
        if (diff != 0)
        {
            if (numDiffering == 0) firstDiff = i;
            lastDiff = i;
            numDiffering++;
            maxDiff = std::max(maxDiff, diff);
        }
    }
    if (numDiffering > 0)
    {
        std::cerr << "[" << numDiffering << " of " << expected.size() << " samples differ from a fresh step"
                  << " (samples " << firstDiff << "-" << lastDiff << ", max difference " << maxDiff << ")]...";
        return false;
    }
    return true;
}

bool resampleUpReset()
{
    return resetMatchesFresh([]() { return new ResampleStep(8000, 48000); });
}

bool resampleDownReset()
{
    return resetMatchesFresh([]() { return new ResampleStep(48000, 8000); });
}

bool agcResetDiscardsBufferedInput()
{
    // Less than a block of loud audio is left buffered, then reset(): the
    // next block out must be made only of what's fed afterwards. With
    // silence fed, anything nonzero is leftover audio.
    for (int sampleRate : {8000, 48000})
    {
        AgcStep step(sampleRate);
        int block = std::min(160, sampleRate / 100); // the AGC's block size

        auto before = makeSignal(sampleRate, 1.0f, 440.0f, 20000.0f, 1);
        run(step, before);
        int numOut = 0;
        step.execute(before.data(), block / 2, &numOut); // stays buffered
        if (numOut != 0)
        {
            std::cerr << "[half a block produced " << numOut << " samples]...";
            return false;
        }

        step.reset();

        std::vector<short> silence(block, 0);
        short* out = step.execute(silence.data(), block, &numOut);
        if (numOut != block)
        {
            std::cerr << "[one block in gave " << numOut << " samples out at " << sampleRate << " Hz]...";
            return false;
        }
        for (int i = 0; i < numOut; i++)
        {
            if (out[i] != 0)
            {
                std::cerr << "[audio from before the reset came out at " << sampleRate << " Hz (sample " << i
                          << " = " << out[i] << ")]...";
                return false;
            }
        }
    }
    return true;
}

bool rnnoiseReset()
{
    return resetMatchesFresh([]() { return new RNNoiseStep(); });
}

bool bandwidthExpandReset()
{
    return resetMatchesFresh([]() { return new BandwidthExpandStep(); });
}

bool eitherOrReset()
{
    // Both branches are stateful; reset() must reach the inactive one too,
    // so run each configuration.
    bool result = true;
    for (bool which : {true, false})
    {
        static bool condition;
        condition = which;
        result &= resetMatchesFresh([]() {
            return new EitherOrStep(
                +[]() FREEDV_NONBLOCKING { return condition; },
                new ResampleStep(48000, 48000),
                new RNNoiseStep());
        });
    }

    // Rates are taken from the branches, which must agree.
    EitherOrStep step(+[]() FREEDV_NONBLOCKING { return true; }, new RNNoiseStep(), new LevelAdjustStep(48000, +[]() FREEDV_NONBLOCKING { return 1.0f; }));
    result &= step.getInputSampleRate() == 48000;
    result &= step.getOutputSampleRate() == 48000;
    return result;
}

bool audioPipelineReset()
{
    // A whole pipeline with resamplers between steps and on the result:
    // 8 kHz in -> 16 kHz level adjust -> 16->48 kHz bandwidth expansion ->
    // 48 kHz noise suppression -> 8 kHz out. AudioPipeline::reset() must
    // reach the steps, the inserted resamplers and the result resampler.
    // (No AgcStep here: its gain deliberately survives a reset.)
    return resetMatchesFresh([]() {
        auto pipeline = new AudioPipeline(8000, 8000);
        pipeline->appendPipelineStep(new LevelAdjustStep(16000, +[]() FREEDV_NONBLOCKING { return 0.5f; }));
        pipeline->appendPipelineStep(new BandwidthExpandStep());
        pipeline->appendPipelineStep(new RNNoiseStep());
        return pipeline;
    });
}

} // namespace

int main()
{
    // Unlike TEST_CASE, keep going after a failure so one run reports every
    // step whose reset() is incomplete.
    bool result = true;
    auto check = [&](const char* name, bool (*fn)()) {
        std::cout << "Executing " << name << "...";
        bool ok = fn();
        std::cout << (ok ? "passed" : "FAILED") << std::endl;
        result &= ok;
    };
#define RUN(name) check(#name, name)
    RUN(resampleUpReset);
    RUN(resampleDownReset);
    RUN(agcResetDiscardsBufferedInput);
    RUN(rnnoiseReset);
    RUN(bandwidthExpandReset);
    RUN(eitherOrReset);
    RUN(audioPipelineReset);
#undef RUN
    return result ? 0 : -1;
}
