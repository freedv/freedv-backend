// Tests for MuteStep, which replaces its input with silence while scaling the
// sample count to the output rate.

#include <vector>

#include "MuteStep.h"
#include "PipelineTestCommon.h"

namespace {

// Runs one block and checks the output is `expectedCount` zero samples.
bool producesSilence(MuteStep& step, int numInput, int expectedCount)
{
    std::vector<short> input(numInput > 0 ? numInput : 1, 12345);
    int numOut = -1;
    short* out = step.execute(input.data(), numInput, &numOut);
    if (numOut != expectedCount)
    {
        std::cerr << "[" << numOut << " samples, expected " << expectedCount << "]...";
        return false;
    }
    if (expectedCount == 0)
    {
        return out == nullptr;
    }
    for (int i = 0; i < numOut; i++)
    {
        if (out[i] != 0)
        {
            std::cerr << "[sample " << i << " is " << out[i] << "]...";
            return false;
        }
    }
    return true;
}

bool sameRate()
{
    MuteStep step(8000);
    bool result = step.getInputSampleRate() == 8000 && step.getOutputSampleRate() == 8000;
    result &= producesSilence(step, 160, 160);
    // Silence stays silent across calls (the buffer is never written to).
    result &= producesSilence(step, 80, 80);
    return result;
}

bool upsample()
{
    MuteStep step(8000, 48000);
    bool result = step.getInputSampleRate() == 8000 && step.getOutputSampleRate() == 48000;
    result &= producesSilence(step, 160, 960);
    return result;
}

bool downsample()
{
    MuteStep step(48000, 8000);
    bool result = step.getInputSampleRate() == 48000 && step.getOutputSampleRate() == 8000;
    result &= producesSilence(step, 960, 160);
    // 44.1 kHz -> 48 kHz rounds down, like the other rate-changing steps.
    MuteStep odd(44100, 48000);
    result &= producesSilence(odd, 441, 480);
    result &= producesSilence(odd, 100, 108);
    return result;
}

bool noInput()
{
    MuteStep step(8000, 48000);
    // Fewer input samples than produce one output sample, and none at all.
    MuteStep down(48000, 8000);
    return producesSilence(step, 0, 0) && producesSilence(down, 5, 0);
}

bool fullSecond()
{
    // The output buffer holds exactly one second at the output rate.
    MuteStep step(8000, 48000);
    return producesSilence(step, 8000, 48000);
}

// The rate conversion is done in 64-bit: numInputSamples * outputSampleRate_
// overflows a 32-bit int for blocks of a few tens of milliseconds at these
// rates (UBSan signed-integer-overflow, found by fuzzing execute()). The result
// is still capped at the one second the output buffer holds.
bool hugeBlockDoesNotOverflowOrOverrun()
{
    MuteStep step(8000, 48000);

    // 8000 samples x 48000 = 384,000,000: fits. 1,000,000 x 48000 does not.
    bool result = producesSilence(step, 1000000, 48000);

    MuteStep up(8000, 192000);
    result &= producesSilence(up, 3 * 8000, 192000);
    return result;
}

bool negativeInputCountIsRejected()
{
    MuteStep step(8000, 48000);
    return producesSilence(step, -1000, 0);
}

} // namespace

int main()
{
    TEST_CASE(sameRate);
    TEST_CASE(upsample);
    TEST_CASE(downsample);
    TEST_CASE(noInput);
    TEST_CASE(fullSecond);
    TEST_CASE(hugeBlockDoesNotOverflowOrOverrun);
    TEST_CASE(negativeInputCountIsRejected);
    return 0;
}
