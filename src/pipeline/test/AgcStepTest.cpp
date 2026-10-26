#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "AgcStep.h"
#include "PipelineTestCommon.h"

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

// Unit checks against synthetic tones, in the real closed loop (the
// leveler's feedback is the step's own measured output loudness).
// End-to-end behaviour on real speech is verified with the diagnostic CSV
// logging instead.

namespace {

constexpr double LIMITER_THRESHOLD_DB = -1.5;
constexpr double LIMITER_RATIO = 20.0;

bool disabledFn() FREEDV_NONBLOCKING
{
    return false;
}

bool noiseReductionOffFn() FREEDV_NONBLOCKING
{
    return false;
}

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

// Amplitude for a given peak level in dBFS.
double amplitudeForPeakDbfs(double peakDbfs)
{
    return 32767.0 * std::pow(10.0, peakDbfs / 20.0);
}

// Approximate amplitude for a 1kHz sine of the given EBU R128 loudness: a
// full-scale 1kHz sine measures ~-3.01 LUFS (RMS is 3dB below peak, and
// K-weighting is close to unity at 1kHz).
double amplitudeForLufs(double lufs)
{
    return amplitudeForPeakDbfs(lufs + 3.01);
}

// Streams the given signal through the step in chunks, mimicking real-time
// usage, and returns the concatenated output.
std::vector<short> runThroughStep(AgcStep& step, const std::vector<short>& input, int chunkSize)
{
    std::vector<short> in(input);
    std::vector<short> output;
    output.reserve(in.size());

    for (std::size_t offset = 0; offset < in.size(); offset += chunkSize)
    {
        int numToWrite = static_cast<int>(std::min<std::size_t>(chunkSize, in.size() - offset));
        int numOutputSamples = 0;
        short* result = step.execute(&in[offset], numToWrite, &numOutputSamples);
        output.insert(output.end(), result, result + numOutputSamples);
    }

    return output;
}

// Peak level in dBFS from startIndex onward.
double measurePeakDbfs(const std::vector<short>& samples, std::size_t startIndex = 0)
{
    double peak = 0.0;
    for (std::size_t i = startIndex; i < samples.size(); i++)
    {
        double absVal = std::abs((double)samples[i]) / 32768.0;
        if (absVal > peak) peak = absVal;
    }
    return peak > 0.0 ? 20.0 * std::log10(peak) : -100.0;
}

// Crest factor (peak/RMS) in dB. A pure sine is ~3.01dB; a clipped one is
// noticeably lower.
double measureCrestFactorDb(const std::vector<short>& samples, std::size_t startIndex)
{
    double peak = 0.0;
    double sumSq = 0.0;
    for (std::size_t i = startIndex; i < samples.size(); i++)
    {
        double v = std::abs((double)samples[i]);
        if (v > peak) peak = v;
        sumSq += v * v;
    }
    double rms = std::sqrt(sumSq / (samples.size() - startIndex));
    return 20.0 * std::log10(peak / rms);
}

int countSaturated(const std::vector<short>& samples)
{
    int saturated = 0;
    for (short v : samples)
    {
        if (v >= 32767 || v <= -32767) saturated++;
    }
    return saturated;
}

} // namespace

// ---- Leveler ----

// Output loudness should converge on the target, here a +7dB correction.
bool levelerConvergesOnTarget()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 1.0;

    // Single-argument form, as used by existing callers (no diagnostic
    // logger).
    AgcStep step(sampleRate);

    auto input = generateSineWave(amplitudeForLufs(-30.0), 1000.0, 60.0, sampleRate); // several time constants
    runThroughStep(step, input, sampleRate / 10);

    float outputLufs = step.getLastOutputLoudnessLufs();
    if (std::abs(outputLufs - -23.0) > TOLERANCE_DB)
    {
        std::cerr << "[output " << outputLufs << " LUFS with gain " << step.getCurrentGainDb()
                   << "dB, expected ~-23 LUFS]...";
        return false;
    }

    return true;
}

