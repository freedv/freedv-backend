//=========================================================================
// Name:            TapStep.cpp
// Purpose:         Describes a tap step in the audio pipeline.
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

#include "TapStep.h"

#include <assert.h>
#include <future>
#include <chrono>

#if defined(__APPLE__)
#include <pthread.h>
#endif // defined(__APPLE__)

#include "../os/os_interface.h"
#include "../util/logging/ulog.h"

using namespace std::chrono_literals;

TapStep::TapStep(int sampleRate, IPipelineStep* tapStep)
    : tapStep_(tapStep)
    , sampleRate_(sampleRate)
    , endingTapThread_(false)
    , tapThreadInput_(sampleRate * 0.25f)
    , consecutiveDrops_(0)
    , samplesDropped_(0)
{
    tapThread_ = std::thread([&]() {
        const int SAMPLE_RATE_AT_10MS = sampleRate_ / 100;
        short* fifoInput = new short[SAMPLE_RATE_AT_10MS];
        assert(fifoInput != nullptr);

        SetThreadName("TapStep");

#if defined(__APPLE__)
        // Downgrade thread QoS to Utility to avoid thread contention issues.        
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif // defined(__APPLE__)

        auto processBufferedBlocks = [&]() {
            while (tapThreadInput_.numUsed() >= SAMPLE_RATE_AT_10MS)
            {
                int temp = 0;
                tapThreadInput_.read(fifoInput, SAMPLE_RATE_AT_10MS);
                tapStep_->execute(fifoInput, SAMPLE_RATE_AT_10MS, &temp);
            }
        };

        while (!endingTapThread_)
        {
            processBufferedBlocks();
            sem_.wait();
        }

        // Hand over whatever arrived before we were told to stop, so that
        // e.g. a recording doesn't lose its last moments.
        processBufferedBlocks();

        delete[] fifoInput;
    });
}

TapStep::~TapStep()
{
    endingTapThread_ = true;
    sem_.signal();
    tapThread_.join();

    if (consecutiveDrops_ > 0)
    {
        log_warn("Tap FIFO was still full when stopped; tap is missing %d samples", samplesDropped_);
    }
}

int TapStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

int TapStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

short* TapStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    assert(tapStep_->getInputSampleRate() == sampleRate_);

    *numOutputSamples = numInputSamples;
    short* tmpInputSamples = inputSamples;
    int samplesDropped = 0;
    while (numInputSamples > 0)
    {
        int samplesToRead = std::min(sampleRate_ / 100, numInputSamples);
        if (tapThreadInput_.write(tmpInputSamples, samplesToRead) != 0)
        {
            samplesDropped += samplesToRead;
        }
        tmpInputSamples += samplesToRead;
        numInputSamples -= samplesToRead;
    }

    // Wake the tap thread as soon as there's a block for it to process. The
    // longer it's left asleep, the less room there is for what arrives before
    // it actually gets to run.
    if (tapThreadInput_.numUsed() >= sampleRate_ / 100)
    {
        sem_.signal();
    }

    // write() is all-or-nothing, so a full FIFO means the tapped step never sees those
    // samples. We can't wait for the tap thread to catch up (this is the audio path), but
    // whatever the tap produces (e.g. a recording) is now missing audio, so say so rather
    // than dropping it quietly. The tap thread is exactly what gets starved first when the
    // machine is loaded.
    if (samplesDropped > 0)
    {
        if (consecutiveDrops_ == 0)
        {
            FREEDV_BEGIN_VERIFIED_SAFE
            log_warn("Tap FIFO full, dropping %d samples (free=%d)", samplesDropped, tapThreadInput_.numFree());
            FREEDV_END_VERIFIED_SAFE
        }
        consecutiveDrops_++;
        samplesDropped_ += samplesDropped;
    }
    else if (consecutiveDrops_ > 0)
    {
        FREEDV_BEGIN_VERIFIED_SAFE
        log_warn("Tap FIFO drops ended after %d call(s); tap is missing %d samples", consecutiveDrops_, samplesDropped_);
        FREEDV_END_VERIFIED_SAFE
        consecutiveDrops_ = 0;
        samplesDropped_ = 0;
    }
 
    return inputSamples;
}
