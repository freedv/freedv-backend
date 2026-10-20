//=========================================================================
// Name:            RADEReceiveStep.cpp
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

#include <math.h>
#include <cassert>
#include <functional>
#include "RADEReceiveStep.h"
#include "lpcnet.h" // from Opus source tree
#include "../util/realtime_fp.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/resource.h>
#endif // defined(__APPLE__)

#include "pipeline_defines.h"

#include "../os/os_interface.h"

#ifndef M_PI
#define M_PI    3.14159265358979323846
#endif // M_PI

using namespace std::chrono_literals;

#if !defined(DISABLE_UNIT_TEST)
#include <string>
extern std::string utRxFeatureFile;
#endif // !defined(DISABLE_UNIT_TEST)

#define FEATURE_FIFO_SIZE ((RADE_SPEECH_SAMPLE_RATE / LPCNET_FRAME_SIZE) * rade_n_features_in_out(dv_))

// reimplementations of below to use RADE_COMP instead of COMP.
inline static RADE_COMP cmult(RADE_COMP a, RADE_COMP b)
{
    RADE_COMP res;

    res.real = a.real*b.real - a.imag*b.imag;
    res.imag = a.real*b.imag + a.imag*b.real;

    return res;
}

inline static float cabsolute(RADE_COMP a)
{
    return sqrtf((a.real * a.real) + (a.imag * a.imag) );
}

static void freq_shift_coh(RADE_COMP rx_fdm_fcorr[], RADE_COMP rx_fdm[], float foff, float Fs, RADE_COMP *foff_phase_rect, int nin) FREEDV_NONBLOCKING
{
    RADE_COMP  foff_rect;
    float mag;
    int   i;

    foff_rect.real = cosf(2.0*M_PI*foff/Fs);
    foff_rect.imag = sinf(2.0*M_PI*foff/Fs);
    for(i=0; i<nin; i++) {
        *foff_phase_rect = cmult(*foff_phase_rect, foff_rect);
        if (foff != 0.f)
            rx_fdm_fcorr[i] = cmult(rx_fdm[i], *foff_phase_rect);
        else
            rx_fdm_fcorr[i] = rx_fdm[i];
    }

    /* normalise digital oscillator as the magnitude can drift over time */

    mag = cabsolute(*foff_phase_rect);
    foff_phase_rect->real /= mag;
    foff_phase_rect->imag /= mag;
}

RADEReceiveStep::RADEReceiveStep(
    struct rade* dv, FARGANState* fargan, rade_text_t textPtr, realtime_fp<void(RADEReceiveStep*)> const& syncFn,
    realtime_fp<float()> const& freqOffsetFn)
    : dv_(dv)
    , fargan_(fargan)
    , inputSampleFifo_(rade_nin_max(dv) + 1)
    , pendingFeatures_(nullptr)
    , pendingFeaturesIdx_(0)
    , featuresFile_(nullptr)
    , textPtr_(textPtr)
    , syncFn_(syncFn)
    , exitingFeatureThread_(false)
    , freqOffsetFn_(freqOffsetFn)
{
    assert(syncState_.is_lock_free());

#if !defined(DISABLE_UNIT_TEST)
    if (utRxFeatureFile != "")
    {
        utFeatures_ = new GenericFIFO<float>(FEATURE_FIFO_SIZE);
        assert(utFeatures_ != nullptr);

        featuresFile_ = fopen((const char*)utRxFeatureFile.c_str(), "wb");
        assert(featuresFile_ != nullptr);
        
        utFeatureThread_ = std::thread(std::bind(&RADEReceiveStep::utFeatureThreadEntry_, this));
    }
#endif // !defined(DISABLE_UNIT_TEST)

    // Pre-allocate buffers so we don't have to do so during real-time operation.
    outputSamples_ = std::make_unique<short[]>(RADE_SPEECH_SAMPLE_RATE);
    assert(outputSamples_ != nullptr);

    inputBufCplx_ = new RADE_COMP[rade_nin_max(dv_)];
    assert(inputBufCplx_ != nullptr);

    inputBuf_ = new short[rade_nin_max(dv_)];
    assert(inputBuf_ != nullptr);

    featuresOut_ = new float[rade_n_features_in_out(dv_)];
    assert(featuresOut_ != nullptr);

    pendingFeatures_ = new float[NB_TOTAL_FEATURES];
    assert(pendingFeatures_ != nullptr);

    rxFdmOffset_ = new RADE_COMP[rade_nin_max(dv_)];
    assert(rxFdmOffset_ != nullptr);
    
    rxFreqOffsetPhaseRectObjs_.real = cos(0.0);
    rxFreqOffsetPhaseRectObjs_.imag = sin(0.0);
}