// A non-default targetLufs should change what the closed loop converges to.
bool levelerConvergesOnConfigurableTarget()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 1.0;
    constexpr float customTargetLufs = -28.0f; // deliberately not the -23.0f default

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, customTargetLufs);

    auto input = generateSineWave(amplitudeForLufs(-33.0), 1000.0, 60.0, sampleRate);
    runThroughStep(step, input, sampleRate / 10);

    float outputLufs = step.getLastOutputLoudnessLufs();
    if (std::abs(outputLufs - customTargetLufs) > TOLERANCE_DB)
    {
        std::cerr << "[output " << outputLufs << " LUFS with gain " << step.getCurrentGainDb()
                   << "dB, expected ~" << customTargetLufs << " LUFS (the custom target, not the -23 default)]...";
        return false;
    }

    return true;
}

// Once the measured loudness falls below the silence threshold, gain
// should hold exactly where it is, even while a correction is in progress.
// The momentary loudness is measured over 400ms, so a sudden drop in input
// takes up to that long to register as a pause.
bool levelerFreezesGainInPauses()
{
    constexpr int sampleRate = 8000;
    constexpr std::size_t meterWindowSamples = sampleRate * 4 / 10;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>());

    // A correction in progress: 1s below target.
    runThroughStep(step, generateSineWave(amplitudeForLufs(-27.0), 1000.0, 1.0, sampleRate), sampleRate / 100);
    float gainBeforePauseDb = step.getCurrentGainDb();

    // A pause begins. Let the meter's window drain, then check gain holds.
    auto pause = generateSineWave(amplitudeForLufs(-60.0), 1000.0, 2.0, sampleRate);
    runThroughStep(step, std::vector<short>(pause.begin(), pause.begin() + meterWindowSamples + sampleRate / 10), sampleRate / 100);
    float gainEarlyInPauseDb = step.getCurrentGainDb();
    runThroughStep(step, std::vector<short>(pause.begin() + meterWindowSamples + sampleRate / 10, pause.end()), sampleRate / 100);
    float gainLateInPauseDb = step.getCurrentGainDb();

    if (gainBeforePauseDb < 0.1f || gainLateInPauseDb != gainEarlyInPauseDb)
    {
        std::cerr << "[gain " << gainBeforePauseDb << "dB before the pause (expected rising), "
                   << gainEarlyInPauseDb << "dB once the pause registered, " << gainLateInPauseDb
                   << "dB later in it (expected held)]...";
        return false;
    }

    return true;
}

// The silence thresholds are -36 LUFS (RNNoise on) and -33 (off), on the
// input level. Checks that both states update at -30 and hold at -40.
// After a loud passage the gain is low, so normal speech comes OUT below the
// silence threshold. The pause test is on the input, so the gain must still
// recover rather than staying trapped as if in a pause.
bool levelerRecoversFromLowGainAfterLoudPassage()
{
    constexpr int sampleRate = 8000;
    // Seeded at -8dB (as after a loud passage), speech at -28 LUFS in: the
    // output is ~-36, below the threshold if it were measured on the output.
    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), -8.0f, -8.0f * 4.0f, -26.0f);
    runThroughStep(step, generateSineWave(amplitudeForLufs(-28.0), 1000.0, 10.0, sampleRate), sampleRate / 100);
    float gainDb = step.getCurrentGainDb();
    if (gainDb < -2.0f)
    {
        std::cerr << "[gain " << gainDb << "dB after 10s of -28 LUFS speech from -8dB (expected recovering towards +2)]...";
        return false;
    }
    return true;
}

// A big, lasting change (here: gain at -8dB, as after a loud passage, then
// normal speech) is corrected within a few seconds, not ~15s.
bool levelerCorrectsBigChangeQuickly()
{
    constexpr int sampleRate = 8000;
    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), -8.0f, -8.0f * 4.0f, -26.0f);
    // -28 LUFS in needs about +2dB.
    runThroughStep(step, generateSineWave(amplitudeForLufs(-28.0), 1000.0, 5.0, sampleRate), sampleRate / 100);
    float gainDb = step.getCurrentGainDb();
    if (gainDb < 0.5f)
    {
        std::cerr << "[gain " << gainDb << "dB after 5s of -28 LUFS speech from -8dB (expected within ~1.5dB of +2)]...";
        return false;
    }
    return true;
}

