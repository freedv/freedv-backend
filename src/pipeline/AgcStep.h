//=========================================================================
// Name:            AgcStep.h
// Purpose:         Describes a loudness leveler + look-ahead peak limiter
//                  step in the audio pipeline.
//
// Authors:         Claude Code (for Barry Jackson, G4MKT), based on design
//                  suggestions from g4dya (Richard) on PR #1472
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

#ifndef AUDIO_PIPELINE__AGC_STEP_H
#define AUDIO_PIPELINE__AGC_STEP_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "IPipelineStep.h"
#include "../util/LoudnessMeter.h"
#include "../util/DiagnosticCsvLogger.h"
#include "../util/realtime_fp.h"

// TX AGC. Replaces the earlier WebRTC-based implementation (whose
// WebRtcAgc_Process, even with compressionGaindB=0, applies a fixed internal
// ~3:1 compression ratio with makeup gain rather than acting as a pure
// limiter). The AgcStep(int sampleRate) form of the constructor is kept.
//
// Two stages, run per 10ms chunk entirely in floating point, so leveler
// gain that pushes peaks above full scale reaches the limiter intact and
// samples are only converted back to int16 once, after limiting:
//
// 1. Leveler: slowly adjusts gain (within +/-12dB) so that the momentary
//    loudness measured at this step's *output* converges on a target LUFS.
//    Measuring after the limiter closes the loop, so the leveler responds
//    to what is actually sent to the encoder, and brief peaks caught by
//    the limiter don't pull its gain down. Gain is driven by a PI
//    controller and smoothed, so the correction scales with the size of
//    the error and settles on the target rather than moving at a fixed
//    dB/sec rate. Gain holds during pauses in speech.
//
// 2. Look-ahead peak limiter driving a single high-ratio soft knee just
//    below full scale, so it only engages on loud excursions near clipping
//    and leaves ordinary speech untouched. The audio is delayed by the
//    look-ahead window; the gain each sample needs is held at its minimum
//    across the window, then smoothed by a moving average of the same
//    length. The gain therefore ramps down smoothly ahead of each peak and
//    is fully in place when the peak reaches the output, so peaks never
//    overshoot the knee curve, however sudden their onset. Leaving speech
//    dynamics alone matters because the RADE encoder is expected to have
//    been trained on uncompressed speech. A lower "compressor" knee was
//    tried and removed: its frequent gain reduction fed the leveler's
//    feedback loop an upward bias on loud input.
class AgcStep : public IPipelineStep
{
public:
    // diagLogger: optional diagnostic CSV logger (see DiagnosticCsvLogger.h).
    // initialGainDb/initialIntegralErrorDb: resume leveler state saved from
    //   a previous session (e.g. in the application's config file) instead
    //   of starting at 0dB.
    // targetLufs: loudness target for this step's output.
    // noiseReductionEnabledFn: polled each chunk to choose the silence
    //   thresholds. With RNNoise off, gaps between words can measure
    //   quieter than RNNoise's own residual noise floor, so a lower floor
    //   is used for the loudness measurement to avoid rejecting quiet
    //   speech.
    // enabledFn: polled each chunk. While false, leveler gain is 0dB and
    //   its state is frozen, so re-enabling resumes from where it was. The
    //   limiter stays in circuit either way.
    AgcStep(int sampleRate, std::shared_ptr<DiagnosticCsvLogger> diagLogger = nullptr,
             float initialGainDb = 0.0f, float initialIntegralErrorDb = 0.0f, float targetLufs = -23.0f,
             realtime_fp<bool()> const& noiseReductionEnabledFn = +[]() FREEDV_NONBLOCKING { return true; },
             realtime_fp<bool()> const& enabledFn = +[]() FREEDV_NONBLOCKING { return true; });
    virtual ~AgcStep();

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING override;
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING override;
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING override;
    virtual void reset() FREEDV_NONBLOCKING override;

    // Leveler state, for persisting across sessions. Not synchronized:
    // only call once the pipeline thread has stopped calling execute().
    float getCurrentGainDb() const FREEDV_NONBLOCKING;
    float getIntegralErrorDb() const FREEDV_NONBLOCKING;

    // Most recent momentary loudness of this step's output, or -100.0 if
    // the last reading was silent/invalid. Not synchronized, as above.
    float getLastOutputLoudnessLufs() const FREEDV_NONBLOCKING;

    // Leveler gain actually applied to the most recent chunk (including the
    // startup ramp-in). Safe to poll from another thread while the
    // pipeline is running, e.g. for a GUI display.
    float getLiveAppliedGainDb() const FREEDV_NONBLOCKING;

private:
    int sampleRate_;
    std::shared_ptr<DiagnosticCsvLogger> diagLogger_;
    realtime_fp<bool()> noiseReductionEnabledFn_;
    realtime_fp<bool()> enabledFn_;

    // Leveler.
    float targetLufs_;
    float targetGainDb_;
    float currentGainDb_;
    // PI controller integral term (accumulated loudness error, dB*sec).
    float integralErrorDb_;
    // Error averaged over FAST_ERROR_AVERAGE_SEC (speech only), for the
    // fast mode.
    float errorAverageDb_;
    // Startup ramp-in: rampStarted_ latches the first time real (non-silent)
    // input is seen; rampElapsedSec_ only advances after that.
    bool rampStarted_;
    float rampElapsedSec_;
    std::atomic<float> liveAppliedGainDb_;

    // Output loudness measurement, the leveler's feedback.
    LoudnessMeter loudnessMeter_;
    float lastOutputLoudnessLufs_;
    int chunksSinceLoudnessUpdate_;

    // Limiter.
    // Returns the minimum of the last windowLength_ values passed in.
    float slidingMinimum_(float value) FREEDV_NONBLOCKING;

    // Look-ahead window length in samples. Must be declared before the
    // buffers below, which are sized from it in the constructor's init list.
    int windowLength_;

    // Audio delay line, windowLength_ - 1 samples.
    std::unique_ptr<float[]> delayBuffer_;
    int delayPos_;

    // Sliding-minimum ring buffer (monotonic queue) of required gains.
    std::unique_ptr<float[]> holdValues_;
    std::unique_ptr<int64_t[]> holdIndices_;
    int holdHead_;
    int holdCount_;
    int64_t sampleIndex_;

    // Held gain after release smoothing (linear, <= 1).
    float releasedGain_;
    float releaseAlpha_;

    // Moving average of releasedGain_ over windowLength_ samples.
    std::unique_ptr<float[]> averageBuffer_;
    double averageSum_;
    int averagePos_;

    std::unique_ptr<short[]> outputSamples_;
};

#endif // AUDIO_PIPELINE__AGC_STEP_H
