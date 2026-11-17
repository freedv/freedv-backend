//==========================================================================
// Name:            RadeTextOtaSim.cpp
//
// Purpose:         Monte Carlo time-to-decode of rade_text callsigns through
//                  the real RADE V2 modem and a simulated HF channel.
//                  Not a pass/fail test -- prints CSV for offline analysis.
// Created:         October 5, 2026
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
// Signal path (mirrors RADETransmitStep/RADEReceiveStep):
//
//   speech features (file, looped) + rade_text TX symbols
//     -> rade_tx() (complex 8 kHz)
//     -> channel: AWGN, or two-path Watterson MPG (0.1 Hz Doppler spread,
//        0.5 ms delay) / MPP (1 Hz, 2 ms), each path an independent
//        Rayleigh process with Gaussian Doppler spectrum
//     -> real part + real AWGN
//     -> rade_rx() fed {2*real, 0} as RADEReceiveStep does
//     -> rade_rx_get_data_symbol() -> rade_text_rx_symbol(), with
//        rade_text_rx_reset() whenever modem sync is lost.
//
// SNR is the usual 3 kHz noise bandwidth definition (as codec2's ch tool),
// with signal power measured on the transmitted signal before fading:
//   SNR3k = P_real / (N0 * 3000),  N0 = sigma^2 / (Fs/2)
//
// Usage: RadeTextOtaSim channel snr3k_db trials max_seconds combine features.f32 [callsign] [seed]
//   combine: -1 = receive combining disabled, 0 = enabled, 1 = enabled with
//            confirmation of combined-only decodes
//
// Output: one CSV row per trial:
//   channel,snr3k_db,trial,decode_s,first_sync_s,sync_frac,rade_snr_est_db,wrong
// decode_s is -1 if the callsign wasn't decoded within max_seconds (measured
// from the start of the signal, so includes modem acquisition).
// rade_snr_est_db is the average of RADE's own SNR estimate while in sync,
// i.e. what a user would see displayed.
//
//==========================================================================

#include "../rade_text.h"
#include "../../util/logging/ulog.h"
#include "rade_api.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

static constexpr float FS = RADE_MODEM_SAMPLE_RATE;
static constexpr int NB_FEATURES = 36; // NB_TOTAL_FEATURES
static constexpr int CODEWORD_SYMS = 112; // LDPC(112,56)

// Rayleigh process with Gaussian Doppler spectrum (sum of sinusoids with
// Gaussian-distributed Doppler frequencies), unit mean power. "Doppler
// spread" follows the CCIR/Watterson convention of 2 sigma.
class RayleighPath
{
public:
    RayleighPath(float spreadHz, std::mt19937& rng)
    {
        std::normal_distribution<float> fd(0.0f, spreadHz / 2.0f);
        std::uniform_real_distribution<float> ph(0.0f, 2.0f * (float)M_PI);
        for (int i = 0; i < N; i++)
        {
            w_[i] = 2.0f * (float)M_PI * fd(rng) / FS;
            phi_[i] = ph(rng);
        }
    }

    std::complex<float> gain(long n) const
    {
        std::complex<float> g(0, 0);
        for (int i = 0; i < N; i++)
        {
            float a = w_[i] * (float)n + phi_[i];
            g += std::complex<float>(std::cos(a), std::sin(a));
        }
        return g / std::sqrt((float)N);
    }

private:
    static constexpr int N = 32;
    float w_[N], phi_[N];
};

struct RxCtx
{
    std::string expected;
    bool gotCorrect = false;
    int wrong = 0;
};

static void onRx(rade_text_t, const char* txt, int len, void* state)
{
    auto* ctx = (RxCtx*)state;
    if (std::string(txt, len) == ctx->expected) ctx->gotCorrect = true;
    else ctx->wrong++;
}