bool levelerThresholdBehavesTheSameBothWaysNow()
{
    constexpr int sampleRate = 8000;

    auto aboveInput = generateSineWave(amplitudeForLufs(-30.0), 1000.0, 2.0, sampleRate); // should update
    auto belowInput = generateSineWave(amplitudeForLufs(-40.0), 1000.0, 2.0, sampleRate); // should hold

    for (bool noiseReductionOn : {true, false})
    {
        const char* label = noiseReductionOn ? "ON" : "OFF";
        realtime_fp<bool()> nrFn = noiseReductionOn ? +[]() FREEDV_NONBLOCKING { return true; } : +noiseReductionOffFn;

        AgcStep stepAbove(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f, nrFn);
        runThroughStep(stepAbove, aboveInput, sampleRate / 10);
        if (std::abs(stepAbove.getCurrentGainDb()) < 0.1f)
        {
            std::cerr << "[gain didn't move with RNNoise reported " << label
                       << " and loudness above the -33 threshold]...";
            return false;
        }

        AgcStep stepBelow(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f, nrFn);
        runThroughStep(stepBelow, belowInput, sampleRate / 10);
        if (stepBelow.getCurrentGainDb() != 0.0f)
        {
            std::cerr << "[gain moved to " << stepBelow.getCurrentGainDb() << "dB with RNNoise reported " << label
                       << " and loudness below the -33 threshold, expected it held]...";
            return false;
        }
    }

    return true;
}

// reset() is called at the start of every transmission and should leave
// leveler gain unchanged, so each transmission doesn't re-climb from 0dB.
bool levelerResetPreservesGain()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 0.5;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>());

    auto input = generateSineWave(amplitudeForLufs(-30.0), 1000.0, 5.0, sampleRate);
    runThroughStep(step, input, sampleRate / 10);
    float gainBeforeDb = step.getCurrentGainDb();

    step.reset();
    float gainAfterResetDb = step.getCurrentGainDb();
    runThroughStep(step, std::vector<short>(input.begin(), input.begin() + sampleRate / 10), sampleRate / 10);
    float appliedAfterDb = step.getLiveAppliedGainDb();

    if (gainBeforeDb < 1.0f || gainAfterResetDb != gainBeforeDb || std::abs(appliedAfterDb - gainBeforeDb) > TOLERANCE_DB)
    {
        std::cerr << "[gain was " << gainBeforeDb << "dB before reset(), " << gainAfterResetDb
                   << "dB right after, " << appliedAfterDb << "dB applied to the next block"
                   << " -- expected reset() to leave gain unchanged]...";
        return false;
    }

    return true;
}

// A step seeded with saved gain/integral state should report it back,
// ramp the applied gain in at startup, then apply it in full.
bool levelerCanBeSeededWithSavedGain()
{
    constexpr int sampleRate = 8000;
    constexpr float seededGainDb = 7.5f;
    // Consistent with seededGainDb when output loudness is at target
    // (target gain == integral / LEVELER_INTEGRAL_TIME_CONSTANT_SEC, i.e.
    // 7.5 * 4.0). Must be kept in sync by hand with that constant in
    // AgcStep.cpp.
    constexpr float seededIntegralErrorDb = 30.0f;
    constexpr double TOLERANCE_DB = 1.0;

    // A -12dBFS peak tone (~-15 LUFS) clears REAL_AUDIO_PEAK_THRESHOLD
    // (-20dBFS) to start the ramp-in. With the seeded gain it comes out at
    // ~-7.5 LUFS, so that is used as the target to keep the loop at rest.
    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), seededGainDb, seededIntegralErrorDb, -7.5f);

    if (step.getCurrentGainDb() != seededGainDb || step.getIntegralErrorDb() != seededIntegralErrorDb)
    {
        std::cerr << "[getters reported gain=" << step.getCurrentGainDb() << "dB, integralError="
                   << step.getIntegralErrorDb() << "dB right after construction, expected " << seededGainDb
                   << "dB/" << seededIntegralErrorDb << "dB]...";
        return false;
    }

    auto input = generateSineWave(amplitudeForPeakDbfs(-12.0), 1000.0, 0.6, sampleRate);

    // The first 100ms falls within the 300ms ramp-in, so applied gain
    // should be well below the seeded value.
    runThroughStep(step, std::vector<short>(input.begin(), input.begin() + sampleRate / 10), sampleRate / 10);
    float firstBlockGainDb = step.getLiveAppliedGainDb();
    if (firstBlockGainDb > seededGainDb - 3.0f)
    {
        std::cerr << "[gain applied 100ms in was " << firstBlockGainDb << "dB, expected it well below the seeded "
                   << seededGainDb << "dB -- ramp-in doesn't seem to be reducing applied gain at startup]...";
        return false;
    }

    // Past the ramp-in, the full gain should be applied, still close to
    // the seeded value.
    runThroughStep(step, std::vector<short>(input.begin() + sampleRate / 10, input.end()), sampleRate / 10);
    if (step.getLiveAppliedGainDb() != step.getCurrentGainDb() || std::abs(step.getCurrentGainDb() - seededGainDb) > TOLERANCE_DB)
    {
        std::cerr << "[after the ramp window, applied gain " << step.getLiveAppliedGainDb() << "dB, current gain "
                   << step.getCurrentGainDb() << "dB, expected both ~" << seededGainDb << "dB]...";
        return false;
    }

    return true;
}

