//=========================================================================
// Name:            LoudnessMeter.cpp
// Purpose:         Thin libebur128 wrapper for EBU R128 momentary loudness
//                  measurement (mono).
//
// Authors:         Claude Code (for Barry Jackson, G4MKT)
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

#include "LoudnessMeter.h"

#include <cassert>
#include <cmath>

#include "ebur128.h" // from libebur128

LoudnessMeter::LoudnessMeter(int sampleRate)
{
    ebur128State_ = ebur128_init(1, sampleRate, EBUR128_MODE_M);
    assert(ebur128State_ != nullptr);
}

LoudnessMeter::~LoudnessMeter()
{
    ebur128_destroy((ebur128_state**)&ebur128State_);
}

void LoudnessMeter::addFrames(const short* samples, int numSamples) FREEDV_NONBLOCKING
{
    ebur128_state* state = static_cast<ebur128_state*>(ebur128State_);

    // Note: libebur128 is unlikely to use RT-unsafe constructs in normal
    // operation (per existing RTSan-enabled tests). Verified 2025-09-30.
    FREEDV_BEGIN_VERIFIED_SAFE
    ebur128_add_frames_short(state, samples, numSamples);
    FREEDV_END_VERIFIED_SAFE
}

bool LoudnessMeter::getMomentaryLoudness(double* lufsOut, double silenceFloorLufs) const FREEDV_NONBLOCKING
{
    ebur128_state* state = static_cast<ebur128_state*>(ebur128State_);

    double lufs = 0.0;
    int result;
    FREEDV_BEGIN_VERIFIED_SAFE
    result = ebur128_loudness_momentary(state, &lufs);
    FREEDV_END_VERIFIED_SAFE

    if (result != EBUR128_SUCCESS || lufs == -HUGE_VAL || lufs <= silenceFloorLufs)
    {
        return false;
    }

    *lufsOut = lufs;
    return true;
}

void LoudnessMeter::reset() FREEDV_NONBLOCKING
{
    // Intentional no-op -- see header comment.
}
