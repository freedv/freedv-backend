#include <algorithm>
#include <cmath>
#include <vector>

#include "AgcStep.h"
#include "PipelineTestCommon.h"
#include "ebur128.h" // from libebur128

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

namespace {

// The AGC is documented to converge toward -23 LUFS; verified independently
// here rather than pulled from AgcStep.cpp's private constant so that these
// tests catch accidental drift of the publicly documented target.
constexpr double AGC_TARGET_LUFS = -23.0;
constexpr double TEST_TONE_FREQ_HZ = 400.0;

std::vector<short> generateSineWave(double amplitude, double freqHz, double durationSec, int sampleRate)
{
    int numSamples = static_cast<int>(durationSec * sampleRate);
    std::vector<short> result(numSamples);
    for (int n = 0; n < numSamples; n++)
    {
        result[n] = static_cast<short>(amplitude * std::cos(2.0 * M_PI * freqHz * n / sampleRate));
    }
    return result;
}

// Measures momentary loudness (last 400ms, per BS.1770) of a block of 16-bit
// mono samples -- the same measure AgcStep itself uses internally.
double measureLoudnessLufs(const short* samples, int numSamples, int sampleRate)
{
    ebur128_state* state = ebur128_init(1, sampleRate, EBUR128_MODE_M);
    assert(state != nullptr);

    ebur128_add_frames_short(state, samples, numSamples);

    double lufs = -HUGE_VAL;
    ebur128_loudness_momentary(state, &lufs);

    ebur128_destroy(&state);
    return lufs;
}

// Binary-searches (in log-amplitude space) for the sine wave amplitude that
// produces the requested loudness, so tests don't rely on hand-derived
// dBFS-to-LUFS conversions that could drift from libebur128's actual
// K-weighting behavior.
double findAmplitudeForLoudness(double targetLufs, double freqHz, int sampleRate)
{
    // 32767 = 0 dB. Solve for targetLufs = 20*log10(amp / 32767.0).
    // 10^(targetLufs / 20) * 32767.0 = amp
    // Note: this may not be the actual measured loudness depending on frequency
    double levelDb = targetLufs;

    // Generate sine wave, make sure the measured LUFS is the same as target.
    // If not, move the level by the error and try again. (Stop as soon as
    // it's on target: adjusting once more would move it off again.)
    for (int iter = 0; iter < 20; iter++)
    {
        double amplitude = 32767.0 * std::pow(10, levelDb / 20.0);
        auto probe = generateSineWave(amplitude, freqHz, 1.0, sampleRate);
        double measuredLufs = measureLoudnessLufs(probe.data(), probe.size(), sampleRate);
        if (std::abs(measuredLufs - targetLufs) < 0.1)
        {
            break;
        }
        levelDb += targetLufs - measuredLufs;
    }
    return 32767.0 * std::pow(10, levelDb / 20.0);
}

// Streams the given signal through the AGC step in small chunks, mimicking
// real-time usage, and returns the concatenated output.
std::vector<short> runThroughAgc(AgcStep& step, std::vector<short>& input, int chunkSize)
{
    std::vector<short> output;
    output.reserve(input.size());

    for (std::size_t offset = 0; offset < input.size(); offset += chunkSize)
    {
        int numToWrite = static_cast<int>(std::min<std::size_t>(chunkSize, input.size() - offset));
        int numOutputSamples = 0;
        short* result = step.execute(&input[offset], numToWrite, &numOutputSamples);
        output.insert(output.end(), result, result + numOutputSamples);
    }

    return output;
}

} // namespace

// A signal that's well louder than -23 LUFS should have its gain pulled down
// until it settles at -23 LUFS (attack path, ~0.5s time constant).
bool agcConvergesLoudSignalToTargetLoudness()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 2.0;

    AgcStep step(sampleRate);

    double amplitude = findAmplitudeForLoudness(-11.0, TEST_TONE_FREQ_HZ, sampleRate);
    auto loudSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 15.0, sampleRate);
    auto output = runThroughAgc(step, loudSignal, sampleRate / 10);

    double outputLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (std::abs(outputLufs - AGC_TARGET_LUFS) > TOLERANCE_DB)
    {
        std::cerr << "[loud signal settled at " << outputLufs << " LUFS, expected "
                   << AGC_TARGET_LUFS << " +/- " << TOLERANCE_DB << "]...";
        return false;
    }

    return true;
}

// A signal that's quieter than -23 LUFS (but still above the silence gate)
// should have its gain raised until it settles at -23 LUFS (release path,
// ~6s time constant, so needs a longer run to converge).
bool agcConvergesQuietSignalToTargetLoudness()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 2.0;

    AgcStep step(sampleRate);

    double amplitude = findAmplitudeForLoudness(-30.0, TEST_TONE_FREQ_HZ, sampleRate);
    auto quietSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 24.0, sampleRate);
    auto output = runThroughAgc(step, quietSignal, sampleRate / 10);

    double outputLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (std::abs(outputLufs - AGC_TARGET_LUFS) > TOLERANCE_DB)
    {
        std::cerr << "[quiet signal settled at " << outputLufs << " LUFS, expected "
                   << AGC_TARGET_LUFS << " +/- " << TOLERANCE_DB << "]...";
        return false;
    }

    return true;
}