// The startup ramp-in must be counted from the first real audio, not from
// construction: in use there is idle time between pressing Start and
// speaking. Simulates several seconds of silence first, then checks the
// first block of real audio is still ramping in.
bool levelerRampInWaitsForRealAudioNotJustElapsedTime()
{
    constexpr int sampleRate = 8000;
    constexpr float seededGainDb = 7.5f;
    constexpr float seededIntegralErrorDb = 30.0f; // see levelerCanBeSeededWithSavedGain

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), seededGainDb, seededIntegralErrorDb, -7.5f);

    // 3s of silence, simulating idle time before PTT.
    runThroughStep(step, std::vector<short>(sampleRate * 3, 0), sampleRate / 10);

    // First real audio (level chosen as in levelerCanBeSeededWithSavedGain).
    runThroughStep(step, generateSineWave(amplitudeForPeakDbfs(-12.0), 1000.0, 0.1, sampleRate), sampleRate / 10);
    float firstRealBlockGainDb = step.getLiveAppliedGainDb();
    if (firstRealBlockGainDb > seededGainDb - 3.0f)
    {
        std::cerr << "[first block of real audio (after 3s of prior silence) had gain " << firstRealBlockGainDb
                   << "dB, expected it well below the seeded " << seededGainDb
                   << "dB -- ramp-in appears to be keyed to elapsed time rather than real audio arriving]...";
        return false;
    }

    return true;
}

// With levelling disabled, audio should pass at unity gain, and the saved
// leveler state must be left untouched for when it's re-enabled.
bool levelerDisabledPassesAudioAtUnityAndKeepsState()
{
    constexpr int sampleRate = 8000;
    constexpr float seededGainDb = 6.0f;
    constexpr float seededIntegralErrorDb = 24.0f;
    constexpr double TOLERANCE_DB = 0.2;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), seededGainDb, seededIntegralErrorDb, -23.0f,
                            +[]() FREEDV_NONBLOCKING { return true; }, +disabledFn);

    // -20dBFS peak, ~-23 LUFS: below the limiter, and loud enough that the
    // leveler would move its gain if it were active.
    auto input = generateSineWave(amplitudeForPeakDbfs(-20.0), 1000.0, 1.0, sampleRate);
    auto output = runThroughStep(step, input, sampleRate / 100);

    double gainDb = measurePeakDbfs(output, sampleRate / 10) - measurePeakDbfs(input, sampleRate / 10);
    if (std::abs(gainDb) > TOLERANCE_DB || step.getCurrentGainDb() != seededGainDb || step.getIntegralErrorDb() != seededIntegralErrorDb)
    {
        std::cerr << "[gain " << gainDb << "dB (expected ~0), leveler state " << step.getCurrentGainDb() << "dB/"
                   << step.getIntegralErrorDb() << "dB (expected unchanged " << seededGainDb << "/" << seededIntegralErrorDb << ")]...";
        return false;
    }

    return true;
}

