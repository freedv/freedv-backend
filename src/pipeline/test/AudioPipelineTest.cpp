#include "PipelineTestCommon.h"
#include "LevelAdjustStep.h"

#include <memory>
#include <vector>
#include <new>

// The resampler-reload internals (resamplers_, resultSampler_,
// reloadResampler_(), reloadResultResampler_()) are only reachable through
// AudioPipeline's private members. Exposing them in this test translation
// unit (the headers themselves are untouched; access control does not
// change object layout or linkage) lets the tests drive those paths
// directly instead of through indirection that can never reach them.
#define private public
#include "AudioPipeline.h"
#undef private

static bool passthroughCommon(int inputSampleRate, int outputSampleRate)
{
    AudioPipeline pipeline(inputSampleRate, outputSampleRate);
    auto sineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(2000, inputSampleRate));
    
    int outputSamples = 0;
    pipeline.execute(sineWave.get(), inputSampleRate, &outputSamples);
    
    auto minOutputSamples = outputSampleRate * 0.9;
    auto maxOutputSamples = outputSampleRate * 1.1;
    
    if (outputSamples < minOutputSamples || outputSamples > maxOutputSamples)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != " << outputSampleRate << " +/- 10%]...";
        return false;
    }
    
    return true;
}

bool passthrough()
{
    return passthroughCommon(8000, 8000);
}

bool passthroughUpsample()
{
    return passthroughCommon(8000, 48000);
}

bool passthroughDownsample()
{
    return passthroughCommon(48000, 8000);
}

bool resampleBeforeStepCommon(int inputSampleRate, int stepSampleRate, int outputSampleRate)
{
    AudioPipeline pipeline(inputSampleRate, outputSampleRate);
    auto levelAdjustStep = new LevelAdjustStep(stepSampleRate, +[]() FREEDV_NONBLOCKING { return (float)1.0; });
    assert(levelAdjustStep != nullptr);
    
    pipeline.appendPipelineStep(levelAdjustStep);
    
    auto sineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(2000, inputSampleRate));
    int numOutputSamples = 0;
    pipeline.execute(sineWave.get(), inputSampleRate, &numOutputSamples);
    
    auto minOutputSamples = outputSampleRate * 0.9;
    auto maxOutputSamples = outputSampleRate * 1.1;
    
    if (numOutputSamples < minOutputSamples || numOutputSamples > maxOutputSamples)
    {
        std::cerr << "[outputSamples[" << numOutputSamples << "] != " << outputSampleRate << " +/- 10%]...";
        return false;
    }
    
    return true;
}

bool upsampleBeforeStep()
{
    return resampleBeforeStepCommon(8000, 48000, 48000);
}

bool upsampleJustForStep()
{
    return resampleBeforeStepCommon(8000, 48000, 8000);
}

bool upsampleOnlyAtEnd()
{
    return resampleBeforeStepCommon(8000, 8000, 48000);
}

bool downsampleBeforeStep()
{
    return resampleBeforeStepCommon(48000, 8000, 8000);
}

bool downsampleJustForStep()
{
    return resampleBeforeStepCommon(48000, 8000, 48000);
}

bool downsampleOnlyAtEnd()
{
    return resampleBeforeStepCommon(48000, 48000, 8000);
}

// Passes audio through unchanged at a fixed rate, counting what it gets.
class CountingStep : public IPipelineStep
{
public:
    explicit CountingStep(int sampleRate) : sampleRate_(sampleRate) {}

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return sampleRate_; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return sampleRate_; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        samplesSeen += numInputSamples;
        *numOutputSamples = numInputSamples;
        return inputSamples;
    }

    int samplesSeen = 0;

private:
    int sampleRate_;
};

// Takes audio at twice its output rate and keeps every other sample: a step
// whose input and output rates differ.
class HalvingStep : public IPipelineStep
{
public:
    explicit HalvingStep(int outputSampleRate)
        : outputSampleRate_(outputSampleRate)
        , buffer_(outputSampleRate)
    {
    }

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return 2 * outputSampleRate_; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return outputSampleRate_; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        int count = 0;
        for (int i = 0; i < numInputSamples; i++)
        {
            if ((phase_++ % 2) == 0)
            {
                buffer_[count++] = inputSamples[i];
            }
        }
        *numOutputSamples = count;
        return buffer_.data();
    }

private:
    int outputSampleRate_;
    int phase_ = 0;
    std::vector<short> buffer_;
};

