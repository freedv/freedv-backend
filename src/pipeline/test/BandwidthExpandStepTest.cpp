// Functional tests for BandwidthExpandStep (16 kHz in, 48 kHz out): output
// comes in whole 20 ms frames, three samples out per sample in, the speech
// band passes through at its original level, the step adds content above
// the 8 kHz input band, and a large block is handled like small ones.

#include <cmath>
#include <vector>

#include "BandwidthExpandStep.h"
#include "ResampleStep.h"
#include "PipelineTestCommon.h"

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int IN_RATE = 16000;
constexpr int OUT_RATE = 48000;
constexpr int FRAME = 320; // 20 ms at 16 kHz

// A crude voiced vowel: a 120 Hz pulse train through two formant
// resonators, band-limited to below 8 kHz like 16 kHz speech.
std::vector<short> makeVowel(int numSamples)
{
    std::vector<short> out(numSamples);
    const double formants[] = {700, 1200};
    double y1[2] = {0, 0}, y2[2] = {0, 0};
    for (int n = 0; n < numSamples; n++)
    {
        double x = (n % (IN_RATE / 120)) == 0 ? 1.0 : 0.0;
        for (int f = 0; f < 2; f++)
        {
            double r = 0.9;
            double w = 2 * M_PI * formants[f] / IN_RATE;
            double y = x + 2 * r * std::cos(w) * y1[f] - r * r * y2[f];
            y2[f] = y1[f];
            y1[f] = y;
            x = y;
        }
        out[n] = (short)std::max(-32767.0, std::min(32767.0, x * 300));
    }
    return out;
}

std::vector<short> makeTone(double freq, double amplitude, int numSamples)
{
    std::vector<short> out(numSamples);
    for (int n = 0; n < numSamples; n++)
    {
        out[n] = (short)(amplitude * std::sin(2 * M_PI * freq * n / IN_RATE));
    }
    return out;
}

std::vector<short> runInBlocks(BandwidthExpandStep& step, std::vector<short>& input, int block)
{
    std::vector<short> output;
    for (size_t pos = 0; pos < input.size(); pos += block)
    {
        int count = (int)std::min<size_t>(block, input.size() - pos);
        int numOut = 0;
        short* result = step.execute(&input[pos], count, &numOut);
        output.insert(output.end(), result, result + numOut);
    }
    return output;
}

// Amplitude of one frequency component (Goertzel).
double amplitudeAt(const short* samples, size_t count, double freq, int rate)
{
    double w = 2 * M_PI * freq / rate;
    double coeff = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < count; i++)
    {
        // Hann window, so strong low harmonics don't leak into the bins
        // being measured.
        double window = 0.5 - 0.5 * std::cos(2 * M_PI * i / (count - 1));
        double s = samples[i] * window + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 4 * std::sqrt(std::max(0.0, power)) / count; // x2 for the window's gain
}

// Fraction of the signal's energy above `cutoff` Hz, from a DFT over bins.
double energyFractionAbove(const short* samples, size_t count, double cutoff, int rate)
{
    double above = 0, total = 0;
    for (double f = 50; f < rate / 2; f += 50)
    {
        double a = amplitudeAt(samples, count, f, rate);
        total += a * a;
        if (f > cutoff) above += a * a;
    }
    return total > 0 ? above / total : 0;
}

} // namespace

bool outputComesInWholeFrames()
{
    BandwidthExpandStep step;
    auto input = makeVowel(2 * FRAME);
    int numOut = -1;

    step.execute(input.data(), 100, &numOut);
    if (numOut != 0)
    {
        std::cerr << "[100 samples (less than a frame) gave " << numOut << " out]...";
        return false;
    }
    step.execute(input.data() + 100, FRAME - 100, &numOut);
    if (numOut != 3 * FRAME)
    {
        std::cerr << "[completing the frame gave " << numOut << " out, expected " << 3 * FRAME << "]...";
        return false;
    }
    step.execute(input.data() + FRAME, FRAME + 0, &numOut);
    if (numOut != 3 * FRAME)
    {
        std::cerr << "[second frame gave " << numOut << " out]...";
        return false;
    }
    return true;
}

bool speechBandPassesAtOriginalLevel()
{
    // A 1 kHz tone (inside the 16 kHz input's band) must come out at about
    // the same level.
    BandwidthExpandStep step;
    const double amplitude = 4000;
    auto input = makeTone(1000, amplitude, IN_RATE);
    auto output = runInBlocks(step, input, FRAME);

    size_t start = OUT_RATE / 5; // past the model's start-up
    double got = amplitudeAt(&output[start], output.size() - start, 1000, OUT_RATE);
    double db = 20 * std::log10(got / amplitude);
    if (std::abs(db) > 3)
    {
        std::cerr << "[1 kHz came out at " << db << " dB]...";
        return false;
    }
    return true;
}

bool addsContentAboveInputBand()
{
    // The point of the step: speech limited to 8 kHz gets content above it.
    // Plain resampling to 48 kHz adds essentially none, so compare with that.
    auto input = makeVowel(IN_RATE);

    BandwidthExpandStep step;
    auto expanded = runInBlocks(step, input, FRAME);

    ResampleStep resampler(IN_RATE, OUT_RATE);
    std::vector<short> resampled;
    for (size_t pos = 0; pos < input.size(); pos += FRAME)
    {
        int numOut = 0;
        short* result = resampler.execute(&input[pos], FRAME, &numOut);
        resampled.insert(resampled.end(), result, result + numOut);
    }

    size_t start = OUT_RATE / 5;
    size_t count = OUT_RATE / 2;
    double withBwe = energyFractionAbove(&expanded[start], count, 8500, OUT_RATE);
    double plain = energyFractionAbove(&resampled[start], count, 8500, OUT_RATE);
    if (withBwe < 10 * plain)
    {
        std::cerr << "[above 8.5 kHz: " << withBwe * 100 << "% of the energy expanded vs "
                  << plain * 100 << "% just resampled]...";
        return false;
    }
    return true;
}

bool largeBlockMatchesSmallBlocks()
{
    // 10000 samples in one call is more than the step's 8000-sample input
    // buffer. The output must be the same as feeding it in frames.
    auto input = makeVowel(10000);

    BandwidthExpandStep small;
    auto expected = runInBlocks(small, input, FRAME);

    BandwidthExpandStep large;
    auto got = runInBlocks(large, input, (int)input.size());

    if (got.size() != expected.size())
    {
        std::cerr << "[one large block gave " << got.size() << " samples, frames gave " << expected.size() << "]...";
        return false;
    }
    if (got != expected)
    {
        std::cerr << "[one large block gave different audio]...";
        return false;
    }
    return true;
}

bool nullInputProducesNoOutput()
{
    // execute() with a null buffer must produce nothing: the feeder checks
    // the pointer before handing samples to the frame FIFO.
    BandwidthExpandStep step;
    int numOut = -1;
    step.execute(nullptr, 2 * FRAME, &numOut);
    if (numOut != 0)
    {
        std::cerr << "[null input produced " << numOut << " output samples]...";
        return false;
    }
    return true;
}

int main()
{
    TEST_CASE(outputComesInWholeFrames);
    TEST_CASE(speechBandPassesAtOriginalLevel);
    TEST_CASE(addsContentAboveInputBand);
    TEST_CASE(largeBlockMatchesSmallBlocks);
    TEST_CASE(nullInputProducesNoOutput);
    return 0;
}
