//=========================================================================
// Name:            LevelAdjustStep.cpp
// Purpose:         Describes a level adjust step in the audio pipeline.
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

#include "LevelAdjustStep.h"

#include <functional>
#include <cassert>

LevelAdjustStep::LevelAdjustStep(int sampleRate, realtime_fp<float()> const& scaleFactorFn)
    : scaleFactorFn_(scaleFactorFn)
    , sampleRate_(sampleRate)
{
    // Pre-allocate buffers so we don't have to do so during real-time operation.
    outputSamples_ = std::make_unique<short[]>(sampleRate_);
    assert(outputSamples_ != nullptr);

    tmpFloatInput_ = std::make_unique<float[]>(sampleRate_ / 100);
    assert(tmpFloatInput_ != nullptr);
}

LevelAdjustStep::~LevelAdjustStep()
{
    // empty
}

int LevelAdjustStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

int LevelAdjustStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

short* LevelAdjustStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    float scaleFactor = scaleFactorFn_();
    short* inPtr = inputSamples;
    short* outPtr = outputSamples_.get();
    float* tmpPtr = tmpFloatInput_.get();
    *numOutputSamples = numInputSamples;

    while (numInputSamples > 0)
    {
        int samplesToRead = std::min(numInputSamples, sampleRate_ / 100); // process 10ms blocks
        ConvertToFloatSampleType_<float, short>(inPtr, tmpPtr, samplesToRead);
        for (int index = 0; index < samplesToRead; index++)
        {
            tmpPtr[index] *= scaleFactor;
        }
        ConvertToIntSampleType_<short, float>(tmpPtr, outPtr, samplesToRead);

        inPtr += samplesToRead;
        outPtr += samplesToRead;
        numInputSamples -= samplesToRead;
    }
    
    return outputSamples_.get();
}