// ---- Limiter ----
// Limiter-only tests disable the leveler, so its gain stays at 0dB.

// A signal well below the limiter's threshold should pass through with
// ~0dB gain reduction: ordinary speech must not be compressed.
bool limiterLeavesQuietSignalUnaffected()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 0.5;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f,
                            +[]() FREEDV_NONBLOCKING { return true; }, +disabledFn);

    auto input = generateSineWave(amplitudeForPeakDbfs(-20.0), 1000.0, 1.0, sampleRate);
    auto output = runThroughStep(step, input, sampleRate / 10);

    double inputPeakDb = measurePeakDbfs(input);
    double outputPeakDb = measurePeakDbfs(output);
    if (std::abs(outputPeakDb - inputPeakDb) > TOLERANCE_DB)
    {
        std::cerr << "[input=" << inputPeakDb << "dBFS, output=" << outputPeakDb
                   << "dBFS, expected ~0dB difference]...";
        return false;
    }

    return true;
}

// A full-scale signal (above the limiter's threshold) should be pulled
// down meaningfully -- confirms the limiter actually engages.
bool limiterReducesGainForLoudSignal()
{
    constexpr int sampleRate = 8000;
    constexpr double MIN_EXPECTED_REDUCTION_DB = 0.5; // conservative lower bound

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f,
                            +[]() FREEDV_NONBLOCKING { return true; }, +disabledFn);

    auto input = generateSineWave(32767.0, 1000.0, 1.0, sampleRate);
    auto output = runThroughStep(step, input, sampleRate / 10);

    // Skip the first 100ms before measuring steady state.
    std::size_t skipSamples = sampleRate / 10;
    double reductionDb = measurePeakDbfs(output, skipSamples) - measurePeakDbfs(input, skipSamples);
    if (reductionDb > -MIN_EXPECTED_REDUCTION_DB)
    {
        std::cerr << "[reduction=" << reductionDb << "dB, expected at least "
                   << MIN_EXPECTED_REDUCTION_DB << "dB of gain reduction on a full-scale tone]...";
        return false;
    }

    return true;
}

// reset() should clear the limiter's gain-reduction state and look-ahead
// buffer, so a quiet signal isn't affected by leftover state from a prior
// loud one.
bool limiterResetClearsGainReductionState()
{
    constexpr int sampleRate = 8000;
    constexpr double TOLERANCE_DB = 0.5;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f,
                            +[]() FREEDV_NONBLOCKING { return true; }, +disabledFn);

    runThroughStep(step, generateSineWave(32767.0, 1000.0, 1.0, sampleRate), sampleRate / 10);

    step.reset();

    auto quietInput = generateSineWave(amplitudeForPeakDbfs(-20.0), 1000.0, 1.0, sampleRate);
    auto output = runThroughStep(step, quietInput, sampleRate / 10);

    double inputPeakDb = measurePeakDbfs(quietInput);
    double outputPeakDb = measurePeakDbfs(output);
    if (std::abs(outputPeakDb - inputPeakDb) > TOLERANCE_DB)
    {
        std::cerr << "[input=" << inputPeakDb << "dBFS, output=" << outputPeakDb
                   << "dBFS right after reset(), expected ~0dB difference]...";
        return false;
    }

    return true;
}

