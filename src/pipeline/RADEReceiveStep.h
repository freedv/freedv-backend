//=========================================================================
// Name:            RADEReceiveStep.h
// Purpose:         Describes a demodulation step in the audio pipeline.
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

#ifndef AUDIO_PIPELINE__RADE_RECEIVE_STEP_H
#define AUDIO_PIPELINE__RADE_RECEIVE_STEP_H

#include <atomic>
#include <cstdio>
#include <thread>
#include <functional>
#include <cmath>

extern "C"
{
    #include "fargan_config.h"
}

#include "IPipelineStep.h"
#include "rade_api.h"
#include "rade_text.h"
#include "../util/GenericFIFO.h"
#include "../util/Semaphore.h"
#include "../util/realtime_fp.h"

// TBD - need to wrap in "extern C" to avoid linker errors
extern "C"
{
    #include "fargan.h"
    #include "lpcnet.h"
}

class RADEReceiveStep : public IPipelineStep
{
public:
    RADEReceiveStep(
        struct rade* dv, 
        FARGANState* fargan, 
        rade_text_t textPtr, 
        realtime_fp<void(RADEReceiveStep*)> const& syncFn,
        realtime_fp<float()> const& freqOffsetFn);
    virtual ~RADEReceiveStep();
    
    virtual int getInputSampleRate() const FREEDV_NONBLOCKING override;
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING override;
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING override;
    virtual void reset() FREEDV_NONBLOCKING override;
    
    int getSync() const { return syncState_.load(std::memory_order_acquire); }
    int getSnr() const { return snr_.load(std::memory_order_acquire); }
    realtime_fp<std::atomic<int>*()> getRxStateFn() { return rxStateFn_; }
    void setRxStateFn(realtime_fp<std::atomic<int>*()> const& rxFn) { rxStateFn_ = rxFn; }
    void* getStateObj() const { return stateObj_; }
    void setStateObj(void* state) { stateObj_ = state; }

private:
    std::atomic<int> syncState_;
    std::atomic<int> snr_;
    struct rade* dv_;
    FARGANState* fargan_;
    GenericFIFO<short> inputSampleFifo_;
    float* pendingFeatures_;
    int pendingFeaturesIdx_;
    FILE* featuresFile_;
    rade_text_t textPtr_;
    realtime_fp<void(RADEReceiveStep*)> syncFn_;
    RADE_COMP* inputBufCplx_;
    short* inputBuf_;
    float* featuresOut_;
    std::unique_ptr<short[]> outputSamples_;
    
    GenericFIFO<float>* utFeatures_;
    std::thread utFeatureThread_;
    std::atomic<bool> exitingFeatureThread_;
    Semaphore featuresAvailableSem_;
   
    realtime_fp<std::atomic<int>*()> rxStateFn_;
    void* stateObj_;
    
    realtime_fp<float()> freqOffsetFn_;
    RADE_COMP rxFreqOffsetPhaseRectObjs_;
    RADE_COMP* rxFdmOffset_;

    void utFeatureThreadEntry_();
};

#endif // AUDIO_PIPELINE__RADE_RECEIVE_STEP_H