RADEReceiveStep::~RADEReceiveStep()
{
    delete[] rxFdmOffset_;
    delete[] inputBuf_;
    delete[] inputBufCplx_;
    delete[] featuresOut_;
    delete[] pendingFeatures_;
    outputSamples_ = nullptr;

    if (featuresFile_ != nullptr)
    {
        exitingFeatureThread_.store(true, std::memory_order_release);
        featuresAvailableSem_.signal();
        utFeatureThread_.join();
        fclose(featuresFile_);

        delete utFeatures_;
    }
}

int RADEReceiveStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return RADE_MODEM_SAMPLE_RATE;
}

int RADEReceiveStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return RADE_SPEECH_SAMPLE_RATE;
}

short* RADEReceiveStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    auto maxSamples = getOutputSampleRate();
    *numOutputSamples = 0;

    int   nout = 0;
    short* outputSamplePtr = outputSamples_.get();
    while ((*numOutputSamples + LPCNET_FRAME_SIZE) < maxSamples)
    {
        FREEDV_BEGIN_VERIFIED_SAFE 
        int   nin = rade_nin(dv_);
        assert(nin <= rade_nin_max(dv_));
        FREEDV_END_VERIFIED_SAFE

        int samplesToWrite = std::min(inputSampleFifo_.numFree(), numInputSamples);
        if (samplesToWrite > 0)
        {
            inputSampleFifo_.write(inputSamples, samplesToWrite);
            numInputSamples -= samplesToWrite;
            inputSamples += samplesToWrite;
        }

        if (inputSampleFifo_.read(inputBuf_, nin) != 0 && numInputSamples == 0)
        {
            break;
        }

        // demod per frame processing
        for(int i=0; i<nin; i++)
        {
            inputBufCplx_[i].real = inputBuf_[i] * (2.0f / RADE_INT16_SCALE);
            inputBufCplx_[i].imag = 0.0;
        }

        // Optional frequency shifting
        auto offset = freqOffsetFn_();
        freq_shift_coh(rxFdmOffset_, inputBufCplx_, offset, RADE_MODEM_SAMPLE_RATE, &rxFreqOffsetPhaseRectObjs_, nin);
        
        // RADE processing (input signal->features).
        int hasEooOut = 0;

        FREEDV_BEGIN_VERIFIED_SAFE
            nout = rade_rx(dv_, featuresOut_, &hasEooOut, nullptr, rxFdmOffset_);
        FREEDV_END_VERIFIED_SAFE

        if (nout > 0 && textPtr_ != nullptr)
        {
            float dataSym = 0.0f;
            FREEDV_BEGIN_VERIFIED_SAFE
                dataSym = rade_rx_get_data_symbol(dv_);

                // Feed the streamed data symbol (~25 bits/s) into the text decoder.
                rade_text_rx_symbol(textPtr_, dataSym);

            FREEDV_END_VERIFIED_SAFE
        }

        if (!hasEooOut)
        {
            if (featuresFile_)
            {
                utFeatures_->write(featuresOut_, nout);
                if (utFeatures_->numUsed() > (0.75 * utFeatures_->capacity()))
                {
                    featuresAvailableSem_.signal();
                }
            }

            for (int i = 0; i < nout; i++)
            {
                pendingFeatures_[pendingFeaturesIdx_++] = featuresOut_[i];
                if (pendingFeaturesIdx_ == NB_TOTAL_FEATURES)
                {
                    pendingFeaturesIdx_ = 0;

                    // FARGAN processing (features->analog audio)
                    float fpcm[LPCNET_FRAME_SIZE];
                    short pcm[LPCNET_FRAME_SIZE];
                    FREEDV_BEGIN_VERIFIED_SAFE 
                    fargan_synthesize(fargan_, fpcm, pendingFeatures_);
                    FREEDV_END_VERIFIED_SAFE 

                    for (int i = 0; i < LPCNET_FRAME_SIZE; i++) 
                    {
                        pcm[i] = (int)floor(.5 + std::min(32767.f, std::max(-32767.f, 32768.f*fpcm[i])));
                    }

                    memcpy(outputSamplePtr, pcm, sizeof(short) * LPCNET_FRAME_SIZE);
                    *numOutputSamples += LPCNET_FRAME_SIZE;
                    outputSamplePtr += LPCNET_FRAME_SIZE;
                }
            }
        }
    }
  
    int sync = 0;
    int snr = 0;

    FREEDV_BEGIN_VERIFIED_SAFE
        sync = rade_sync(dv_);
    FREEDV_END_VERIFIED_SAFE

    int prevSync = syncState_.exchange(sync, std::memory_order_acq_rel);

    // Position within the text transmitter's cycle is lost along with sync,
    // so previously received symbols can no longer be combined with new ones.
    if (prevSync && !sync && textPtr_ != nullptr)
    {
        FREEDV_BEGIN_VERIFIED_SAFE
            rade_text_rx_reset(textPtr_);
        FREEDV_END_VERIFIED_SAFE
    }
    
    // In RADEV1, SNR is only valid when in sync. It cannot be assumed
    // that the SNR remains valid when not in sync (for instance, we
    // can't assume it was the last SNR prior to losing sync). Thus,
    // don't pass it up to higher layers if not previously determined
    // that we are in sync.
    if (sync)
    {
        FREEDV_BEGIN_VERIFIED_SAFE
            snr = rade_snrdB_3k_est(dv_);
        FREEDV_END_VERIFIED_SAFE

        snr_.store(snr, std::memory_order_release);
    }

    syncFn_(this);

    return outputSamples_.get();
}