// Signals near/below the AGC's silence gate shouldn't be dragged up toward
// -23 LUFS -- otherwise room tone / background noise would get amplified.
bool agcDoesNotBoostNearSilentSignal()
{
    constexpr int sampleRate = 8000;
    constexpr double RAW_LUFS = -40.0; // below the AGC's -33 LUFS silence threshold
    constexpr double TOLERANCE_DB = 2.0;

    AgcStep step(sampleRate);

    double amplitude = findAmplitudeForLoudness(RAW_LUFS, TEST_TONE_FREQ_HZ, sampleRate);
    auto quietSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 5.0, sampleRate);
    auto referenceQuietSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 5.0, sampleRate);
    auto output = runThroughAgc(step, quietSignal, sampleRate / 10);

    double referenceOutputLufs = measureLoudnessLufs(&referenceQuietSignal[0], sampleRate * 5.0, sampleRate);
    double outputLufs = measureLoudnessLufs(&output[0], sampleRate * 5.0, sampleRate);
    if (std::abs(outputLufs - referenceOutputLufs) > TOLERANCE_DB)
    {
        std::cerr << "[near-silent signal was altered: raw=" << referenceOutputLufs << " output=" << outputLufs << "]...";
        return false;
    }

    if (std::abs(outputLufs - AGC_TARGET_LUFS) < 5.0)
    {
        std::cerr << "[near-silent signal was incorrectly pulled toward target: output=" << outputLufs << "]...";
        return false;
    }

    return true;
}

// reset() should not touch the gain.
bool agcResetDoesNotReturnGainToUnity()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 2.0;

    AgcStep step(sampleRate);

    double amplitude = findAmplitudeForLoudness(-10.0, TEST_TONE_FREQ_HZ, sampleRate);
    auto loudSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 15.0, sampleRate);
    auto output = runThroughAgc(step, loudSignal, sampleRate / 10);

    // Sanity check: confirm gain actually drifted away from unity before we
    // rely on reset() to bring it back.
    double convergedLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (std::abs(convergedLufs - AGC_TARGET_LUFS) > TOLERANCE_DB)
    {
        std::cerr << "[test setup invalid: gain never drifted away from unity, settled at "
                   << convergedLufs << " LUFS]...";
        return false;
    }

    step.reset();

    // Immediately after reset(), the first block processed should be close to the previous
    // measurement.
    output = runThroughAgc(step, loudSignal, sampleRate / 10);
    double newConvergedLufs = measureLoudnessLufs(&output[0], sampleRate, sampleRate);
    if (std::abs(convergedLufs - newConvergedLufs) > TOLERANCE_DB)
    {
        std::cerr << "[gain significantly different post-reset: expected = " << convergedLufs << ", actual = " << newConvergedLufs << "]...";
        return false;
    }

    return true;
}

bool agcLimitsCutToTwelveDb()
{
    // A full-scale 3 kHz square wave measures well above 0 LUFS (K-weighting
    // boosts that range), so reaching -23 LUFS would take more than the
    // AGC's maximum 12 dB of cut. It must stop at 12 dB.
    constexpr int sampleRate = 48000;
    constexpr double MAX_CUT_DB = 12.0;
    constexpr double TOLERANCE_DB = 1.5;

    // Leveler only: the limiter would pull a square wave this loud down
    // further, and this is about where the leveler stops.
    AgcStep step(sampleRate, false, true);

    // The gain moves at 1 dB/s, so allow well over 12 s to get there.
    std::vector<short> loudSignal(20 * sampleRate);
    for (size_t n = 0; n < loudSignal.size(); n++)
    {
        loudSignal[n] = (n / 8) % 2 ? 32767 : -32767; // 3 kHz
    }
    double inputLufs = measureLoudnessLufs(loudSignal.data(), sampleRate, sampleRate);
    if (inputLufs - MAX_CUT_DB < AGC_TARGET_LUFS + 3.0)
    {
        std::cerr << "[test signal too quiet to need more than 12 dB of cut: " << inputLufs << " LUFS]...";
        return false;
    }

    auto output = runThroughAgc(step, loudSignal, sampleRate / 50);
    double outputLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (std::abs(outputLufs - (inputLufs - MAX_CUT_DB)) > TOLERANCE_DB)
    {
        std::cerr << "[input " << inputLufs << " LUFS settled at " << outputLufs << " LUFS, expected "
                  << (inputLufs - MAX_CUT_DB) << " (12 dB of cut)]...";
        return false;
    }

    return true;
}

bool agcUnsupportedRateRunsAt48k()
{
    // 44.1 kHz isn't supported, so the AGC runs at 48 kHz instead. The
    // pipeline then sends it 48 kHz audio, up to a second at a time.
    AgcStep step(44100);
    if (step.getInputSampleRate() != 48000 || step.getOutputSampleRate() != 48000)
    {
        std::cerr << "[runs at " << step.getInputSampleRate() << " Hz, expected 48000]...";
        return false;
    }

    double amplitude = findAmplitudeForLoudness(AGC_TARGET_LUFS, TEST_TONE_FREQ_HZ, 48000);
    auto input = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 1.0, 48000);
    auto output = runThroughAgc(step, input, input.size());
    if (output.size() != input.size())
    {
        std::cerr << "[got " << output.size() << " samples back from " << input.size() << "]...";
        return false;
    }

    double outputLufs = measureLoudnessLufs(&output[output.size() - 24000], 24000, 48000);
    if (std::abs(outputLufs - AGC_TARGET_LUFS) > 2.0)
    {
        std::cerr << "[output at " << outputLufs << " LUFS, expected about " << AGC_TARGET_LUFS << "]...";
        return false;
    }

    return true;
}

int main()
{
    TEST_CASE(agcConvergesLoudSignalToTargetLoudness);
    TEST_CASE(agcConvergesQuietSignalToTargetLoudness);
    TEST_CASE(agcDoesNotBoostNearSilentSignal);
    TEST_CASE(agcResetDoesNotReturnGainToUnity);
    TEST_CASE(agcLimitsCutToTwelveDb);
    TEST_CASE(agcUnsupportedRateRunsAt48k);
    return 0;
}
