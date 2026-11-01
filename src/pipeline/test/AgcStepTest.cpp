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
    double lo = 1.0;
    double hi = 32767.0;

    for (int iter = 0; iter < 40; iter++)
    {
        double mid = std::sqrt(lo * hi);
        auto probe = generateSineWave(mid, freqHz, 1.0, sampleRate);
        double lufs = measureLoudnessLufs(probe.data(), probe.size(), sampleRate);

        if (lufs < targetLufs)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }

    return std::sqrt(lo * hi);
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

    double amplitude = findAmplitudeForLoudness(-10.0, TEST_TONE_FREQ_HZ, sampleRate);
    auto loudSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 6.0, sampleRate);
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
    auto output = runThroughAgc(step, quietSignal, sampleRate / 10);

    double outputLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (std::abs(outputLufs - RAW_LUFS) > TOLERANCE_DB)
    {
        std::cerr << "[near-silent signal was altered: raw=" << RAW_LUFS << " output=" << outputLufs << "]...";
        return false;
    }

    if (std::abs(outputLufs - AGC_TARGET_LUFS) < 5.0)
    {
        std::cerr << "[near-silent signal was incorrectly pulled toward target: output=" << outputLufs << "]...";
        return false;
    }

    return true;
}

// reset() should immediately return the gain to unity (0dB) rather than
// continuing from wherever it had drifted to.
bool agcResetReturnsGainToUnity()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 2.0;

    AgcStep step(sampleRate);

    double amplitude = findAmplitudeForLoudness(-10.0, TEST_TONE_FREQ_HZ, sampleRate);
    auto loudSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 6.0, sampleRate);
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

    // Immediately after reset(), the first block processed should be only a
    // hair away from unity gain, regardless of how far gain had drifted.
    int chunkSize = sampleRate / 100; // one 10ms AGC block
    int numOutputSamples = 0;
    short* result = step.execute(loudSignal.data(), chunkSize, &numOutputSamples);

    if (numOutputSamples != chunkSize)
    {
        std::cerr << "[numOutputSamples[" << numOutputSamples << "] != " << chunkSize << "]...";
        return false;
    }

    double inputRms = 0.0;
    double outputRms = 0.0;
    for (int i = 0; i < numOutputSamples; i++)
    {
        inputRms += static_cast<double>(loudSignal[i]) * loudSignal[i];
        outputRms += static_cast<double>(result[i]) * result[i];
    }
    inputRms = std::sqrt(inputRms / numOutputSamples);
    outputRms = std::sqrt(outputRms / numOutputSamples);

    // Note: the WebRTC limiter stage adds a small amount of its own gain
    // adjustment on top of AgcStep's own dB tracking, so this won't be
    // exactly 0dB -- but it should be nowhere near the ~-13dB gain that had
    // been applied just before reset() was called.
    double gainDb = 20.0 * std::log10(outputRms / inputRms);
    if (std::abs(gainDb) > 4.0)
    {
        std::cerr << "[gain right after reset() was " << gainDb << " dB, expected close to 0 dB]...";
        return false;
    }

    return true;
}

