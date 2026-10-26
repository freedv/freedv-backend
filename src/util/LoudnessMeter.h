//=========================================================================
// Name:            LoudnessMeter.h
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

#ifndef UTIL__LOUDNESS_METER_H
#define UTIL__LOUDNESS_METER_H

#include "freedv_sanitizers.h"

// Mono EBU R128 momentary loudness meter (K-weighted, gated, over the
// last 400ms -- see libebur128). Used by AgcStep to measure
// its own output for the leveler's feedback loop.
class LoudnessMeter
{
public:
    LoudnessMeter(int sampleRate);
    ~LoudnessMeter();

    void addFrames(const short* samples, int numSamples) FREEDV_NONBLOCKING;

    // Returns true and writes *lufsOut if a valid momentary reading is
    // available, i.e. there is enough data and the level is above
    // silenceFloorLufs. Returns false (leaving *lufsOut untouched) on
    // silence or before the first 400ms window has filled.
    bool getMomentaryLoudness(double* lufsOut, double silenceFloorLufs = -70.0) const FREEDV_NONBLOCKING;

    // No-op: libebur128 has no way to clear its history without
    // destroying and reinitialising its state, both of which allocate.
    void reset() FREEDV_NONBLOCKING;

private:
    void* ebur128State_;
};

#endif // UTIL__LOUDNESS_METER_H