// Claims an input sample rate no buffer could ever be sized for.
class ImpossibleStep : public IPipelineStep
{
public:
    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return -100000; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        (void)inputSamples;
        (void)numInputSamples;
        *numOutputSamples = 0;
        return nullptr;
    }
};

// Exercises IPipelineStep's protected float conversion helpers with an
// explicit divisor: an ordinary one, and the special case of a divisor
// equal to the float maximum (which divides by max + 1 instead).
class ConversionProbeStep : public IPipelineStep
{
public:
    // 2^128 - 2^104: FLT_MAX as an integer, the one integer value a float
    // can hold exactly at its extreme.
    static constexpr unsigned __int128 MaxFloatAsInteger = ((((unsigned __int128)1) << 24) - 1) << 104;

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        ConvertSingleSampleToFloatSampleType_<float, short, 100>(inputSamples, floatOut_);
        ConvertToFloatSampleType_<float, short, 100>(inputSamples, floatOut_, (std::size_t)numInputSamples);
        unsigned __int128 bigIn = 2;
        ConvertSingleSampleToFloatSampleType_<float, unsigned __int128, MaxFloatAsInteger>(&bigIn, hugeDivOut_);
        *numOutputSamples = numInputSamples;
        return inputSamples;
    }

    float lastDividedSample() const { return floatOut_[0]; }
    float maxDivisorOut() const { return hugeDivOut_[0]; }

private:
    float floatOut_[4] = { 0 };
    float hugeDivOut_[1] = { 0 };
};

// Runs one second of a 400 Hz tone through the pipeline in 20 ms blocks.
static std::vector<short> runOneSecond(AudioPipeline& pipeline)
{
    int inputRate = pipeline.getInputSampleRate();
    auto sineWave = std::unique_ptr<short[]>(generateOneSecondSineWave(2000, inputRate));
    std::vector<short> output;
    for (int pos = 0; pos < inputRate; pos += inputRate / 50)
    {
        int numOutputSamples = 0;
        short* result = pipeline.execute(sineWave.get() + pos, inputRate / 50, &numOutputSamples);
        output.insert(output.end(), result, result + numOutputSamples);
    }
    return output;
}

static bool within10Percent(const char* what, int got, int expected)
{
    if (got < expected * 0.9 || got > expected * 1.1)
    {
        std::cerr << "[" << what << ": " << got << " samples, expected " << expected << " +/- 10%]...";
        return false;
    }
    return true;
}

// The output should still be the 400 Hz tone at its original level.
static bool toneIntact(const std::vector<short>& output, int sampleRate)
{
    // Skip the first 100 ms (resampler start-up).
    size_t start = sampleRate / 10;
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
    double seconds = (double)(output.size() - start - 1) / sampleRate;
    double freq = crossings / (2 * seconds);
    double rms = std::sqrt(sumSquares / (output.size() - start - 1));
    double expectedRms = 2000 / std::sqrt(2.0);
    if (std::abs(freq - 400) > 10 || std::abs(20 * std::log10(rms / expectedRms)) > 1.0)
    {
        std::cerr << "[output tone " << freq << " Hz at RMS " << rms << ", expected 400 Hz at "
                  << expectedRms << "]...";
        return false;
    }
    return true;
}

bool eachStepGetsItsOwnRate()
{
    // 8 kHz in, 48 kHz out, through steps at 8, 16 and 48 kHz: resamplers
    // must appear between the steps (not just at the ends), and the result
    // resampler must be replaced as each step is added (8->48, 16->48, none).
    AudioPipeline pipeline(8000, 48000);
    auto step8 = new CountingStep(8000);
    auto step16 = new CountingStep(16000);
    auto step48 = new CountingStep(48000);
    pipeline.appendPipelineStep(step8);
    pipeline.appendPipelineStep(step16);
    pipeline.appendPipelineStep(step48);

    auto output = runOneSecond(pipeline);
    bool result = within10Percent("8 kHz step", step8->samplesSeen, 8000);
    result &= within10Percent("16 kHz step", step16->samplesSeen, 16000);
    result &= within10Percent("48 kHz step", step48->samplesSeen, 48000);
    result &= within10Percent("output", output.size(), 48000);
    result &= toneIntact(output, 48000);
    return result;
}

