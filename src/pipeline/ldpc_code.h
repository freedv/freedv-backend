//==========================================================================
// Name:            ldpc_code.h
//
// Purpose:         Generic encode/decode of systematic HRA-style LDPC(N, K)
//                  codewords, parameterized on the parity check matrix.
// Created:         October 4, 2026
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

#ifndef LDPC_CODE_H
#define LDPC_CODE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

template<int N>
struct LDPCDecodeResultN {
    std::array<uint8_t, N> message{}; // decoded codeword bits (0 or 1); first K are the payload
    bool  converged;                  // true if all parity checks are satisfied
    int   iterations;                 // number of BP iterations performed
};

// Systematic LDPC(N, K) code with M = N - K parity bits, defined by a dense
// M x N parity check matrix H = [H_a | H_b] where H_b (the last M columns)
// is lower-bidiagonal (HRA/accumulator structure, as used by every codec2
// LDPC code). This allows parity bits p to be solved from
// H_a*s + H_b*p = 0 (mod 2) via forward substitution:
//   p[0]   = r[0]
//   p[i]   = r[i] XOR p[i-1]   for i = 1..M-1
// where r = H_a * s (mod 2).
//
// Each instance owns its own belief-propagation message buffers, so
// separate instances can decode concurrently; a single instance is not
// thread-safe.
template<int K, int M>
class LDPCCode
{
public:
    static constexpr int DATA_BITS = K;
    static constexpr int PARITY_BITS = M;
    static constexpr int CODEWORD_BITS = K + M;

    using Matrix = uint8_t[M][K + M];
    using Result = LDPCDecodeResultN<K + M>;

    explicit LDPCCode(const Matrix& H)
        : H_(H)
    {
        for (int i = 0; i < M; i++)
            for (int j = 0; j < N; j++)
                if (H[i][j])
                {
                    int e = (int)edges_.size();
                    edges_.push_back({i, j});
                    checkEdges_[i].push_back(e);
                    varEdges_[j].push_back(e);
                }

        mVC_.resize(edges_.size());
        mCV_.resize(edges_.size());
    }

    // Input:  K bits (each element must be 0 or 1)
    // Output: N-bit codeword [ s | p ]
    std::array<uint8_t, K + M> encode(const std::array<uint8_t, K>& s) const
    {
        std::array<uint8_t, N> codeword{};

        // Codeword must satisfy Hc^t = 0. The first K bits of c are known
        // to be s, so we can prepopulate now.
        for (int i = 0; i < K; i++)
        {
            codeword[i] = s[i];
        }

        // Right part of H is lower-bidiagonal. For each parity bit, we use
        // p[index - 1] and p[index] (except for the first parity bit, which
        // is just p[index]).
        for (int i = 0; i < M; i++)
        {
            int parityCtr = 0;
            for (int j = 0; j < K; j++)
            {
                parityCtr += codeword[j] * H_[i][j];
            }
            if (i > 0)
            {
                parityCtr += codeword[K + i - 1];
            }
            codeword[K + i] = (parityCtr % 2) ? 1 : 0;
        }

        return codeword;
    }

