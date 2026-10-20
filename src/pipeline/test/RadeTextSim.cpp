//==========================================================================
// Name:            RadeTextSim.cpp
//
// Purpose:         Monte Carlo evaluation of rade_text receive combining.
//                  Not a pass/fail test -- prints CSV for offline analysis.
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
//
// Channel model: the RADE V2 auxiliary data channel delivers one real
// soft-decision BPSK symbol per 40ms modem frame (25 symbols/s), so the
// simulation works at that level rather than running the full modem:
//
//   y[k] = |h[k]| * s[k] + n[k],   s = +/-1,  n ~ N(0, sigma^2)
//   Es/N0 = E|h|^2 / (2 sigma^2)   (real part of complex AWGN)
//
// where h is either 1 (AWGN) or a unit-power Rayleigh process with the
// given Doppler spread (Clarke sum-of-sinusoids), sampled at 25 Hz.
//
// Modes (first argument):
//
//   codeword  Ideal codeword alignment. Compares, at equal Es/N0, the
//             codeword error rate of one LDPC(112,56) codeword against two
//             copies soft-combined (224 symbols). Uses the same
//             interleaver, LLR normalization, noise estimate and
//             10-iteration cap as rade_text, plus an "ideal" receiver for
//             reference. Also times decode attempts.
//
//   e2e       End to end through the real rade_text TX/RX code: receiver
//             joins at a random point in the TX cycle and the time until
//             the correct callsign is first delivered is recorded.
//
//   noise     Feeds pure noise to the receiver and counts (false) decodes.
//
// e2e and noise take optional 0/1 arguments selecting whether the
// receiver combines repeated copies of the TX cycle (default 1) and whether
// combined-only decodes must be confirmed (default 0).
//
//==========================================================================

#include "../rade_text.h"
#include "../ldpc_code.h"
#include "../HRA_56_56.h"
#include "../../util/logging/ulog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

static constexpr float SYMBOL_RATE = 25.0f;
static constexpr int CODEWORD_SYMS = 112; // LDPC(112,56)

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

class FadingChannel
{
public:
    // dopplerHz <= 0 means AWGN only (|h| == 1).
    FadingChannel(float dopplerHz, std::mt19937& rng)
        : dopplerHz_(dopplerHz), k_(0)
    {
        std::uniform_real_distribution<float> u(0.0f, 2.0f * (float)M_PI);
        for (int i = 0; i < NUM_PATHS; i++)
        {
            // Clarke/Jakes-style: random arrival angle and phase per path,
            // independently for I and Q.
            thetaI_[i] = u(rng); phiI_[i] = u(rng);
            thetaQ_[i] = u(rng); phiQ_[i] = u(rng);
        }
    }

    float nextGain()
    {
        if (dopplerHz_ <= 0.0f) return 1.0f;

        float t = (float)k_++ / SYMBOL_RATE;
        float hi = 0, hq = 0;
        for (int i = 0; i < NUM_PATHS; i++)
        {
            hi += std::cos(2.0f * (float)M_PI * dopplerHz_ * std::cos(thetaI_[i]) * t + phiI_[i]);
            hq += std::cos(2.0f * (float)M_PI * dopplerHz_ * std::cos(thetaQ_[i]) * t + phiQ_[i]);
        }
        // Each sum has variance NUM_PATHS/2, so E|h|^2 = 1 after scaling.
        float scale = 1.0f / std::sqrt((float)NUM_PATHS);
        return std::sqrt(hi * hi + hq * hq) * scale;
    }

private:
    static constexpr int NUM_PATHS = 16;
    float dopplerHz_;
    long k_;
    float thetaI_[NUM_PATHS], phiI_[NUM_PATHS], thetaQ_[NUM_PATHS], phiQ_[NUM_PATHS];
};

static float sigmaForEsNo(float esnoDb)
{
    float esno = std::pow(10.0f, esnoDb / 10.0f);
    return std::sqrt(1.0f / (2.0f * esno));
}

struct ChannelDef { const char* name; float dopplerHz; };
static const ChannelDef CHANNELS[] = {
    {"awgn", 0.0f},
    {"rayleigh_0.1Hz", 0.1f},
    {"rayleigh_1Hz", 1.0f},
};