bool stepThatChangesRate()
{
    // A step taking 16 kHz and producing 8 kHz, followed by an 8 kHz step:
    // nothing is needed between them, but the pipeline (48 kHz in and out)
    // must convert 48->16 before the first and 8->48 after the last.
    AudioPipeline pipeline(48000, 48000);
    auto halving = new HalvingStep(8000);
    auto step8 = new CountingStep(8000);
    pipeline.appendPipelineStep(halving);
    pipeline.appendPipelineStep(step8);

    auto output = runOneSecond(pipeline);
    bool result = within10Percent("8 kHz step after the halving step", step8->samplesSeen, 8000);
    result &= within10Percent("output", output.size(), 48000);
    result &= toneIntact(output, 48000);
    return result;
}

bool lastStepAtOutputRateNeedsNoResampler()
{
    // After the 8 kHz step the pipeline needs an 8->48 kHz resampler at the
    // end. Appending a 48 kHz step moves that conversion in front of the new
    // step, and the end resampler must go: the output is then exactly what
    // the last step produced.
    AudioPipeline pipeline(8000, 48000);
    auto step8 = new CountingStep(8000);
    auto step48 = new CountingStep(48000);
    pipeline.appendPipelineStep(step8);
    pipeline.appendPipelineStep(step48);

    auto output = runOneSecond(pipeline);
    bool result = within10Percent("48 kHz step", step48->samplesSeen, 48000);
    result &= (output.size() == (size_t)step48->samplesSeen);
    if (!result)
    {
        std::cerr << "[output " << output.size() << " samples vs " << step48->samplesSeen << " from the last step]...";
    }
    result &= toneIntact(output, 48000);
    return result;
}

// dumpSetup() must describe every step, with and without a resampler in
// front of it, and the trailing result resampler when there is one.
bool dumpSetupDescribesResampledAndPlainSteps()
{
    AudioPipeline withResampler(8000, 48000);
    withResampler.appendPipelineStep(new CountingStep(8000));
    withResampler.appendPipelineStep(new CountingStep(16000));
    withResampler.dumpSetup();

    AudioPipeline plain(8000, 8000);
    plain.appendPipelineStep(new CountingStep(8000));
    plain.dumpSetup();
    return true;
}

// Appending a step while a result resampler already exists: the step's
// output still feeds that resampler at its input rate, so the existing
// resampler must survive the append.
bool appendWhileResultSamplerPresent()
{
    AudioPipeline pipeline(8000, 48000);
    pipeline.appendPipelineStep(new CountingStep(8000));

    auto output = runOneSecond(pipeline);
    bool result = within10Percent("output", (int)output.size(), 48000);
    result &= toneIntact(output, 48000);
    return result;
}

// A step claiming an absurd (negative) input sample rate: the resampler
// AudioPipeline builds to join it can't allocate buffers of that size, and
// both join points must let that rejection propagate (the pipeline still
// frees the step it had already taken ownership of).
bool absurdStepRateRejectedAtBothJoinPoints()
{
    bool rejectedFirst = false;
    bool rejectedLater = false;

    {
        AudioPipeline pipeline(8000, 8000);
        try
        {
            pipeline.appendPipelineStep(new ImpossibleStep());
        }
        catch (const std::bad_alloc&)
        {
            rejectedFirst = true;
        }
    }

    {
        AudioPipeline pipeline(8000, 8000);
        pipeline.appendPipelineStep(new CountingStep(8000));
        try
        {
            pipeline.appendPipelineStep(new ImpossibleStep());
        }
        catch (const std::bad_alloc&)
        {
            rejectedLater = true;
        }
    }

    if (!rejectedFirst || !rejectedLater)
    {
        std::cerr << "[absurd step rate " << (rejectedFirst ? "rejected" : "accepted")
                  << " at the start, " << (rejectedLater ? "rejected" : "accepted")
                  << " after a good step]...";
        return false;
    }
    return true;
}

// The conversion helpers' divisor special cases: an ordinary divisor, and
// one equal to the float maximum that forces division by (max + 1).
bool conversionHelperDivisorSpecialCases()
{
    ConversionProbeStep probe;
    short sample = 350;
    int numOut = -1;
    short* returned = probe.execute(&sample, 1, &numOut);
    if (returned != &sample || numOut != 1)
    {
        std::cerr << "[probe step returned wrong buffer or count]...";
        return false;
    }
    if (std::abs(probe.lastDividedSample() - 3.5f) > 1e-3f)
    {
        std::cerr << "[divisor-100 conversion gave " << probe.lastDividedSample() << ", expected 3.5]...";
        return false;
    }
    if (probe.maxDivisorOut() <= 0.0f || probe.maxDivisorOut() >= 1e-30f)
    {
        std::cerr << "[max-float-divisor conversion gave " << probe.maxDivisorOut() << ", expected ~6e-39]...";
        return false;
    }
    return true;
}

