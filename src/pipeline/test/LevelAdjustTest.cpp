#include <limits>
#include <vector>
#include "LevelAdjustStep.h"
#include "PipelineTestCommon.h"

static float LevelAdjustCommonVal;

bool levelAdjustCommon(float val)
{
    LevelAdjustCommonVal = val;
    LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });
    
    int outputSamples = 0;
    std::unique_ptr<short[]> pData = std::make_unique<short[]>(1);
    pData[0] = 10000;
    
    auto result = levelAdjustStep.execute(pData.get(), 1, &outputSamples);
    if (outputSamples != 1)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 1]...";
        return false;
    }
    
    auto expectedVal = 10000 * val;
    if (result[0] < (expectedVal - 1) || result[0] > (expectedVal + 1))
    {
        std::cerr << "[result[" << result[0] << "] != " << expectedVal << "] +/- 1...";
        return false;
    }
    
    return true;
}

bool levelAdjustUp()
{
    return levelAdjustCommon(2.0);
}

bool levelAdjustDown()
{
    return levelAdjustCommon(0.5);
}

bool levelAdjustClipsInsteadOfWrapping()
{
    // Gain that takes samples past full scale must clip them at the limits,
    // not wrap around to the opposite sign (as ToneInterfererStep once did).
    LevelAdjustCommonVal = 4.0;
    LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });

    short input[] = {20000, -20000, 32767, -32768, 8000, -8000, 9000, -9000};
    const short expected[] = {32767, -32768, 32767, -32768, 32000, -32000, 32767, -32768};
    int outputSamples = 0;
    auto result = levelAdjustStep.execute(input, 8, &outputSamples);
    if (outputSamples != 8)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 8]...";
        return false;
    }
    for (int i = 0; i < 8; i++)
    {
        if (result[i] != expected[i])
        {
            std::cerr << "[" << input[i] << " x 4 -> " << result[i] << ", expected " << expected[i] << "]...";
            return false;
        }
    }
    return true;
}

// outputSamples_ holds one second of audio. A longer block used to be written
// straight through it (ASan heap-buffer-overflow, found by fuzzing execute()
// with random block lengths); the step must truncate at one second.
bool levelAdjustTruncatesBlocksLongerThanASecond()
{
    LevelAdjustCommonVal = 1.0;
    LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });

    std::vector<short> input(3 * 8000, 1000);
    int outputSamples = 0;
    auto result = levelAdjustStep.execute(input.data(), (int)input.size(), &outputSamples);
    if (outputSamples != 8000)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 8000]...";
        return false;
    }
    // Reading back the whole reported block is what trips ASan when the step
    // has run past the end of its buffer.
    for (int i = 0; i < outputSamples; i++)
    {
        if (result[i] != 1000)
        {
            std::cerr << "[" << result[i] << " != 1000 at sample " << i << "]...";
            return false;
        }
    }
    return true;
}

// A negative sample count must not be turned into a huge unsigned one, and a
// null buffer must not be dereferenced.
bool levelAdjustRejectsNegativeSampleCount()
{
    LevelAdjustCommonVal = 1.0;
    LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });

    short input[] = {1000, 2000};
    int outputSamples = 12345;
    levelAdjustStep.execute(input, -1, &outputSamples);
    if (outputSamples != 0)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 0 for a negative input count]...";
        return false;
    }
    return true;
}

// A NaN gain (a caller computing it from loudness can produce one) used to reach
// a float->short cast, which is undefined behaviour that UBSan flags; it must
// become silence instead. An infinite gain must clip, not cast.
bool levelAdjustNonFiniteGainIsSafe()
{
    struct Case { float gain; short positive; short negative; };
    const Case cases[] = {
        {std::numeric_limits<float>::quiet_NaN(), 0, 0},
        {std::numeric_limits<float>::infinity(), 32767, -32768},
        {-std::numeric_limits<float>::infinity(), -32768, 32767},
    };

    for (auto& c : cases)
    {
        LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });
        LevelAdjustCommonVal = c.gain;

        short input[] = {10000, -10000};
        int outputSamples = 0;
        auto result = levelAdjustStep.execute(input, 2, &outputSamples);
        if (outputSamples != 2)
        {
            std::cerr << "[outputSamples[" << outputSamples << "] != 2]...";
            return false;
        }
        if (result[0] != c.positive || result[1] != c.negative)
        {
            std::cerr << "[" << input[0] << " x " << c.gain << " -> " << result[0]
                      << ", expected " << c.positive << "]...";
            return false;
        }
    }
    return true;
}

int main()
{
    TEST_CASE(levelAdjustUp);
    TEST_CASE(levelAdjustDown);
    TEST_CASE(levelAdjustClipsInsteadOfWrapping);
    TEST_CASE(levelAdjustTruncatesBlocksLongerThanASecond);
    TEST_CASE(levelAdjustRejectsNegativeSampleCount);
    TEST_CASE(levelAdjustNonFiniteGainIsSafe);
    return 0;
}
