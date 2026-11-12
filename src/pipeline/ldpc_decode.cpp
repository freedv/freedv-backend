//==========================================================================
// Name:            ldpc_decode.cpp
//
// Purpose:         Handles decode of LDPC(112, 56) codewords.
// Created:         May 20, 2026
// Authors:         Mooneer Salem
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

#include "ldpc_decode.h"
#include "ldpc_code.h"
#include "HRA_56_56.h"

// Shared by all callers of the free functions below (not thread-safe, as
// before). Code that needs its own decoder state should instantiate
// LDPCCode directly.
static LDPCCode<56, 56> code_(HRA_56_56);

LDPCDecodeResult ldpc_decode(const float* syms,
                              const float* amplitudes,
                              float        noise_var,
                              int          max_iter)
{
    auto r = code_.decode(syms, amplitudes, noise_var, max_iter);

    LDPCDecodeResult result{};
    result.message = r.message;
    result.converged = r.converged;
    result.iterations = r.iterations;
    return result;
}

void ldpc_linear_log_map(const float* syms,
                         const float* amplitudes,
                         float        noise_var,
                         float*       llr_out)
{
    LDPCCode<56, 56>::linearLogMap(syms, amplitudes, noise_var, llr_out);
}
