//=========================================================================
// Name:            DiagnosticCsvLogger.cpp
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

#include "DiagnosticCsvLogger.h"

#include <cstdlib>
#include <string>

DiagnosticCsvLogger::DiagnosticCsvLogger()
    : file_(nullptr)
{
#if defined(FREEDV_ENABLE_AUDIO_DIAG_LOGGING)
    const char* home = std::getenv("HOME");
    if (home != nullptr)
    {
        std::string path = std::string(home) + "/agc_diag.csv";
        file_ = fopen(path.c_str(), "w");
        if (file_ != nullptr)
        {
            fprintf(file_, "elapsed_ms,input_dbfs,feedback_lufs,leveler_target_gain_db,leveler_current_gain_db,leveler_applied_gain_db,comp_limiter_gain_reduction_db,output_dbfs\n");
            fflush(file_);
        }
    }
#endif // defined(FREEDV_ENABLE_AUDIO_DIAG_LOGGING)

    startTime_ = std::chrono::steady_clock::now();
}

DiagnosticCsvLogger::~DiagnosticCsvLogger()
{
    if (file_ != nullptr)
    {
        fclose(file_);
        file_ = nullptr;
    }
}

void DiagnosticCsvLogger::logChunk(double inputDbfs, double feedbackLufs, double targetGainDb, double currentGainDb, double appliedGainDb,
                                   double gainReductionDb, double outputDbfs) FREEDV_NONBLOCKING
{
    if (file_ == nullptr) return;

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime_).count();

    // Blocking file I/O in the audio thread is accepted here only because
    // this is diagnostic-only and compiled in only with
    // ENABLE_AUDIO_DIAG_LOGGING.
    FREEDV_BEGIN_VERIFIED_SAFE
    fprintf(file_, "%lld,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
        (long long)elapsedMs, inputDbfs, feedbackLufs,
        targetGainDb, currentGainDb, appliedGainDb, gainReductionDb, outputDbfs);
    fflush(file_);
    FREEDV_END_VERIFIED_SAFE
}
