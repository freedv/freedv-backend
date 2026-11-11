#include "ResampleStep.h"
#include "PipelineTestCommon.h"

#include <vector>

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

bool resampleTestCaseCommon(int inputSampleRate, int outputSampleRate)
{
    ResampleStep resampleStep(inputSampleRate, outputSampleRate);
    auto inputSineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(2000, inputSampleRate), std::default_delete<short[]>());
    auto outputSineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(2000, outputSampleRate), std::default_delete<short[]>());
    
    int numOutputSamples = 0;
    resampleStep.execute(inputSineWave.get(), inputSampleRate, &numOutputSamples);
    
    // Allowed output samples are +/- 10% of the theoretical max.
    int minOutputSamples = outputSampleRate * 0.9;
    int maxOutputSamples = outputSampleRate * 1.1;    
    if (numOutputSamples < minOutputSamples || numOutputSamples > maxOutputSamples)
    {
        std::cerr << "[numOutputSamples(" << numOutputSamples << ") != " << outputSampleRate << "]...";
        return false;
    }
    
    return true;
}

bool resampleEqual()
{
    return resampleTestCaseCommon(8000, 8000);
}

bool resampleToHigher()
{
    return resampleTestCaseCommon(8000, 48000);
}

bool resampleToLower()
{
    return resampleTestCaseCommon(48000, 8000);
}

// Streams two seconds of a 400 Hz tone through the resampler in 10 ms blocks
// (441 samples at 44.1 kHz) and checks what comes out: the right number of
// samples (no drift), still a 400 Hz tone, at the same level.
bool resampleToneCommon(int inputSampleRate, int outputSampleRate)
{
    ResampleStep resampleStep(inputSampleRate, outputSampleRate);

    const int seconds = 2;
    const double amplitude = 8000;
    std::vector<short> input(seconds * inputSampleRate);
    for (size_t n = 0; n < input.size(); n++)
    {
        input[n] = (short)(amplitude * std::sin(2 * M_PI * 400.0 * n / inputSampleRate));
    }

    std::vector<short> output;
    const int block = inputSampleRate / 100;
    for (size_t pos = 0; pos < input.size(); pos += block)
    {
        int numOutputSamples = 0;
        short* result = resampleStep.execute(&input[pos], block, &numOutputSamples);
        output.insert(output.end(), result, result + numOutputSamples);
    }

    // Allow for the filter delay, but not for drift.
    int expected = seconds * outputSampleRate;
    if (std::abs((int)output.size() - expected) > outputSampleRate / 50)
    {
        std::cerr << "[" << output.size() << " samples out, expected " << expected << "]...";
        return false;
    }

    // Measure from 100 ms in (past the filter's start-up).
    size_t start = outputSampleRate / 10;
    int crossings = 0;
    double sumSquares = 0;
    for (size_t i = start + 1; i < output.size(); i++)
    {
        if ((output[i - 1] < 0) != (output[i] < 0))
        {
            crossings++;
        }
        sumSquares += (double)output[i] * output[i];
    }
    double measured = (double)(output.size() - start - 1);
    double freq = crossings / (2 * measured / outputSampleRate);
    double levelDb = 20 * std::log10(std::sqrt(sumSquares / measured) / (amplitude / std::sqrt(2.0)));
    if (std::abs(freq - 400) > 4 || std::abs(levelDb) > 0.5)
    {
        std::cerr << "[output is " << freq << " Hz at " << levelDb << " dB, expected 400 Hz at 0 dB]...";
        return false;
    }

    return true;
}

bool resample44100To48000() { return resampleToneCommon(44100, 48000); }
bool resample48000To44100() { return resampleToneCommon(48000, 44100); }
bool resample44100To8000() { return resampleToneCommon(44100, 8000); }
bool resample8000To44100() { return resampleToneCommon(8000, 44100); }
bool resample48000To8000Tone() { return resampleToneCommon(48000, 8000); }

bool linearResampleForPlots()
{
    // The linear variant (for plots only) must resample just like the
    // default one, at the expected output count.
    ResampleStep resampleStep(8000, 48000, true);
    auto inputSineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(2000, 8000), std::default_delete<short[]>());
    int numOutputSamples = 0;
    resampleStep.execute(inputSineWave.get(), 8000, &numOutputSamples);
    if (numOutputSamples < 48000 * 0.9 || numOutputSamples > 48000 * 1.1)
    {
        std::cerr << "[linear resampling produced " << numOutputSamples << " samples, expected 48000]...";
        return false;
    }
    return true;
}

// Regression: feeding exactly one second of input at an integer upsample
// ratio must not push more samples into the one-second output buffer than
// it can hold. The linear converter emits ceil(input*ratio) per 10 ms
// chunk (80 samples at x6 yield 481), which used to overshoot by one
// sample per chunk: 100 chunks wrote 48100 shorts into a 48000-sample
// buffer.
bool linearOneSecondFeedStaysWithinOutputBuffer()
{
    ResampleStep resampleStep(8000, 48000, true);
    auto inputSineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(1000, 8000), std::default_delete<short[]>());

    int numOutputSamples = 0;
    resampleStep.execute(inputSineWave.get(), 8000, &numOutputSamples);

    bool result = numOutputSamples > 0 && numOutputSamples <= 48000;
    if (!result)
    {
        std::cerr << "[linear one-second feed produced " << numOutputSamples
                  << " samples, more than the 48000-sample buffer]...";
    }
    return result;
}

// Regression: a call carrying only one or two samples. libsamplerate's
// linear converter interpolates from data_in[-1] before it has consumed
// anything from the buffer it was just handed, which read one float before
// tempInput_ (ASan: heap-buffer-overflow) -- one sample at any ratio, two
// when upsampling. Short calls must stay in bounds and must not disturb the
// converter: the rest of the second still has to come out at the expected
// count.
bool shortFeedsStayInBounds()
{
    const int RATES[][2] = { {48000, 8000}, {8000, 48000}, {16000, 8000} };

    for (auto& rate : RATES)
    {
        ResampleStep resampleStep(rate[0], rate[1], true);
        std::vector<short> input(rate[0]);
        for (size_t n = 0; n < input.size(); n++)
        {
            input[n] = (short)(8000 * std::sin(2 * M_PI * 400.0 * n / rate[0]));
        }

        int total = 0;
        size_t pos = 0;
        for (int count = 1; count <= 3; count++)
        {
            int numOutputSamples = 0;
            resampleStep.execute(&input[pos], count, &numOutputSamples);
            total += numOutputSamples;
            pos += count;
        }

        while (pos < input.size())
        {
            int count = std::min(rate[0] / 100, (int)(input.size() - pos));
            int numOutputSamples = 0;
            resampleStep.execute(&input[pos], count, &numOutputSamples);
            total += numOutputSamples;
            pos += count;
        }

        if (std::abs(total - rate[1]) > rate[1] / 50)
        {
            std::cerr << "[" << rate[0] << "->" << rate[1] << " produced " << total
                      << " samples, expected " << rate[1] << "]...";
            return false;
        }
    }
    return true;
}

int main()
{
    TEST_CASE(resampleEqual);
    TEST_CASE(resampleToHigher);
    TEST_CASE(resampleToLower);
    TEST_CASE(resample44100To48000);
    TEST_CASE(resample48000To44100);
    TEST_CASE(resample44100To8000);
    TEST_CASE(resample8000To44100);
    TEST_CASE(resample48000To8000Tone);
    TEST_CASE(linearResampleForPlots);
    TEST_CASE(linearOneSecondFeedStaysWithinOutputBuffer);
    TEST_CASE(shortFeedsStayInBounds);
    return 0;
}