int main(int argc, char** argv)
{
    if (argc < 7)
    {
        fprintf(stderr, "usage: %s awgn|mpg|mpp snr3k_db trials max_seconds combine features.f32 [callsign] [seed]\n", argv[0]);
        return 1;
    }

    std::string channel = argv[1];
    float snrDb = (float)atof(argv[2]);
    int trials = atoi(argv[3]);
    float maxSeconds = (float)atof(argv[4]);
    int combine = atoi(argv[5]);
    const char* featureFile = argv[6];
    std::string callsign = argc > 7 ? argv[7] : "KG6AOV";
    unsigned seed = argc > 8 ? (unsigned)atoi(argv[8]) : 1;

    float spreadHz = 0.0f, delayMs = 0.0f;
    if (channel == "mpg") { spreadHz = 0.1f; delayMs = 0.5f; }
    else if (channel == "mpp") { spreadHz = 1.0f; delayMs = 2.0f; }
    else if (channel != "awgn") { fprintf(stderr, "unknown channel\n"); return 1; }
    const int delaySamples = (int)std::lround(delayMs * 1e-3f * FS);

    ulog_set_quiet(true);
    ulog_set_level(LOG_FATAL);

    // Load speech features to loop through.
    std::vector<float> features;
    {
        FILE* fp = fopen(featureFile, "rb");
        if (!fp) { perror(featureFile); return 1; }
        float buf[4096];
        size_t n;
        while ((n = fread(buf, sizeof(float), 4096, fp)) > 0) features.insert(features.end(), buf, buf + n);
        fclose(fp);
    }
    const int numFeatureFrames = (int)(features.size() / NB_FEATURES);

    rade_initialize();

    for (int trial = 0; trial < trials; trial++)
    {
        std::mt19937 rng(seed * 1000003u + (unsigned)trial * 7919u + (unsigned)(snrDb * 100.0f + 10000.0f));

        char modelFile[1] = {0};
        struct rade* r = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_MODE_V2 | RADE_VERBOSE_0);
        const int nFeat = rade_n_features_in_out(r);
        const int nTxOut = rade_n_tx_out(r);

        // ---- TX ----
        rade_text_t tx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_generate_tx_string(tx, callsign.c_str(), (int)callsign.size());
        int blocks = (int)((callsign.size() + 7) / 8);
        int join = std::uniform_int_distribution<int>(0, CODEWORD_SYMS * blocks - 1)(rng);
        for (int k = 0; k < join; k++) rade_text_tx_next_symbol(tx);

        // Enough modem frames for max_seconds of RX plus a little slack for
        // receiver latency.
        const int numFrames = (int)((maxSeconds + 1.0f) * FS / nTxOut) + 1;
        std::vector<std::complex<float>> txSig;
        txSig.reserve((size_t)numFrames * nTxOut);
        std::vector<float> featIn(nFeat);
        std::vector<RADE_COMP> txOut(nTxOut);
        int featFrame = std::uniform_int_distribution<int>(0, numFeatureFrames - 1)(rng);
        for (int f = 0; f < numFrames; f++)
        {
            for (int i = 0; i < nFeat; i += NB_FEATURES)
            {
                memcpy(&featIn[i], &features[(size_t)featFrame * NB_FEATURES], sizeof(float) * NB_FEATURES);
                featFrame = (featFrame + 1) % numFeatureFrames;
            }
            rade_tx_set_data_symbol(r, rade_text_tx_next_symbol(tx));
            int n = rade_tx(r, txOut.data(), featIn.data());
            for (int i = 0; i < n; i++) txSig.emplace_back(txOut[i].real, txOut[i].imag);
        }
        rade_text_destroy(tx);

        // ---- Channel ----
        double pComplex = 0;
        for (auto& s : txSig) pComplex += std::norm(s);
        pComplex /= txSig.size();
        const double pReal = pComplex / 2.0;
        const double snrLin = std::pow(10.0, snrDb / 10.0);
        const float sigma = (float)std::sqrt(pReal / (snrLin * 3000.0 / (FS / 2.0)));

        RayleighPath p1(spreadHz > 0 ? spreadHz : 1.0f, rng), p2(spreadHz > 0 ? spreadHz : 1.0f, rng);
        std::normal_distribution<float> noise(0.0f, sigma);
        std::vector<float> rxReal(txSig.size());
        for (size_t n = 0; n < txSig.size(); n++)
        {
            std::complex<float> y = txSig[n];
            if (spreadHz > 0)
            {
                std::complex<float> delayed = n >= (size_t)delaySamples ? txSig[n - delaySamples] : std::complex<float>(0, 0);
                // Two equal-power paths, total mean power gain 1.
                y = (p1.gain((long)n) * txSig[n] + p2.gain((long)n) * delayed) / std::sqrt(2.0f);
            }
            rxReal[n] = y.real() + noise(rng);
        }

        // ---- RX ----
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(rx, 0);
        rade_text_enable_rx_combining(rx, combine >= 0);
        rade_text_enable_rx_combine_confirm(rx, combine > 0);
        RxCtx ctx;
        ctx.expected = callsign;
        rade_text_set_rx_callback(rx, onRx, &ctx);

        std::vector<RADE_COMP> rxIn(rade_nin_max(r));
        std::vector<float> featOut(nFeat);
        size_t pos = 0;
        const size_t maxPos = (size_t)(maxSeconds * FS);
        int prevSync = 0;
        float decodeS = -1, firstSyncS = -1;
        long syncFrames = 0, frames = 0;
        double snrEstSum = 0;
        long snrEstN = 0;
        while (pos + rade_nin(r) <= maxPos)
        {
            int nin = rade_nin(r);
            for (int i = 0; i < nin; i++)
            {
                rxIn[i].real = 2.0f * rxReal[pos + i];
                rxIn[i].imag = 0.0f;
            }
            pos += nin;

            int hasEoo = 0;
            int nout = rade_rx(r, featOut.data(), &hasEoo, nullptr, rxIn.data());
            if (nout > 0)
            {
                float y = rade_rx_get_data_symbol(r);
                rade_text_rx_symbol(rx, y);
            }

            int sync = rade_sync(r);
            frames++;
            if (sync)
            {
                syncFrames++;
                snrEstSum += rade_snrdB_3k_est(r);
                snrEstN++;
                if (firstSyncS < 0) firstSyncS = pos / FS;
            }
            if (prevSync && !sync) rade_text_rx_reset(rx);
            prevSync = sync;

            if (ctx.gotCorrect)
            {
                decodeS = pos / FS;
                break;
            }
        }

        printf("%s,%.1f,%d,%.2f,%.2f,%.3f,%.2f,%d\n", channel.c_str(), snrDb, trial, decodeS, firstSyncS,
               frames ? (float)syncFrames / frames : 0.0f, snrEstN ? snrEstSum / snrEstN : NAN, ctx.wrong);
        fflush(stdout);

        rade_text_destroy(rx);
        rade_close(r);
    }

    rade_finalize();
    return 0;
}