// Pass-through step whose reported rates the test can change, so the reload
// paths can be presented with inconsistent (stale) pipeline state.
class MutableStep : public IPipelineStep
{
public:
    MutableStep(int inputSampleRate, int outputSampleRate)
        : inputSampleRate_(inputSampleRate)
        , outputSampleRate_(outputSampleRate)
    {
    }

    void setRates(int inputSampleRate, int outputSampleRate)
    {
        inputSampleRate_ = inputSampleRate;
        outputSampleRate_ = outputSampleRate;
    }

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return inputSampleRate_; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return outputSampleRate_; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        *numOutputSamples = numInputSamples;
        return inputSamples;
    }

private:
    int inputSampleRate_;
    int outputSampleRate_;
};

// reloadResampler_() with a live intermediate resampler: kept while it still
// joins the steps either side of it, rebuilt once both steps' rates move so
// it joins neither.
bool reloadResamplerKeepThenRebuild()
{
    AudioPipeline pipeline(8000, 48000);
    pipeline.appendPipelineStep(new MutableStep(8000, 8000));
    pipeline.appendPipelineStep(new MutableStep(16000, 16000));

    ResampleStep* original = pipeline.resamplers_[1].get();
    bool result = original != nullptr;
    result &= original->getInputSampleRate() == 8000;
    result &= original->getOutputSampleRate() == 16000;

    // Nothing moved: the same resampler stays.
    pipeline.reloadResampler_(1);
    result &= pipeline.resamplers_[1].get() == original;

    // Both step rates moved: the old resampler now matches neither its
    // predecessor's output nor its successor's input, forcing a rebuild.
    MutableStep* step0 = static_cast<MutableStep*>(pipeline.pipelineSteps_[0].get());
    MutableStep* step1 = static_cast<MutableStep*>(pipeline.pipelineSteps_[1].get());
    step0->setRates(8000, 22050);
    step1->setRates(24000, 24000);
    pipeline.reloadResampler_(1);
    result &= pipeline.resamplers_[1] != nullptr;
    result &= pipeline.resamplers_[1]->getInputSampleRate() == 22050;
    result &= pipeline.resamplers_[1]->getOutputSampleRate() == 24000;

    if (!result)
    {
        std::cerr << "[reloadResampler_ keep/rebuild states wrong]...";
    }
    return result;
}

// reloadResultResampler_() presented with every combination of its
// three-part condition: null sampler on an input==output pipeline, a
// matching sampler on an empty pipeline (condition fully false), a sampler
// whose input or output disagrees with the pipeline endpoints, and a
// matching sampler behind a last step that already delivers the pipeline's
// output rate.
bool reloadResultResamplerEveryCondition()
{
    bool result = true;

    {
        // Null sampler: condition true, but last rate equals pipeline
        // output, so it stays null.
        AudioPipeline pipeline(8000, 8000);
        pipeline.reloadResultResampler_();
        result &= pipeline.resultSampler_ == nullptr;
    }

    {
        // Empty pipeline with an exact-fit sampler: whole condition false,
        // sampler untouched.
        AudioPipeline pipeline(8000, 48000);
        pipeline.reloadResultResampler_();
        result &= pipeline.resultSampler_ != nullptr;
        result &= pipeline.resultSampler_->getInputSampleRate() == 8000;
        result &= pipeline.resultSampler_->getOutputSampleRate() == 48000;
    }

    {
        // Empty pipeline, sampler input wrong: rebuilt from pipeline input.
        AudioPipeline pipeline(8000, 48000);
        pipeline.resultSampler_.reset(new ResampleStep(44100, 48000));
        pipeline.reloadResultResampler_();
        result &= pipeline.resultSampler_ != nullptr;
        result &= pipeline.resultSampler_->getInputSampleRate() == 8000;
        result &= pipeline.resultSampler_->getOutputSampleRate() == 48000;
    }

    {
        // Empty pipeline, sampler output wrong (input right): rebuilt.
        AudioPipeline pipeline(8000, 48000);
        pipeline.resultSampler_.reset(new ResampleStep(8000, 22050));
        pipeline.reloadResultResampler_();
        result &= pipeline.resultSampler_ != nullptr;
        result &= pipeline.resultSampler_->getInputSampleRate() == 8000;
        result &= pipeline.resultSampler_->getOutputSampleRate() == 48000;
    }

    {
        // Steps present, sampler already fed by the last step's output
        // rate: condition false, sampler untouched.
        AudioPipeline pipeline(8000, 48000);
        pipeline.appendPipelineStep(new MutableStep(8000, 16000));
        pipeline.reloadResultResampler_();
        result &= pipeline.resultSampler_ != nullptr;
        result &= pipeline.resultSampler_->getInputSampleRate() == 16000;
        result &= pipeline.resultSampler_->getOutputSampleRate() == 48000;
    }

    if (!result)
    {
        std::cerr << "[reloadResultResampler_ condition arms wrong]...";
    }
    return result;
}