void RADEReceiveStep::reset() FREEDV_NONBLOCKING
{
    inputSampleFifo_.reset();
    pendingFeaturesIdx_ = 0;
}

void RADEReceiveStep::utFeatureThreadEntry_()
{
#if defined(__APPLE__)
    // Downgrade thread QoS to Utility to avoid thread contention issues.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    
    // Make sure other I/O can throttle us.
    setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
#endif // defined(__APPLE__)

    SetThreadName("RADERx");

    float* featureBuf = new float[utFeatures_->capacity()];
    assert(featureBuf != nullptr);

    while (!exitingFeatureThread_.load(std::memory_order_acquire))
    {
        auto numToRead = std::min(utFeatures_->numUsed(), utFeatures_->capacity());
        while (numToRead > 0)
        {
            utFeatures_->read(featureBuf, numToRead);
            fwrite(featureBuf, sizeof(float) * numToRead, 1, featuresFile_);
            numToRead = std::min(utFeatures_->numUsed(), utFeatures_->capacity());
        }
        
        featuresAvailableSem_.wait();
    }
    
    auto numToRead = std::min(utFeatures_->numUsed(), utFeatures_->capacity());
    while (numToRead > 0)
    {
        utFeatures_->read(featureBuf, numToRead);
        fwrite(featureBuf, sizeof(float) * numToRead, 1, featuresFile_);
        numToRead = std::min(utFeatures_->numUsed(), utFeatures_->capacity());
    }
    
    delete[] featureBuf;
}