    // Soft-decision decode using sum-product belief propagation.
    //
    // BPSK bit mapping (one real symbol per bit):
    //   s = +a  ->  bit 0
    //   s = -a  ->  bit 1
    //
    // Parameters:
    //   syms       - N received BPSK symbols
    //   amplitudes - per-symbol channel fading amplitude (use 1.0 for flat/unfaded channel)
    //   noise_var  - noise variance per symbol (sigma^2 of the AWGN)
    //   max_iter   - maximum belief-propagation iterations
    Result decode(const float* syms, const float* amplitudes, float noise_var, int max_iter = 30)
    {
        if (noise_var < 1e-10f) noise_var = 1e-10f;

        // Compute channel LLRs.
        float llr_ch[N];
        linearLogMap(syms, amplitudes, noise_var, llr_ch);

        // mVC_[e]: variable-to-check message on edge e
        // mCV_[e]: check-to-variable message on edge e
        const int E = (int)edges_.size();
        for (int e = 0; e < E; e++)
        {
            mVC_[e] = llr_ch[edges_[e].var];
            mCV_[e] = 0.0f;
        }

        Result result{};
        result.converged  = false;
        result.iterations = 0;

        for (int iter = 0; iter < max_iter; iter++) {

            // ---- Check-to-variable update (tanh / sum-product rule) ----
            //
            // For check i and its neighbor set N(i), the outgoing message to
            // variable j is:
            //
            //   r_{i→j} = ∏_{j'≠j}(sign(q_j'i)) * phi(sum_{i'!=i}(phi(abs(q_ji')))
            //
            for (int i = 0; i < M; i++) {
                const auto& ce = checkEdges_[i];
                const int   nd = (int)ce.size();

                // phi(|v|) is needed once per edge to build the sum below, and
                // again per edge afterward as part of phi(sum - phi(|v|)) --
                // cache it here instead of recomputing the same exp/log twice.
                float phi_v[N];
                int sign = 1;
                float sum = 0;
                for (int k = 0; k < nd; k++) {
                    const float v = mVC_[ce[k]];
                    sign *= (v >= 0.0f) ? 1 : -1;
                    phi_v[k] = std::max(0.0f, phi_(std::abs(v)));
                    sum += phi_v[k];
                }

                for (int k = 0; k < nd; k++) {
                    const float v = mVC_[ce[k]];
                    int inv_sign = (v >= 0.0f) ? 1 : -1;
                    mCV_[ce[k]] = std::clamp(inv_sign * sign * std::max(0.0f, phi_(sum - phi_v[k])), -LLR_MAX, LLR_MAX);
                }
            }

            // ---- Variable-to-check update ----
            //
            //   q_{j→i} = λ_j + Σ_{i'≠i} r_{i'→j}
            //
            // Computed as (sum of all incoming check messages + channel LLR) minus
            // the one edge being excluded.
            for (int j = 0; j < N; j++) {
                const auto& ve = varEdges_[j];
                float total = llr_ch[j];
                for (int e : ve) total += mCV_[e];
                for (int e : ve)
                    mVC_[e] = std::clamp(total - mCV_[e], -LLR_MAX, LLR_MAX);
            }

            // ---- Posterior LLR, hard decision, syndrome check ----
            for (int j = 0; j < N; j++) {
                float L = llr_ch[j];
                for (int e : varEdges_[j]) L += mCV_[e];
                result.message[j] = (L < 0.0f) ? 1 : 0;
            }

            // bits * H' must equal 0. Walk the sparse edge list instead of
            // scanning the dense matrix -- same result, but touches far
            // fewer entries for a sparse code.
            bool ok = true;
            for (int i = 0; i < M && ok; i++) {
                int ctr = 0;
                for (int e : checkEdges_[i]) {
                    ctr += result.message[edges_[e].var];
                }
                ok = (ctr % 2) == 0; // non-zero check
            }

            result.iterations = iter + 1;
            if (ok) {
                result.converged = true;
                break;
            }
        }

        // Return result, even if not converged.
        return result;
    }

    // Compute N channel LLRs from N received BPSK symbols.
    //
    // For BPSK the MAP log-likelihood ratio has a closed form since each bit
    // value corresponds to exactly one constellation point (no log-sum-exp over
    // multiple candidates is needed, unlike higher-order constellations):
    //
    //   LLR = log( exp(-(r-a)²/(2σ²)) ) - log( exp(-(r+a)²/(2σ²)) )
    //       = 2·a·r / σ²
    //
    // Constellation (amplitude a): s = +a -> bit 0, s = -a -> bit 1.
    // Sign convention: positive LLR => bit more likely 0.
    //
    // The amplitude used above is weighted relative to the mean amplitude across
    // all symbols so that per-symbol fading confidence scales the LLR.
    static void linearLogMap(const float* syms, const float* amplitudes, float noise_var, float* llr_out)
    {
        float mean_amp = 0;
        for (int k = 0; k < N; k++)
        {
            mean_amp += amplitudes[k];
        }
        mean_amp /= N;

        for (int k = 0; k < N; k++) {
            const float rel_amp = amplitudes[k] / mean_amp;
            const float llr = 2.0f * rel_amp * rel_amp * syms[k] / noise_var;
            llr_out[k] = std::clamp(llr, -LLR_MAX, LLR_MAX);
        }
    }

private:
    static constexpr int N = K + M;
    static constexpr float LLR_MAX = 10000.0f;

    struct Edge { int check, var; };

    const Matrix& H_;
    std::vector<Edge> edges_;
    std::vector<int>  checkEdges_[M];   // edge indices per check node
    std::vector<int>  varEdges_[N];     // edge indices per variable node
    std::vector<float> mVC_, mCV_;

    static float phi_(float x)
    {
        if (x < 1e-10f) return LLR_MAX;

        auto expx = std::exp((double)x);
        if (expx < 1e-10f) return LLR_MAX;

        return std::log((expx + 1.0f) / (expx - 1.0f));
    }
};

#endif // LDPC_CODE_H