// Only the step after a resampler changing its input rate: the resampler
// no longer feeds it the rate it needs, so it must be rebuilt to match. (An
// earlier version kept it, leaving a resampler whose output rate execute()
// asserts against.)
bool reloadResamplerFollowsNextStepsRate()
{
    AudioPipeline pipeline(8000, 48000);
    pipeline.appendPipelineStep(new MutableStep(8000, 8000));
    pipeline.appendPipelineStep(new MutableStep(16000, 16000));

    MutableStep* step1 = static_cast<MutableStep*>(pipeline.pipelineSteps_[1].get());
    step1->setRates(32000, 16000);
    pipeline.reloadResampler_(1);

    bool result = pipeline.resamplers_[1] != nullptr;
    result &= pipeline.resamplers_[1]->getInputSampleRate() == 8000;
    result &= pipeline.resamplers_[1]->getOutputSampleRate() == 32000;

    if (!result)
    {
        std::cerr << "[reloadResampler_ left a resampler that doesn't feed the next step's rate]...";
    }
    return result;
}

// Regression: when reloadResampler_() keeps the existing resampler, it must
// leave it untouched. Historically the keep path re-wrapped the still-live
// raw pointer into the same unique_ptr slot, which is a unique_ptr
// self-move: the object is destroyed, the slot keeps a dangling pointer, and
// pipeline teardown double-frees it.
bool reloadResamplerKeepPathIsSafe()
{
    AudioPipeline pipeline(8000, 48000);
    pipeline.appendPipelineStep(new MutableStep(8000, 8000));
    pipeline.appendPipelineStep(new MutableStep(16000, 16000));

    ResampleStep* original = pipeline.resamplers_[1].get();
    bool result = original != nullptr;

    pipeline.reloadResampler_(1);
    result &= pipeline.resamplers_[1].get() == original;
    result &= original->getOutputSampleRate() == 16000; // still a live object

    // A pass through the pipeline uses the kept resampler.
    auto output = runOneSecond(pipeline);
    result &= within10Percent("output", (int)output.size(), 48000);

    if (!result)
    {
        std::cerr << "[reloadResampler_ keep path replaced or broke the matching resampler]...";
    }
    // Scope exit destroys the pipeline: with the self-move bug this
    // double-frees the resampler (ASan/heap-checker abort).
    return result;
}

int main()
{
    TEST_CASE(passthrough);
    TEST_CASE(passthroughUpsample);
    TEST_CASE(passthroughDownsample);
    
    TEST_CASE(upsampleBeforeStep);
    TEST_CASE(upsampleJustForStep);
    TEST_CASE(upsampleOnlyAtEnd);
    
    TEST_CASE(downsampleBeforeStep);
    TEST_CASE(downsampleJustForStep);
    TEST_CASE(downsampleOnlyAtEnd);
    TEST_CASE(eachStepGetsItsOwnRate);
    TEST_CASE(stepThatChangesRate);
    TEST_CASE(lastStepAtOutputRateNeedsNoResampler);
    TEST_CASE(dumpSetupDescribesResampledAndPlainSteps);
    TEST_CASE(appendWhileResultSamplerPresent);
    TEST_CASE(absurdStepRateRejectedAtBothJoinPoints);
    TEST_CASE(conversionHelperDivisorSpecialCases);
    TEST_CASE(reloadResamplerKeepThenRebuild);
    TEST_CASE(reloadResultResamplerEveryCondition);
    TEST_CASE(reloadResamplerFollowsNextStepsRate);
    TEST_CASE(reloadResamplerKeepPathIsSafe);
    
    return 0;
}