// Leveler gain that pushes peaks above full scale must reach the limiter
// intact and be limited smoothly. The target is set unreachably high so
// the leveler drives its gain up to the +12dB maximum: a -2dBFS sine
// (up to +10dBFS true peak) should come out as an undistorted sine at the
// limiter's ceiling. If the peaks were clipped before limiting, the output
// would still sit near the ceiling but with a much lower crest factor.
bool levelerGainAboveFullScaleIsLimitedNotClipped()
{
    constexpr int sampleRate = 8000;
    constexpr double SINE_CREST_DB = 3.01;
    constexpr double CREST_TOLERANCE_DB = 0.5;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 6.0f, 24.0f, 0.0f);

    auto input = generateSineWave(amplitudeForPeakDbfs(-2.0), 1000.0, 2.0, sampleRate);
    auto output = runThroughStep(step, input, sampleRate / 10);

    // Skip the first second (startup ramp-in and limiter attack).
    std::size_t skipSamples = sampleRate;
    double outputPeakDb = measurePeakDbfs(output, skipSamples);
    double crestDb = measureCrestFactorDb(output, skipSamples);
    float gainDb = step.getCurrentGainDb();

    if (gainDb < 4.0f || outputPeakDb > -0.8 || outputPeakDb < -3.0 || std::abs(crestDb - SINE_CREST_DB) > CREST_TOLERANCE_DB)
    {
        std::cerr << "[leveler gain " << gainDb << "dB, output peak=" << outputPeakDb << "dBFS, crest factor=" << crestDb
                   << "dB -- expected gain well above +2dB, output ~-1dBFS with a sine's ~3.0dB crest factor"
                   << " (lower crest means clipping before the limiter)]...";
        return false;
    }

    return true;
}

// A tone that starts abruptly above full scale must be limited from its
// very first cycle: the output peak must not exceed the knee curve's
// output for that level (-1.5dBFS + overshoot/20), and no samples may hit
// the int16 saturation backstop. Covers low/high voice frequencies, sample
// rates, and true peaks up to the leveler's +12dB maximum gain on a
// full-scale input.
bool limiterCatchesSuddenOnsets()
{
    constexpr double LEVELER_GAIN_DB = 12.0;
    constexpr double TOLERANCE_DB = 0.1;

    for (int sampleRate : {8000, 16000, 48000})
    for (double freqHz : {150.0, 300.0, 1000.0})
    for (double truePeakDb : {2.1, 6.0, 12.0})
    {
        // Seeded at the maximum gain, with a target high enough that the
        // leveler keeps it there.
        AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(),
                                (float)LEVELER_GAIN_DB, (float)LEVELER_GAIN_DB * 4.0f, 0.0f);

        // 400ms of quieter tone to complete the startup ramp-in, 200ms of
        // silence, then the tone starting at a positive peak.
        auto input = generateSineWave(amplitudeForPeakDbfs(-18.0), freqHz, 0.4, sampleRate);
        input.resize(input.size() + sampleRate / 5, 0);
        auto onset = generateSineWave(amplitudeForPeakDbfs(truePeakDb - LEVELER_GAIN_DB), freqHz, 0.2, sampleRate);
        input.insert(input.end(), onset.begin(), onset.end());
        auto output = runThroughStep(step, input, sampleRate / 100);

        int saturated = countSaturated(output);
        double outputPeakDb = measurePeakDbfs(output);
        double expectedMaxDb = LIMITER_THRESHOLD_DB + (truePeakDb - LIMITER_THRESHOLD_DB) / LIMITER_RATIO;
        float appliedGainDb = step.getLiveAppliedGainDb();

        if (std::abs(appliedGainDb - LEVELER_GAIN_DB) > 0.01 || saturated > 0 || outputPeakDb > expectedMaxDb + TOLERANCE_DB)
        {
            std::cerr << "[" << freqHz << "Hz at " << sampleRate << "Hz, true peak +" << truePeakDb
                       << "dBFS (leveler gain " << appliedGainDb << "dB): output peak " << outputPeakDb
                       << "dBFS (limit " << expectedMaxDb << "), " << saturated << " saturated samples]...";
            return false;
        }
    }

    return true;
}

