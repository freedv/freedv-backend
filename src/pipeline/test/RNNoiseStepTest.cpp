// Functional tests for RNNoiseStep (48 kHz): output comes in whole 10 ms
// frames after a first frame of latency, how the input is split into
// blocks doesn't change the result, and steady noise is suppressed while
// real speech is kept.

#include <cmath>
#include <random>
#include <vector>

#include "RNNoiseStep.h"
#include "ResampleStep.h"
#include "SpeechSample.h"
#include "PipelineTestCommon.h"

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int RATE = 48000;
constexpr int FRAME = 480; // 10 ms

std::vector<short> makeNoise(int numSamples, double rms, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> dist(0, rms);
    std::vector<short> out(numSamples);
    for (auto& s : out)
    {
        s = (short)std::max(-32767.0, std::min(32767.0, dist(rng)));
    }
    return out;
}

std::vector<short> runInBlocks(RNNoiseStep& step, std::vector<short>& input, int block)
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

double rms(const short* samples, size_t count)
{
    double sum = 0;
    for (size_t i = 0; i < count; i++)
    {
        sum += (double)samples[i] * samples[i];
    }
    return std::sqrt(sum / count);
}

// Change in level (dB) from input to output over the last second.
double levelChangeDb(std::vector<short>& input)
{
    RNNoiseStep step;
    auto output = runInBlocks(step, input, FRAME);
    double in = rms(&input[input.size() - RATE], RATE);
    double out = rms(&output[output.size() - RATE], RATE);
    return 20 * std::log10(out / in);
}

} // namespace

bool firstFrameIsLatency()
{
    RNNoiseStep step;
    auto input = makeNoise(11 * FRAME, 1000);
    int numOut = -1;

    step.execute(input.data(), 100, &numOut);
    bool result = numOut == 0;
    step.execute(input.data() + 100, 10 * FRAME - 100, &numOut);
    result &= numOut == 9 * FRAME; // ten frames in, the first held back
    step.execute(input.data() + 10 * FRAME, FRAME, &numOut);
    result &= numOut == FRAME;
    if (!result)
    {
        std::cerr << "[unexpected output count " << numOut << "]...";
    }
    return result;
}

bool blockSizeDoesNotMatter()
{
    auto input = makeNoise(RATE, 1000);
    RNNoiseStep reference;
    auto expected = runInBlocks(reference, input, FRAME);
    for (int block : {1, 7, 333, 1000, RATE / 2})
    {
        RNNoiseStep step;
        auto got = runInBlocks(step, input, block);
        if (got != expected)
        {
            std::cerr << "[blocks of " << block << " gave different output]...";
            return false;
        }
    }
    return true;
}

// Real speech at 48 kHz (empty if the sample can't be found).
std::vector<short> loadSpeech48k(double seconds)
{
    auto speech16k = loadSpeech16k(seconds);
    ResampleStep resampler(16000, RATE);
    std::vector<short> out;
    for (size_t pos = 0; pos + 320 <= speech16k.size(); pos += 320)
    {
        int numOut = 0;
        short* result = resampler.execute(&speech16k[pos], 320, &numOut);
        out.insert(out.end(), result, result + numOut);
    }
    return out;
}

bool suppressesNoiseButKeepsSpeech()
{
    auto speech = loadSpeech48k(6.0);
    if (speech.size() < (size_t)(5 * RATE))
    {
        std::cerr << "[SKIP: speech sample not found]...";
        return true;
    }
    double speechRms = rms(speech.data(), speech.size());

    // Steady noise at the speech's level.
    auto noise = makeNoise(speech.size(), speechRms);
    double noiseDb = levelChangeDb(noise);
    double speechDb = levelChangeDb(speech);
    if (noiseDb > -15 || speechDb < -6)
    {
        std::cerr << "[noise changed by " << noiseDb << " dB, speech by " << speechDb
                  << " dB; expected noise below -15 dB and speech within 6 dB]...";
        return false;
    }
    return true;
}

// outputSamples_ holds one second of audio. Handing execute() more than a
// second in one call used to write past the end of it (ASan
// heap-buffer-overflow, found by fuzzing execute() with random block sizes), so
// the step must stop at the buffer and never report more than it can hold.
bool singleCallStaysWithinOutputBuffer()
{
    RNNoiseStep step;
    auto input = makeNoise(RATE * 3, 10000);

    int numOut = 0;
    short* result = step.execute(input.data(), (int)input.size(), &numOut);
    if (numOut <= 0 || numOut > RATE)
    {
        std::cerr << "[" << numOut << " samples returned from a " << RATE << " sample buffer]...";
        return false;
    }

    // Reading back exactly what the step reports is what trips ASan if it
    // wrote past the allocation. The filter attenuates steady noise, so only
    // require that it actually processed the block.
    int maxAbs = 0;
    for (int n = 0; n < numOut; n++)
    {
        maxAbs = std::max(maxAbs, std::abs((int)result[n]));
    }
    if (maxAbs == 0)
    {
        std::cerr << "[output was entirely silent]...";
        return false;
    }
    return true;
}

int main()
{
    TEST_CASE(firstFrameIsLatency);
    TEST_CASE(blockSizeDoesNotMatter);
    TEST_CASE(suppressesNoiseButKeepsSpeech);
    TEST_CASE(singleCallStaysWithinOutputBuffer);
    return 0;
}