// Runs fn(trialIndex, rng) for trials in parallel across all cores.
template<typename Fn>
static void parallelFor(int trials, unsigned seed, Fn fn)
{
    unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> next(0);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < nthreads; t++)
    {
        threads.emplace_back([&, t]() {
            int i;
            while ((i = next++) < trials)
            {
                std::mt19937 rng(seed * 7919u + (unsigned)i * 104729u + 17u);
                fn(i, rng);
            }
        });
    }
    for (auto& th : threads) th.join();
}

// ---------------------------------------------------------------------------
// Codeword-level mode. Mirrors rade_text's RX processing for an aligned
// window (see rade_text_try_decode_block_() and rade_text_ldpc_decode()).
// ---------------------------------------------------------------------------

static constexpr int INTERLEAVER_B = 37;
static constexpr int MAX_CONFIDENT_ITERATIONS = 10;

static float estimateNoiseVar(const float *window, int n)
{
    float meanAmp = 0.0f;
    for (int i = 0; i < n; i++) meanAmp += std::fabs(window[i]);
    meanAmp /= n;

    float var = 0.0f;
    for (int i = 0; i < n; i++)
    {
        float d = std::fabs(window[i]) - meanAmp;
        var += d * d;
    }
    var /= std::max(n - 1, 1);
    return std::max(var, 1e-6f);
}

template<typename Code>
static std::vector<float> encodeInterleaveBpsk(Code& code, const std::array<uint8_t, 56>& msg)
{
    constexpr int N = Code::CODEWORD_BITS;
    auto cw = code.encode(msg);
    std::vector<float> tx(N);
    for (int i = 0; i < N; i++)
    {
        tx[(INTERLEAVER_B * i) % N] = cw[i] ? -1.0f : 1.0f;
    }
    return tx;
}

// Returns true if window (N received symbols, still interleaved) decodes to msg.
//
// trueSigma2 <= 0 reproduces rade_text's receiver exactly (|y|-weighted
// LLRs, estimated noise variance, MAX_CONFIDENT_ITERATIONS cap). Otherwise
// an "ideal" receiver is used instead -- standard AWGN LLRs 2y/sigma^2
// with the true noise variance and up to 50 iterations -- to separate the
// codes' own performance from rade_text's receiver simplifications.
template<typename Code>
static bool decodeWindow(Code& code, const float* window, const std::array<uint8_t, 56>& msg, int* iters = nullptr, float trueSigma2 = 0.0f)
{
    constexpr int N = Code::CODEWORD_BITS;
    float syms[N], amps[N];
    for (int i = 0; i < N; i++)
    {
        float s = window[(INTERLEAVER_B * i) % N];
        if (trueSigma2 > 0.0f)
        {
            amps[i] = 1.0f;
            syms[i] = s;
        }
        else
        {
            amps[i] = std::fabs(s);
            syms[i] = s / amps[i];
        }
    }
    float sigma2 = trueSigma2 > 0.0f ? trueSigma2 : estimateNoiseVar(window, N);
    auto r = code.decode(syms, amps, sigma2, trueSigma2 > 0.0f ? 50 : MAX_CONFIDENT_ITERATIONS);
    if (iters) *iters = r.iterations;
    if (!r.converged) return false;
    for (int i = 0; i < 56; i++)
        if (r.message[i] != msg[i]) return false;
    return true;
}