// The limiter must stay effective with levelling disabled: a sudden
// full-scale onset must stay within the knee curve with no saturated
// samples.
bool limiterStaysActiveWithLevelerDisabled()
{
    constexpr int sampleRate = 48000;
    constexpr double EXPECTED_MAX_DB = LIMITER_THRESHOLD_DB + (0.0 - LIMITER_THRESHOLD_DB) / LIMITER_RATIO;
    constexpr double TOLERANCE_DB = 0.1;

    AgcStep step(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f,
                            +[]() FREEDV_NONBLOCKING { return true; }, +disabledFn);

    std::vector<short> input(sampleRate / 5, 0);
    auto onset = generateSineWave(32767.0, 300.0, 0.2, sampleRate);
    input.insert(input.end(), onset.begin(), onset.end());
    auto output = runThroughStep(step, input, sampleRate / 100);

    int saturated = countSaturated(output);
    double outputPeakDb = measurePeakDbfs(output);
    if (saturated > 0 || outputPeakDb > EXPECTED_MAX_DB + TOLERANCE_DB)
    {
        std::cerr << "[output peak " << outputPeakDb << "dBFS (limit " << EXPECTED_MAX_DB << "), "
                   << saturated << " saturated samples, with the leveler disabled]...";
        return false;
    }

    return true;
}

// ---- Output loudness measurement ----

// The same quiet-but-real signal should be rejected by the RNNoise-on
// silence floor (-70 LUFS) but accepted by the lower RNNoise-off floor
// (-85 LUFS). Without RNNoise, gaps between words can measure quieter than
// RNNoise's residual noise floor, so the higher floor would reject real
// speech.
bool loudnessUsesLooserSilenceFloorWithNoiseReductionOff()
{
    constexpr int sampleRate = 8000;

    // -78 LUFS sits roughly in the middle of the two floors with a
    // comfortable margin either side, so this doesn't depend on
    // amplitudeForLufs() being exact. Far below the leveler's -33
    // threshold, so its gain stays at 0dB.
    constexpr double approxTargetLufs = -78.0;
    auto input = generateSineWave(amplitudeForLufs(approxTargetLufs), 1000.0, 2.0, sampleRate); // long enough for the 400ms window

    {
        AgcStep stepOn(sampleRate, std::make_shared<DiagnosticCsvLogger>());
        runThroughStep(stepOn, input, sampleRate / 10);
        float lufsOn = stepOn.getLastOutputLoudnessLufs();
        if (lufsOn > -99.0f)
        {
            std::cerr << "[with RNNoise reported ON, getLastOutputLoudnessLufs()=" << lufsOn
                       << " -- expected it rejected (~-100) by the stricter -70 floor]...";
            return false;
        }
    }

    {
        AgcStep stepOff(sampleRate, std::make_shared<DiagnosticCsvLogger>(), 0.0f, 0.0f, -23.0f, +noiseReductionOffFn);
        runThroughStep(stepOff, input, sampleRate / 10);
        float lufsOff = stepOff.getLastOutputLoudnessLufs();
        if (lufsOff < -85.0f || lufsOff > -50.0f)
        {
            std::cerr << "[with RNNoise reported OFF, getLastOutputLoudnessLufs()=" << lufsOff
                       << " -- expected a genuine reading around " << approxTargetLufs << " LUFS, accepted by the looser -85 floor]...";
            return false;
        }
    }

    return true;
}

int main()
{
    TEST_CASE(levelerConvergesOnTarget);
    TEST_CASE(levelerConvergesOnConfigurableTarget);
    TEST_CASE(levelerFreezesGainInPauses);
    TEST_CASE(levelerRecoversFromLowGainAfterLoudPassage);
    TEST_CASE(levelerCorrectsBigChangeQuickly);
    TEST_CASE(levelerThresholdBehavesTheSameBothWaysNow);
    TEST_CASE(levelerResetPreservesGain);
    TEST_CASE(levelerCanBeSeededWithSavedGain);
    TEST_CASE(levelerRampInWaitsForRealAudioNotJustElapsedTime);
    TEST_CASE(levelerDisabledPassesAudioAtUnityAndKeepsState);
    TEST_CASE(limiterLeavesQuietSignalUnaffected);
    TEST_CASE(limiterReducesGainForLoudSignal);
    TEST_CASE(limiterResetClearsGainReductionState);
    TEST_CASE(levelerGainAboveFullScaleIsLimitedNotClipped);
    TEST_CASE(limiterCatchesSuddenOnsets);
    TEST_CASE(limiterStaysActiveWithLevelerDisabled);
    TEST_CASE(loudnessUsesLooserSilenceFloorWithNoiseReductionOff);
    return 0;
}
