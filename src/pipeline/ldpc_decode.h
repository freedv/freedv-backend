//==========================================================================
// Name:            ldpc_decode.h
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

#ifndef LDPC_DECODE_H
#define LDPC_DECODE_H

#include <array>
#include <cstdint>

struct LDPCDecodeResult {
    std::array<uint8_t, 112> message{}; // decoded message bits (0 or 1)
    bool  converged;                    // true if all parity checks are satisfied
    int   iterations;                   // number of BP iterations performed
};

// Soft-decision LDPC(112,56) decoder using sum-product belief propagation.
//
// BPSK bit mapping (one real symbol per bit):
//   bit k -> sym[k]
//   s = +a  ->  bit 0
//   s = -a  ->  bit 1
//
// LLR model (AWGN with known fading):
//   LLR = 2 * amplitude * received_symbol / noise_var
//   positive LLR => bit is more likely 0
//   negative LLR => bit is more likely 1
//
// Parameters:
//   syms       - 112 received BPSK symbols
//   amplitudes - per-symbol channel fading amplitude (use 1.0 for flat/unfaded channel)
//   noise_var  - noise variance per symbol (sigma^2 of the AWGN)
//   max_iter   - maximum belief-propagation iterations (default 30)
LDPCDecodeResult ldpc_decode(const float* syms,
                              const float* amplitudes,
                              float        noise_var,
                              int          max_iter = 30);

// Compute 112 channel LLRs from 112 received BPSK symbols.
//
// For BPSK the MAP log-likelihood ratio has an exact closed form (unlike
// higher-order constellations, no log-sum-exp approximation is needed since
// each bit value corresponds to exactly one constellation point):
//
//   LLR = 2 * a · r / σ²
//
// BPSK constellation and bit mapping:
//   s = +a  ->  bit 0
//   s = -a  ->  bit 1
//
// Sign convention: positive LLR => bit more likely 0,
//                 negative LLR => bit more likely 1.
//
// Parameters:
//   syms       - 112 received BPSK symbols
//   amplitudes - per-symbol channel fading amplitude (use 1.0 for flat channel)
//   noise_var  - noise variance per symbol (sigma^2 of the AWGN)
//   llr_out    - caller-allocated output buffer of 112 floats
void ldpc_linear_log_map(const float* syms,
                         const float* amplitudes,
                         float        noise_var,
                         float*       llr_out);

#endif // LDPC_DECODE_H