static void runCodewordMode(int trials)
{
    printf("channel,esno_db,cer_112,cer_112x2,ideal_112,ideal_112x2\n");
    for (const auto& ch : CHANNELS)
    {
        for (float esno = -12.0f; esno <= 6.01f; esno += 1.0f)
        {
            std::atomic<int> err112(0), err112x2(0);
            std::atomic<int> ierr112(0), ierr112x2(0);
            float sigma = sigmaForEsNo(esno);

            parallelFor(trials, (unsigned)(esno * 100 + 1000) + (unsigned)(ch.dopplerHz * 37), [&](int, std::mt19937& rng) {
                // Decoders hold mutable BP buffers -- one per trial/thread.
                LDPCCode<56, 56> c112(HRA_56_56);

                std::array<uint8_t, 56> msg;
                std::bernoulli_distribution bit(0.5);
                for (auto& b : msg) b = bit(rng) ? 1 : 0;

                std::normal_distribution<float> noise(0.0f, sigma);

                // LDPC(112,56) single shot and sent twice back to back (fading
                // continues across both copies).
                {
                    FadingChannel fc(ch.dopplerHz, rng);
                    auto tx = encodeInterleaveBpsk(c112, msg);
                    float rx1[112], rx2[112], comb[112];
                    for (int i = 0; i < 112; i++) rx1[i] = fc.nextGain() * tx[i] + noise(rng);
                    for (int i = 0; i < 112; i++) rx2[i] = fc.nextGain() * tx[i] + noise(rng);
                    for (int i = 0; i < 112; i++) comb[i] = rx1[i] + rx2[i];
                    if (!decodeWindow(c112, rx1, msg)) err112++;
                    if (!decodeWindow(c112, comb, msg)) err112x2++;
                    if (!decodeWindow(c112, rx1, msg, nullptr, sigma * sigma)) ierr112++;
                    if (!decodeWindow(c112, comb, msg, nullptr, 2 * sigma * sigma)) ierr112x2++;
                }

            });

            printf("%s,%.1f,%.5f,%.5f,%.5f,%.5f\n", ch.name, esno,
                   (float)err112 / trials, (float)err112x2 / trials,
                   (float)ierr112 / trials, (float)ierr112x2 / trials);
            fflush(stdout);
        }
    }

    // Timing: cost of a decode attempt on a misaligned (non-converging)
    // window, which dominates rade_text's rotation sweep.
    auto timeDecode = [](auto& code, const char* name) {
        constexpr int N = std::decay_t<decltype(code)>::CODEWORD_BITS;
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        std::array<uint8_t, 56> msg{};
        const int REPS = 2000;
        std::vector<float> window(N);
        int totalIters = 0;
        auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < REPS; r++)
        {
            for (auto& w : window) w = nd(rng);
            int it = 0;
            decodeWindow(code, window.data(), msg, &it);
            totalIters += it;
        }
        auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / REPS;
        printf("# timing %s: %.1f us per misaligned decode attempt (avg %.1f iters); full %d-rotation sweep = %.1f ms\n",
               name, us, (double)totalIters / REPS, N, us * N / 1000.0);
    };
    LDPCCode<56, 56> c112(HRA_56_56);
    timeDecode(c112, "LDPC(112,56)");
}

// ---------------------------------------------------------------------------
// End-to-end mode through rade_text.
// ---------------------------------------------------------------------------

struct RxCtx
{
    const std::string* expected;
    bool gotCorrect = false;
    int wrong = 0;
};

static void onRx(rade_text_t, const char* txt, int len, void* state)
{
    auto* ctx = (RxCtx*)state;
    if (std::string(txt, len) == *ctx->expected) ctx->gotCorrect = true;
    else ctx->wrong++;
}

