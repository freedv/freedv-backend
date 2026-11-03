//=========================================================================
// Name:            RADEFeatureExpander.h
// Purpose:         Fills in the high band of RADE's input features when
//                  the mic only gives 8 kHz audio.
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

#ifndef AUDIO_PIPELINE__RADE_FEATURE_EXPANDER_H
#define AUDIO_PIPELINE__RADE_FEATURE_EXPANDER_H

#include "../util/freedv_sanitizers.h"

extern "C"
{
    #include "fargan_config.h"
    #include "lpcnet.h"
    #include "rade_bwe_data.h"
}

// Speech from an 8 kHz mic has nothing above 4 kHz, so the features RADE
// encodes from it describe a dull, muffled voice even though RADE could
// carry up to 8 kHz. This runs a small causal model (a convolution and two
// GRUs over the 10 ms feature frames) that guesses the band energies and
// pitch correlation a 16 kHz mic would have given for the same speech. The
// pitch period, and the far end, are unchanged.
//
// The model was trained on paired wideband/narrowband LibriSpeech features,
// with an adversarial loss so that the predicted high band varies like real
// speech rather than being averaged out. tools/rade_bwe has the scripts that
// trained it and generated rade_bwe_data.c.
class RADEFeatureExpander
{
public:
    RADEFeatureExpander();

    // Replaces the band energies and pitch correlation of one frame of
    // features (NB_TOTAL_FEATURES floats, from
    // lpcnet_compute_single_frame_features()) with the model's guess.
    void process(float* features) FREEDV_NONBLOCKING;

    // Forgets the speech seen so far, e.g. on a new transmission.
    void reset() FREEDV_NONBLOCKING;

private:
    RadeBwe model_;
    float convMem_[BWE_CONV_STATE_SIZE];
    float gru1State_[BWE_GRU1_STATE_SIZE];
    float gru2State_[BWE_GRU2_STATE_SIZE];
    bool firstFrame_;
    int arch_;
};

#endif // AUDIO_PIPELINE__RADE_FEATURE_EXPANDER_H
