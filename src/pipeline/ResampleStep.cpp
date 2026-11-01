//=========================================================================
// Name:            ResampeStep.cpp
// Purpose:         Describes a resampling step in the audio pipeline.
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

#include "ResampleStep.h"

#include <algorithm>
#include <assert.h>
#include <cstdio>

ResampleStep::ResampleStep(int inputSampleRate, int outputSampleRate, bool forPlotsOnly)
    : inputSampleRate_(inputSampleRate)
    , outputSampleRate_(outputSampleRate)
{
    assert(inputSampleRate > 0 && outputSampleRate > 0);

    int maxInputLen = inputSampleRate * 10 / 1000;
    int maxOutputLen = outputSampleRate * 20 / 1000; // twice as much time just in case a block is bigger than usual
    int src_error = 0;

    // Pre-allocate buffers so we don't have to do so during real-time
    // operation. All buffers are RAII members and src_new() runs last, so
    // nothing claimed earlier leaks if a later allocation throws.
    outputSamples_ = std::make_unique<short[]>(outputSampleRate);
    assert(outputSamples_ != nullptr);

    tempInput_ = std::make_unique<float[]>(maxInputLen);
    assert(tempInput_ != nullptr);

    tempOutput_ = std::make_unique<float[]>(maxOutputLen);
    assert(tempOutput_ != nullptr);

    resampleState_ = src_new(forPlotsOnly ? SRC_LINEAR : SRC_SINC_MEDIUM_QUALITY, 1, &src_error);
    assert(resampleState_ != nullptr);
}

ResampleStep::~ResampleStep()
{
    src_delete(resampleState_);
}

int ResampleStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return inputSampleRate_;
}

int ResampleStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return outputSampleRate_;
}

short* ResampleStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    if (numInputSamples == 0)
    {
        // Not generating any samples if we haven't gotten any.
        *numOutputSamples = 0;
        return inputSamples;
    }

    if (inputSampleRate_ == outputSampleRate_)
    {
        // shortcut - just return what we got.
        *numOutputSamples = numInputSamples;
        return inputSamples;
    }
    
    *numOutputSamples = 0;

    auto inputPtr = inputSamples;
    auto outputPtr = outputSamples_.get();
    while (numInputSamples > 0)
    {
        SRC_DATA src_data;

        int inputSize = std::min(numInputSamples, inputSampleRate_ * 10 / 1000);
        int outputSize = outputSampleRate_ * 20 / 1000;

        // outputSamples_ holds exactly one second of output, and
        // libsamplerate can overshoot the ideal sample count by a sample
        // per chunk (the linear converter emits ceil(input*ratio) per call,
        // e.g. 80 input samples at x6 produce 481). Cap each chunk's output
        // space at the room left in the buffer, and stop when it's full.
        int roomInOutput = outputSampleRate_ - *numOutputSamples;
        if (roomInOutput <= 0)
        {
            break;
        }
        int outputRoom = std::min(outputSize, roomInOutput);

        // libsamplerate is unlikely to use RT-unsafe constructs in normal use
        // (verified with RTsan-enabled automated testing). Verified on 2025-09-30.
        FREEDV_BEGIN_VERIFIED_SAFE
        src_short_to_float_array(inputPtr, tempInput_.get(), inputSize);
        FREEDV_END_VERIFIED_SAFE

        //ConvertToFloatSampleType_<float, short>(inputPtr, tempInput_, inputSize);

        src_data.data_in = tempInput_.get();
        src_data.data_out = tempOutput_.get();
        src_data.input_frames = inputSize;
        src_data.output_frames = outputRoom;
        src_data.end_of_input = 0;
        src_data.src_ratio = (double)outputSampleRate_/(double)inputSampleRate_;

        // libsamplerate is unlikely to use RT-unsafe constructs in normal use
        // (verified with RTsan-enabled automated testing). Verified on 2025-09-30.
        FREEDV_BEGIN_VERIFIED_SAFE
        int ret = src_process(resampleState_, &src_data);
        assert(ret == 0);
        (void)ret; // silence compiler warnings on release builds -- can't log in RT code.
        assert(src_data.output_frames_gen <= outputRoom);
        FREEDV_END_VERIFIED_SAFE

        //ConvertToIntSampleType_<short, float>(tempOutput_, outputPtr, src_data.output_frames_gen);
        // libsamplerate is unlikely to use RT-unsafe constructs in normal use
        // (verified with RTsan-enabled automated testing). Verified on 2025-09-30.
        FREEDV_BEGIN_VERIFIED_SAFE
        src_float_to_short_array(tempOutput_.get(), outputPtr, src_data.output_frames_gen);
        FREEDV_END_VERIFIED_SAFE


        outputPtr += src_data.output_frames_gen;
        inputPtr += src_data.input_frames_used;
        numInputSamples -= src_data.input_frames_used;
        *numOutputSamples += src_data.output_frames_gen;
        if (src_data.input_frames_used == 0)
        {
            // src_process consumed nothing with room still in the buffer:
            // no further progress is possible from this call.
            break;
        }
    }
    
    return outputSamples_.get();
}

void ResampleStep::reset() FREEDV_NONBLOCKING
{
    src_reset(resampleState_);
}
