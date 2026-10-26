//=========================================================================
// Name:            DiagnosticCsvLogger.h
// Purpose:         Diagnostic-only CSV logger for the leveler/limiter
//                  pipeline step.
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

#ifndef UTIL__DIAGNOSTIC_CSV_LOGGER_H
#define UTIL__DIAGNOSTIC_CSV_LOGGER_H

#include <cstdio>
#include <chrono>

#include "freedv_sanitizers.h"

// Diagnostic only, not for production use. Opens and writes ~/agc_diag.csv
// only when built with -DENABLE_AUDIO_DIAG_LOGGING=ON (see the top-level
// CMakeLists.txt); otherwise every call is a cheap no-op. AgcStep takes
// an optional instance (nullptr means no logging at all).
//
// Writes one row per ~10ms processing chunk.
class DiagnosticCsvLogger
{
public:
    DiagnosticCsvLogger();
    ~DiagnosticCsvLogger();

    // appliedGainDb differs from currentGainDb only during the leveler's
    // startup ramp-in (see STARTUP_RAMP_SEC in AgcStep.cpp).
    void logChunk(double inputDbfs, double feedbackLufs, double targetGainDb, double currentGainDb, double appliedGainDb,
                  double gainReductionDb, double outputDbfs) FREEDV_NONBLOCKING;

private:
    FILE* file_;
    std::chrono::steady_clock::time_point startTime_;
};

#endif // UTIL__DIAGNOSTIC_CSV_LOGGER_H
