//=========================================================================
// Name:            RADETransmitStep.cpp
// Purpose:         Describes a modulation step in the audio pipeline.
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

#include <cstring>
#include <cassert>
#include <cmath>
#include <functional>
#include "RADETransmitStep.h"
#include "pipeline_defines.h"
#include "../util/logging/ulog.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/resource.h>
#endif // defined(__APPLE__)

#include "../os/os_interface.h"

extern "C" 
{
    #include "fargan_config.h"
    #include "../silk/structs.h"
    #include "osce_features.h"
    #include "osce_structs.h"
    #include "osce.h"
}

using namespace std::chrono_literals;

#define FEATURE_FIFO_SIZE ((RADE_SPEECH_SAMPLE_RATE / LPCNET_FRAME_SIZE) * rade_n_features_in_out(dv_))

const int RADE_SCALING_FACTOR = 16383;

// Additional silence added at the end of the EOO block to ensure that it actually gets
// transmitted out over the air. This was determined experimentally using the FlexRadio
// waveform and OTA testing to be 200ms. Other radios (especially ones directly connected
// to a PC) may not need as long.
const int NUM_SAMPLES_SILENCE = 200 * RADE_MODEM_SAMPLE_RATE / 1000;

#if !defined(DISABLE_UNIT_TEST)
#include <string>
extern std::string utTxFeatureFile;
#endif // !defined(DISABLE_UNIT_TEST)

int RADETransmitStep::eooLengthInSamples() const FREEDV_NONBLOCKING
{
    FREEDV_BEGIN_VERIFIED_SAFE
        return rade_n_tx_eoo_out(dv_) + NUM_SAMPLES_SILENCE;
    FREEDV_END_VERIFIED_SAFE
}

RADETransmitStep::RADETransmitStep(struct rade* dv, LPCNetEncState* encState)
    : dv_(dv)
    , encState_(encState)
    , inputSampleFifo_(LPCNET_FRAME_SIZE + 1)
    , featureList_(nullptr)
    , featureListIdx_(0)
    , featuresFile_(nullptr)
    , exitingFeatureThread_(false)
{
#if !defined(DISABLE_UNIT_TEST)
    if (utTxFeatureFile != "")
    {
        utFeatures_ = new GenericFIFO<float>(FEATURE_FIFO_SIZE);
        assert(utFeatures_ != nullptr);

        featuresFile_ = fopen((const char*)utTxFeatureFile.c_str(), "wb");
        assert(featuresFile_ != nullptr);
        
        utFeatureThread_ = std::thread(std::bind(&RADETransmitStep::utFeatureThreadEntry_, this));
    }
#endif // !defined(DISABLE_UNIT_TEST)

    // Pre-allocate buffers so we don't have to do so during real-time operation.
    auto maxSamples = RADE_MODEM_SAMPLE_RATE;
    outputSamples_ = std::make_unique<short[]>(maxSamples);
    assert(outputSamples_ != nullptr);

    int numOutputSamples = rade_n_tx_out(dv_);

    radeOut_ = new RADE_COMP[numOutputSamples];
    assert(radeOut_ != nullptr);

    radeOutShort_ = new short[numOutputSamples];
    assert(radeOutShort_ != nullptr);

    int numEOOSamples = rade_n_tx_eoo_out(dv_);

    eooOut_ = new RADE_COMP[numEOOSamples];
    assert(eooOut_ != nullptr);

    eooOutShort_ = new short[numEOOSamples + NUM_SAMPLES_SILENCE];
    assert(eooOutShort_ != nullptr);

    featureList_ = new float[rade_n_features_in_out(dv_)];
    assert(featureList_ != nullptr);

    arch_ = opus_select_arch();
}

RADETransmitStep::~RADETransmitStep()
{
    delete[] radeOut_;
    delete[] radeOutShort_;
    delete[] eooOut_;
    delete[] eooOutShort_;
    delete[] featureList_;
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

int RADETransmitStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return RADE_SPEECH_SAMPLE_RATE;
}

int RADETransmitStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return RADE_MODEM_SAMPLE_RATE;
}

short* RADETransmitStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    auto maxSamples = getOutputSampleRate();
    FREEDV_BEGIN_VERIFIED_SAFE
    int numSamplesPerTx = rade_n_tx_out(dv_);
    int numRequiredFeaturesForRADE = rade_n_features_in_out(dv_);
    FREEDV_END_VERIFIED_SAFE

    *numOutputSamples = 0;

    if (numInputSamples == 0)
    {
        // Special case logic for EOO
        *numOutputSamples = outputSampleFifo_.numUsed();
        if (*numOutputSamples > 0)
        {
            outputSampleFifo_.read(outputSamples_.get(), *numOutputSamples);
        }

        return outputSamples_.get();
    }

    while ((*numOutputSamples + numSamplesPerTx) < maxSamples)
    {
        int samplesToWrite = std::min(inputSampleFifo_.numFree(), numInputSamples);
        inputSampleFifo_.write(inputSamples, samplesToWrite);
        numInputSamples -= samplesToWrite;
        inputSamples += samplesToWrite;

        if (inputSampleFifo_.numFree() > 0)
        {
            break;
        }
        else
        {
            short pcm[LPCNET_FRAME_SIZE];
            float features[NB_TOTAL_FEATURES];

            // Feature extraction
            inputSampleFifo_.read(pcm, LPCNET_FRAME_SIZE);
            FREEDV_BEGIN_VERIFIED_SAFE
            lpcnet_compute_single_frame_features(encState_, pcm, features, arch_);
            FREEDV_END_VERIFIED_SAFE
            
            if (featuresFile_)
            {
                utFeatures_->write(features, NB_TOTAL_FEATURES);
                if (utFeatures_->numUsed() > (0.75 * utFeatures_->capacity()))
                {
                    featuresAvailableSem_.signal();
                }
            }
            
            for (int index = 0; index < NB_TOTAL_FEATURES; index++)
            {
                featureList_[featureListIdx_++] = features[index];
                if (featureListIdx_ == numRequiredFeaturesForRADE)
                {
                    featureListIdx_ = 0;

                    // RADE TX handling
                    int numOut = 0;
                    FREEDV_BEGIN_VERIFIED_SAFE
                        numOut = rade_tx(dv_, radeOut_, &featureList_[0]);
                    FREEDV_END_VERIFIED_SAFE

                    for (int index = 0; index < numOut; index++)
                    {
                        // We only need the real component for TX.
                        radeOutShort_[index] = radeOut_[index].real * RADE_SCALING_FACTOR;
                    }
                    outputSampleFifo_.write(radeOutShort_, numOut);
                    *numOutputSamples = outputSampleFifo_.numUsed();
                }
            }
        }
    }

    if (*numOutputSamples > 0)
    {
        outputSampleFifo_.read(outputSamples_.get(), *numOutputSamples);
    }

    return outputSamples_.get();
}

void RADETransmitStep::restartVocoder() FREEDV_NONBLOCKING
{
    // Queues up EOO for return on the next call to this pipeline step.
    FREEDV_BEGIN_VERIFIED_SAFE
    int numEOOSamples = rade_n_tx_eoo_out(dv_);
    FREEDV_END_VERIFIED_SAFE

    FREEDV_BEGIN_VERIFIED_SAFE
        rade_tx_eoo(dv_, eooOut_);
    FREEDV_END_VERIFIED_SAFE

    memset(eooOutShort_, 0, sizeof(short) * (numEOOSamples + NUM_SAMPLES_SILENCE));
    for (int index = 0; index < numEOOSamples; index++)
    {
        eooOutShort_[index] = eooOut_[index].real * RADE_SCALING_FACTOR;
    }

    if (outputSampleFifo_.write(eooOutShort_, numEOOSamples + NUM_SAMPLES_SILENCE) != 0)
    {
        FREEDV_BEGIN_VERIFIED_SAFE
        log_warn("Could not queue EOO samples (remaining space in FIFO = %d)", outputSampleFifo_.numFree());
        FREEDV_END_VERIFIED_SAFE
    }
}

void RADETransmitStep::reset() FREEDV_NONBLOCKING
{
    inputSampleFifo_.reset();
    outputSampleFifo_.reset();
    featureListIdx_ = 0;
}

void RADETransmitStep::utFeatureThreadEntry_()
{
#if defined(__APPLE__)
    // Downgrade thread QoS to Utility to avoid thread contention issues.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    
    // Make sure other I/O can throttle us.
    setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
#endif // defined(__APPLE__)

    SetThreadName("RADETx");

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
