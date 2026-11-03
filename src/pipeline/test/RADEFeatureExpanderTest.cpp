//==========================================================================
// Name:            RADEFeatureExpanderTest.cpp
//
// Purpose:         Checks the C version of the 8 kHz-mic feature predictor
//                  against reference frames from the PyTorch model.
// Created:         October 31, 2026
// Authors:         Mooneer Salem
//
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
//==========================================================================

#include "../RADEFeatureExpander.h"
#include "rade_bwe_test_vectors.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define PITCH_INDEX 18

// Opus's nnet code uses fast approximations of tanh and sigmoid, so the C
// version doesn't match PyTorch exactly. On x86 the difference is a few
// thousandths at most. The NEON versions are coarser (tanh is off by up to
// about 3e-3), and through the GRUs that gives up to 0.07 on a few frames,
// with an RMS of 0.005. Features are a DCT of log10 band energies, so 0.1 is
// about 0.3 dB in any band, against the model's own error of several dB. A
// wrong weight or gate order is off by more than that from the first frame.
static constexpr float MAX_TOLERANCE = 0.1f;
static constexpr float RMS_TOLERANCE = 0.01f;

struct Errors
{
    float max;
    float rms;
};

static Errors runFrames(RADEFeatureExpander& expander, int numFrames)
{
    Errors errors = {0, 0};
    for (int frame = 0; frame < numFrames; frame++)
    {
        float features[NB_TOTAL_FEATURES] = {0};
        memcpy(features, bwe_test_input[frame], sizeof(float) * BWE_TEST_FEATURES);
        expander.process(features);
        for (int index = 0; index < BWE_TEST_FEATURES; index++)
        {
            float error = features[index] - bwe_test_output[frame][index];
            errors.max = std::fmax(errors.max, std::fabs(error));
            errors.rms += error * error;
        }
    }
    errors.rms = std::sqrt(errors.rms / (numFrames * BWE_TEST_FEATURES));
    return errors;
}

static bool withinTolerance(Errors errors)
{
    return errors.max < MAX_TOLERANCE && errors.rms < RMS_TOLERANCE;
}

static bool test1_matches_reference()
{
    printf("test1_matches_reference: ");
    RADEFeatureExpander expander;
    Errors errors = runFrames(expander, BWE_TEST_FRAMES);
    bool ok = withinTolerance(errors);
    printf("max error %g, RMS %g over %d frames: %s\n", errors.max, errors.rms, BWE_TEST_FRAMES, ok ? "PASS" : "FAIL");
    return ok;
}

static bool test2_reset_starts_over()
{
    printf("test2_reset_starts_over: ");
    RADEFeatureExpander expander;
    runFrames(expander, BWE_TEST_FRAMES / 2);
    expander.reset();
    Errors errors = runFrames(expander, BWE_TEST_FRAMES);
    bool ok = withinTolerance(errors);
    printf("max error %g, RMS %g after reset: %s\n", errors.max, errors.rms, ok ? "PASS" : "FAIL");
    return ok;
}

static bool test3_leaves_pitch_and_lpc_alone()
{
    printf("test3_leaves_pitch_and_lpc_alone: ");
    RADEFeatureExpander expander;
    bool ok = true;
    for (int frame = 0; frame < BWE_TEST_FRAMES; frame++)
    {
        float features[NB_TOTAL_FEATURES];
        for (int index = 0; index < NB_TOTAL_FEATURES; index++)
        {
            features[index] = 100.0f + index;
        }
        memcpy(features, bwe_test_input[frame], sizeof(float) * BWE_TEST_FEATURES);
        float pitch = features[PITCH_INDEX];
        expander.process(features);
        ok &= features[PITCH_INDEX] == pitch;
        for (int index = BWE_TEST_FEATURES; index < NB_TOTAL_FEATURES; index++)
        {
            ok &= features[index] == 100.0f + index;
        }
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main()
{
    bool success = true;

    success &= test1_matches_reference();
    success &= test2_reset_starts_over();
    success &= test3_leaves_pitch_and_lpc_alone();

    printf("=== Overall: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
