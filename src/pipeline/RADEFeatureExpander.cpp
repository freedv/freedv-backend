//=========================================================================
// Name:            RADEFeatureExpander.cpp
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

#include <cassert>
#include <cstring>

#include "RADEFeatureExpander.h"

extern "C"
{
    extern const WeightArray radebwe_arrays[];
}

// Feature layout (see lpcnet.h): NB_BANDS cepstral band energies, then the
// pitch period and the pitch correlation.
#define BWE_NB_BANDS 18
#define BWE_PITCH_CORR (BWE_NB_BANDS + 1)

static_assert(BWE_CONV_IN_SIZE == BWE_NB_BANDS + 2, "model input should be the bands, pitch and correlation");
static_assert(BWE_OUT_OUT_SIZE == BWE_NB_BANDS + 1, "model output should be the bands and correlation");

RADEFeatureExpander::RADEFeatureExpander()
{
    int result = init_radebwe(&model_, radebwe_arrays);
    assert(result == 0);
    (void)result;

    arch_ = opus_select_arch();
    reset();
}

void RADEFeatureExpander::reset() FREEDV_NONBLOCKING
{
    memset(convMem_, 0, sizeof(convMem_));
    memset(gru1State_, 0, sizeof(gru1State_));
    memset(gru2State_, 0, sizeof(gru2State_));
    firstFrame_ = true;
}

void RADEFeatureExpander::process(float* features) FREEDV_NONBLOCKING
{
    float conv[BWE_CONV_OUT_SIZE];
    float out[BWE_OUT_OUT_SIZE];

    // The model was trained with the convolution seeing copies of the first
    // frame before the start of the speech, rather than silence.
    if (firstFrame_)
    {
        for (int index = 0; index < BWE_CONV_STATE_SIZE; index += BWE_CONV_IN_SIZE)
        {
            memcpy(&convMem_[index], features, sizeof(float) * BWE_CONV_IN_SIZE);
        }
        firstFrame_ = false;
    }

    // The input normalization is folded into the convolution's weights.
    FREEDV_BEGIN_VERIFIED_SAFE
    compute_generic_conv1d(&model_.bwe_conv, conv, convMem_, features, BWE_CONV_IN_SIZE, ACTIVATION_TANH, arch_);
    compute_generic_gru(&model_.bwe_gru1_input, &model_.bwe_gru1_recurrent, gru1State_, conv, arch_);
    compute_generic_gru(&model_.bwe_gru2_input, &model_.bwe_gru2_recurrent, gru2State_, gru1State_, arch_);
    compute_generic_dense(&model_.bwe_out, out, gru2State_, ACTIVATION_LINEAR, arch_);
    FREEDV_END_VERIFIED_SAFE

    // The model predicts the difference from the narrowband features.
    for (int index = 0; index < BWE_NB_BANDS; index++)
    {
        features[index] += out[index];
    }
    features[BWE_PITCH_CORR] += out[BWE_NB_BANDS];
}