static void runE2EMode(int trials, float maxSeconds, bool combine, bool confirm)
{
    const char* callsigns[] = {"KG6AOV", "VE3/KG6AOV/MM"}; // 1 and 2 blocks
    const int maxSyms = (int)(maxSeconds * SYMBOL_RATE);

    printf("combine,confirm,callsign,channel,esno_db,trials,p_by_10s,p_by_20s,p_by_30s,p_by_45s,p_by_max,median_s,wrong_decodes\n");
    for (const char* cs : callsigns)
    {
        std::string expected(cs);
        for (const auto& ch : CHANNELS)
        {
            for (float esno = -10.0f; esno <= 2.01f; esno += 1.0f)
            {
                float sigma = sigmaForEsNo(esno);
                std::vector<int> decodeSym(trials, -1);
                std::atomic<int> wrong(0);

                parallelFor(trials, (unsigned)(esno * 100 + 5000) + (unsigned)(ch.dopplerHz * 37) + (unsigned)expected.size() * 3, [&](int i, std::mt19937& rng) {
                    rade_text_t tx = rade_text_create();
                    rade_text_t rx = rade_text_create();
                    rade_text_enable_stats_output(tx, 0);
                    rade_text_enable_stats_output(rx, 0);
                    rade_text_enable_rx_combining(rx, combine ? 1 : 0);
                    rade_text_enable_rx_combine_confirm(rx, confirm ? 1 : 0);
                    rade_text_generate_tx_string(tx, cs, (int)strlen(cs));

                    RxCtx ctx;
                    ctx.expected = &expected;
                    rade_text_set_rx_callback(rx, onRx, &ctx);

                    // Join at a random point within the TX cycle.
                    int blocks = (int)((expected.size() + 7) / 8);
                    int cycle = CODEWORD_SYMS * blocks;
                    int join = std::uniform_int_distribution<int>(0, cycle - 1)(rng);
                    for (int k = 0; k < join; k++) rade_text_tx_next_symbol(tx);

                    FadingChannel fc(ch.dopplerHz, rng);
                    std::normal_distribution<float> noise(0.0f, sigma);
                    for (int k = 0; k < maxSyms; k++)
                    {
                        float s = rade_text_tx_next_symbol(tx);
                        rade_text_rx_symbol(rx, fc.nextGain() * s + noise(rng));
                        if (ctx.gotCorrect)
                        {
                            decodeSym[i] = k + 1;
                            break;
                        }
                    }
                    wrong += ctx.wrong;

                    rade_text_destroy(tx);
                    rade_text_destroy(rx);
                });

                auto pBy = [&](float secs) {
                    int lim = (int)(secs * SYMBOL_RATE), n = 0;
                    for (int d : decodeSym) if (d > 0 && d <= lim) n++;
                    return (float)n / trials;
                };
                std::vector<int> ok;
                for (int d : decodeSym) if (d > 0) ok.push_back(d);
                std::sort(ok.begin(), ok.end());
                // Median over all trials (undecoded counted as infinite).
                float median = ((int)ok.size() * 2 > trials) ? ok[trials / 2] / SYMBOL_RATE : -1.0f;

                printf("%d,%d,%s,%s,%.1f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%d\n", combine ? 1 : 0, confirm ? 1 : 0, cs, ch.name, esno, trials,
                       pBy(10), pBy(20), pBy(30), pBy(45), pBy(maxSeconds), median, (int)wrong);
                fflush(stdout);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Noise-only false decode mode.
// ---------------------------------------------------------------------------

static void runNoiseMode(int trials, float seconds, bool combine, bool confirm)
{
    const int syms = (int)(seconds * SYMBOL_RATE);
    std::atomic<long> falseDecodes(0);

    parallelFor(trials, 99, [&](int, std::mt19937& rng) {
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(rx, 0);
        rade_text_enable_rx_combining(rx, combine ? 1 : 0);
        rade_text_enable_rx_combine_confirm(rx, confirm ? 1 : 0);
        std::string never = "\x01";
        RxCtx ctx;
        ctx.expected = &never;
        rade_text_set_rx_callback(rx, onRx, &ctx);

        std::normal_distribution<float> noise(0.0f, 1.0f);
        for (int k = 0; k < syms; k++) rade_text_rx_symbol(rx, noise(rng));
        falseDecodes += ctx.wrong;
        rade_text_destroy(rx);
    });

    double hours = (double)trials * seconds / 3600.0;
    printf("combine,confirm,noise_hours,false_decodes,per_hour\n%d,%d,%.2f,%ld,%.3f\n",
           combine ? 1 : 0, confirm ? 1 : 0, hours, (long)falseDecodes, falseDecodes / hours);
}

int main(int argc, char** argv)
{
    ulog_set_quiet(true);
    ulog_set_level(LOG_FATAL);

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s codeword [trials] | e2e [trials] [max_seconds] [combine] [confirm] | noise [trials] [seconds] [combine] [confirm]\n", argv[0]);
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "codeword")
    {
        runCodewordMode(argc > 2 ? atoi(argv[2]) : 2000);
    }
    else if (mode == "e2e")
    {
        runE2EMode(argc > 2 ? atoi(argv[2]) : 200, argc > 3 ? (float)atof(argv[3]) : 60.0f, argc > 4 ? atoi(argv[4]) != 0 : true, argc > 5 ? atoi(argv[5]) != 0 : false);
    }
    else if (mode == "noise")
    {
        runNoiseMode(argc > 2 ? atoi(argv[2]) : 100, argc > 3 ? (float)atof(argv[3]) : 600.0f, argc > 4 ? atoi(argv[4]) != 0 : true, argc > 5 ? atoi(argv[5]) != 0 : false);
    }
    else
    {
        fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 1;
    }
    return 0;
}