bool agcLimitsCutToTwentyDb()
{
    // A full-scale 3 kHz square wave measures well above 0 LUFS (K-weighting
    // boosts that range), so reaching -23 LUFS would take more than the
    // AGC's maximum 20 dB of cut. It must stop at 20 dB.
    constexpr int sampleRate = 48000;
    constexpr double MAX_CUT_DB = 20.0;
    constexpr double TOLERANCE_DB = 1.5;

    AgcStep step(sampleRate);

    std::vector<short> loudSignal(6 * sampleRate);
    for (size_t n = 0; n < loudSignal.size(); n++)
    {
        loudSignal[n] = (n / 8) % 2 ? 32767 : -32767; // 3 kHz
    }
    double inputLufs = measureLoudnessLufs(loudSignal.data(), sampleRate, sampleRate);
    if (inputLufs - MAX_CUT_DB < AGC_TARGET_LUFS + 3.0)
    {
        std::cerr << "[test signal too quiet to need more than 20 dB of cut: " << inputLufs << " LUFS]...";
        return false;
    }

    auto output = runThroughAgc(step, loudSignal, sampleRate / 50);
    double outputLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (std::abs(outputLufs - (inputLufs - MAX_CUT_DB)) > TOLERANCE_DB)
    {
        std::cerr << "[input " << inputLufs << " LUFS settled at " << outputLufs << " LUFS, expected "
                  << (inputLufs - MAX_CUT_DB) << " (20 dB of cut)]...";
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

// 16 kHz and 32 kHz are natively supported rates: the AGC must keep them
// (no 48 kHz fallback) and still regulate toward -23 LUFS at those rates.
bool agcNativeRates16kAnd32k()
{
    const int rates[] = { 16000, 32000 };
    for (int rate : rates)
    {
        AgcStep step(rate);
        if (step.getInputSampleRate() != rate || step.getOutputSampleRate() != rate)
        {
            std::cerr << "[" << rate << " Hz AGC runs at " << step.getInputSampleRate() << " Hz]...";
            return false;
        }

        double amplitude = findAmplitudeForLoudness(AGC_TARGET_LUFS, TEST_TONE_FREQ_HZ, rate);
        auto input = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 1.0, rate);
        auto output = runThroughAgc(step, input, rate / 100);
        if ((int)output.size() != rate)
        {
            std::cerr << "[" << output.size() << " samples back from " << rate << " at " << rate << " Hz]...";
            return false;
        }

        double outputLufs = measureLoudnessLufs(&output[output.size() - rate / 2], rate / 2, rate);
        if (std::abs(outputLufs - AGC_TARGET_LUFS) > 3.0)
        {
            std::cerr << "[" << rate << " Hz output at " << outputLufs << " LUFS, expected about " << AGC_TARGET_LUFS << "]...";
            return false;
        }
    }
    return true;
}

// Pure silence: every momentary measurement comes back as -HUGE_VAL, so the
// measurement-validity chain must reject it at the "not -HUGE_VAL" step and
// the gain stays gated (silence in, silence out). Also verifies execute()
// with zero input samples is a no-op.
bool agcSilenceMeasurementsAreRejected()
{
    constexpr int sampleRate = 48000;

    AgcStep step(sampleRate);

    std::vector<short> oneSample(1, 0);
    int zeroInputOutput = -1;
    step.execute(oneSample.data(), 0, &zeroInputOutput);
    if (zeroInputOutput != 0)
    {
        std::cerr << "[execute() with 0 input samples returned " << zeroInputOutput << " output samples]...";
        return false;
    }

    std::vector<short> silence(sampleRate, 0); // 1 s: ten 10 ms blocks, five loudness updates
    auto output = runThroughAgc(step, silence, sampleRate / 10);

    if ((int)output.size() != sampleRate)
    {
        std::cerr << "[got " << output.size() << " samples back from " << sampleRate << "]...";
        return false;
    }

    for (size_t i = 0; i < output.size(); i++)
    {
        if (output[i] != 0)
        {
            std::cerr << "[silence in, nonzero sample " << output[i] << " out at " << i << "]...";
            return false;
        }
    }

    return true;
}

// A tone at -28 LUFS: quieter than the target, but above the -33 LUFS
// silence gate. The measurement must pass the gate's loudness threshold and
// the release ramp must start lifting the signal (by roughly 2 dB in 4 s of
// 6 s-constant ramping; the assertion only requires at least 1 dB).
bool agcQuietAboveGateStartsGainRamp()
{
    constexpr int sampleRate = 8000;
    constexpr double RAW_LUFS = -28.0;

    AgcStep step(sampleRate);

    double amplitude = findAmplitudeForLoudness(RAW_LUFS, TEST_TONE_FREQ_HZ, sampleRate);
    auto quietSignal = generateSineWave(amplitude, TEST_TONE_FREQ_HZ, 4.0, sampleRate);

    double rawLufs = measureLoudnessLufs(quietSignal.data() + sampleRate, sampleRate, sampleRate);
    if (rawLufs <= -33.0 || rawLufs >= AGC_TARGET_LUFS)
    {
        std::cerr << "[test signal is " << rawLufs << " LUFS, expected between the -33 gate and "
                   << AGC_TARGET_LUFS << "]...";
        return false;
    }

    auto output = runThroughAgc(step, quietSignal, sampleRate / 10);
    if ((int)output.size() != 4 * sampleRate)
    {
        std::cerr << "[got " << output.size() << " samples back from " << 4 * sampleRate << "]...";
        return false;
    }

    double outLufs = measureLoudnessLufs(&output[output.size() - sampleRate], sampleRate, sampleRate);
    if (outLufs < rawLufs + 1.0)
    {
        std::cerr << "[quiet-above-gate signal was not ramped up: raw=" << rawLufs
                   << " output=" << outLufs << "]...";
        return false;
    }

    return true;
}

int main()
{
    TEST_CASE(agcConvergesLoudSignalToTargetLoudness);
    TEST_CASE(agcConvergesQuietSignalToTargetLoudness);
    TEST_CASE(agcDoesNotBoostNearSilentSignal);
    TEST_CASE(agcResetReturnsGainToUnity);
    TEST_CASE(agcLimitsCutToTwentyDb);
    TEST_CASE(agcUnsupportedRateRunsAt48k);
    TEST_CASE(agcNativeRates16kAnd32k);
    TEST_CASE(agcSilenceMeasurementsAreRejected);
    TEST_CASE(agcQuietAboveGateStartsGainRamp);
    return 0;
}
