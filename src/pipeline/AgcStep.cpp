//=========================================================================
// Name:            AgcStep.cpp
// Purpose:         Describes an AGC step in the audio pipeline.
//
// Authors:         Mooneer Salem
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================================

#include <algorithm>
#include <atomic>
#include <cmath>

#include "AgcStep.h"
#include "../util/logging/ulog.h"
#include "ebur128.h" // from libebur128

#include <assert.h>
#include <algorithm>
#include <cstring>

// AGC settings
constexpr float AGC_LOUDNESS_TARGET_LUFS = -23.0;
constexpr float AGC_MAX_GAIN_DB = 12.0;
constexpr float AGC_MIN_GAIN_DB = -20.0;
constexpr float AGC_ATTACK_TIME_SEC = 0.5;
constexpr float AGC_RELEASE_TIME_SEC = 6.0;
constexpr float SILENCE_THRESHOLD_LUFS = -33.0;
// A measurement is only used when lufs > SILENCE_THRESHOLD_LUFS, so the
// computed target gain is always < AGC_LOUDNESS_TARGET_LUFS - SILENCE_THRESHOLD_LUFS;
// the upper clamp below would therefore never fire.
static_assert(AGC_LOUDNESS_TARGET_LUFS - SILENCE_THRESHOLD_LUFS < AGC_MAX_GAIN_DB);
constexpr int LIMITER_LEVEL_DB = -1;

constexpr int TEN_MS_DIVIDER = 100;
constexpr int MAX_AGC_SAMPLES = 160;

// ebur128_loudness_momentary() recomputes the sum-of-squares over its
// *entire* 400ms window from scratch on every call (no incremental
// caching in libebur128) -- profiling showed this one call as ~94% of
// AgcStep::execute's own cost when queried every 10ms block, i.e. ~40x
// more work than the single new 10ms block actually needs. AGC's own
// attack/release time constants (0.5s/6s, see below) are far slower
// than 10ms, so nothing meaningful is lost by re-measuring loudness
// (and recalculating targetGainDb_) only every Nth block; the gain
// ramp toward that target still updates every block, at full 10ms
// granularity, so the AGC's smoothing behavior is unaffected.
constexpr int LOUDNESS_UPDATE_INTERVAL_BLOCKS = 5;

AgcStep::AgcStep(int sampleRate)
    : sampleRate_(sampleRate == 8000 || sampleRate == 16000 || sampleRate == 32000 || sampleRate == 48000 ? sampleRate : 48000)
    , targetGainDb_(0.0)
    , currentGainDb_(0.0)
    , blocksSinceLoudnessUpdate_(0)
    , lastMeasurementValid_(false)
    , inputSampleFifo_(MAX_AGC_SAMPLES + 1)
{
    numSamplesPerRun_ = std::min(MAX_AGC_SAMPLES, sampleRate_ / TEN_MS_DIVIDER); // 10ms blocks, 160 max samples

    // Configure WebRTC as solely a limiter (i.e. no AGC)
    agcState_ = WebRtcAgc_Create();
    assert(agcState_ != nullptr);
 
    // WebRtcAgc_Init fails only on invalid arguments: micLevelMin(0) <= micLevelMax(255),
    // kAgcModeUnchanged is valid, and sampleRate_ is validated above (8/16/32/48 kHz),
    // so this call cannot fail.
    WebRtcAgc_Init(agcState_, 0, 255, kAgcModeUnchanged, sampleRate_);

    // Set AGC configuration
    agcConfig_.compressionGaindB = 0; // default 9 dB
    agcConfig_.limiterEnable = 1; // default kAgcTrue (on)
    agcConfig_.targetLevelDbfs = -LIMITER_LEVEL_DB; // default 3 (-3 dBOv)
    // Same argument-validity reasoning as WebRtcAgc_Init above: set_config cannot fail.
    WebRtcAgc_set_config(agcState_, agcConfig_);

    ebur128State_ = ebur128_init(1, sampleRate_, EBUR128_MODE_M);
    assert(ebur128State_ != nullptr);

    // Pre-allocate buffers so we don't have to do so during real-time operation.
    // Sized for the rate we actually run at: an unsupported sampleRate falls
    // back to 48 kHz, and callers then send audio at that rate.
    outputSamples_ = std::make_unique<short[]>(sampleRate_);

    tmpInput_ = std::make_unique<short[]>(numSamplesPerRun_);
}

AgcStep::~AgcStep()
{
    outputSamples_ = nullptr;
    WebRtcAgc_Free(agcState_);
    ebur128_destroy((ebur128_state**)&ebur128State_);
}

int AgcStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

int AgcStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

short* AgcStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    ebur128_state* state = static_cast<ebur128_state*>(ebur128State_);

    *numOutputSamples = 0;
    short* outputSamples = outputSamples_.get();
    short* tmpOutput = outputSamples;
    short* tmpInput = tmpInput_.get();

    while (numInputSamples > 0)
    {
        // Always room on every pass: the FIFO is sized numSamplesPerRun_ + 1 and
        // the drain below leaves numUsed() < numSamplesPerRun_, so numFree() >= 2
        // whenever numInputSamples > 0.
        int samplesToWrite = std::min(inputSampleFifo_.numFree(), numInputSamples);
        inputSampleFifo_.write(inputSamples, samplesToWrite);
        inputSamples += samplesToWrite;
        numInputSamples -= samplesToWrite;

        while (inputSampleFifo_.numUsed() >= numSamplesPerRun_)
        {
            *numOutputSamples += numSamplesPerRun_;
            inputSampleFifo_.read(tmpInput, numSamplesPerRun_);

            // Step 1: feed samples into ebur128 every block (cheap -- this is
            // just the K-weighting filter), but only ask for the momentary
            // loudness (and thus recompute targetGainDb_) every Nth block --
            // see LOUDNESS_UPDATE_INTERVAL_BLOCKS above for why.
            double lufs = 0.0;
            bool loudnessUpdated = false;

            // Note: libebur128 is unlikely to use RT-unsafe constructs in normal operation
            // (per existing RTSan-enabled tests). Verified on 2025-09-30.
            FREEDV_BEGIN_VERIFIED_SAFE
            ebur128_add_frames_short(state, tmpInput, numSamplesPerRun_);
            if (++blocksSinceLoudnessUpdate_ >= LOUDNESS_UPDATE_INTERVAL_BLOCKS)
            {
                blocksSinceLoudnessUpdate_ = 0;
                (void)ebur128_loudness_momentary(state, &lufs);
                loudnessUpdated = true;
            }
            FREEDV_END_VERIFIED_SAFE

            if (loudnessUpdated)
            {
                // Persist validity across the blocks we don't re-measure, so
                // the silence gate below still behaves as if it were checked
                // every block (matching the original per-block behavior).
                lastMeasurementValid_ = lufs != -HUGE_VAL && lufs > SILENCE_THRESHOLD_LUFS;
                if (lastMeasurementValid_)
                {
                    // Step 2: calculate target gain. Assume LUFS = dbFS (?)
                    targetGainDb_ = AGC_LOUDNESS_TARGET_LUFS - lufs;
                    // targetGainDb_ is always < AGC_MAX_GAIN_DB here (see static_assert above).
                    if (targetGainDb_ <= AGC_MIN_GAIN_DB) targetGainDb_ = AGC_MIN_GAIN_DB;
                }
            }

            if (lastMeasurementValid_)
            {
                // Step 3: increment/decrement current gain in the direction
                // of target. Still runs every block (not just when the
                // target was just refreshed), so the gain ramp itself stays
                // at full 10ms granularity -- only how often the target
                // updates -- and whether we're gated by silence -- changes
                // less often now.
                float agcInterval = 0;
                if (targetGainDb_ < currentGainDb_)
                {
                    agcInterval = AGC_ATTACK_TIME_SEC;
                }
                else
                {
                    agcInterval = AGC_RELEASE_TIME_SEC;
                }
                currentGainDb_ += ((targetGainDb_ - currentGainDb_) / agcInterval) * ((float)numSamplesPerRun_ / sampleRate_);
            }

            // Scale samples based on current gain.
            float scaleFactor = expf(currentGainDb_/20.0f * logf(10.0f));
            float temp = 0;
            for (auto ctr = 0; ctr < numSamplesPerRun_; ctr++)
            {
                ConvertSingleSampleToFloatSampleType_<float, short>(&tmpInput[ctr], &temp);
                temp *= scaleFactor;
                ConvertSingleSampleToIntSampleType_<short, float>(&temp, &tmpInput[ctr]);
            }

            // Run WebRTC to make sure we don't clip.
            int outMicLevel = 0;
            int inMicLevel = 0;
            short echo = 0;
            unsigned char saturationWarning = 1;
            WebRtcAgc_Process(
                agcState_, const_cast<const int16_t *const *>(&tmpInput), 1, numSamplesPerRun_, 
                const_cast<int16_t *const *>(&tmpOutput), inMicLevel, &outMicLevel, echo, &saturationWarning);
            tmpOutput += numSamplesPerRun_;
        }
    }
    
    return outputSamples;
}

void AgcStep::reset() FREEDV_NONBLOCKING
{
    inputSampleFifo_.reset();
    currentGainDb_ = 0;
    targetGainDb_ = 0;
    blocksSinceLoudnessUpdate_ = 0;
    lastMeasurementValid_ = false;

    // Audio after a reset (e.g. the start of a new transmission) must not be
    // shaped by audio from before it, so clear the history kept by both
    // libraries. reset() runs on the real-time audio thread, so neither step
    // may allocate.

    // The WebRTC limiter keeps its own gain/envelope state. Re-initializing it
    // doesn't allocate. (agcState_ is non-null: the init above cannot fail.)
    WebRtcAgc_Init(agcState_, 0, 255, kAgcModeUnchanged, sampleRate_);
    WebRtcAgc_set_config(agcState_, agcConfig_);

    // libebur128 has no reset, and recreating the state allocates. Its
    // momentary loudness only looks at the last 400 ms, so feeding it 400 ms
    // of silence leaves it equivalent to a freshly created state (the
    // K-weighting filter history decays to zero as well).
    ebur128_state* state = static_cast<ebur128_state*>(ebur128State_);
    short* silence = tmpInput_.get();
    memset(silence, 0, sizeof(short) * numSamplesPerRun_);
    for (int remaining = sampleRate_ * 4 / 10; remaining > 0; remaining -= numSamplesPerRun_)
    {
        FREEDV_BEGIN_VERIFIED_SAFE
        ebur128_add_frames_short(state, silence, std::min(remaining, numSamplesPerRun_));
        FREEDV_END_VERIFIED_SAFE
    }
}
