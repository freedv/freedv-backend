//==========================================================================
// Name:            RadeTextUnitTest.cpp
//
// Purpose:         Unit tests for rade_text encode/decode without the full
//                  RADE audio pipeline.  Tests exercise character encoding,
//                  LDPC encode/decode, interleaving, CRC validation, and the
//                  complete generate→receive round-trip.
// Created:         June 14, 2026
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

#include "../rade_text.h"
#include "../ldpc_encode.h"
#include "../../util/logging/ulog.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Total float count for the EOO symbols (payload only, no filler).
// LDPC(112,56): 56 symbols × 2 floats = 112 floats.
static constexpr int PAYLOAD_FLOATS  = 112;
// Number of symbols = floats / 2.
static constexpr int PAYLOAD_SYMBOLS = PAYLOAD_FLOATS / 2;

// Extra symbols appended after the payload so that rade_text_rx can estimate
// noise variance from the known filler sequence.
static constexpr int FILLER_SYMS  = 20;
static constexpr int TOTAL_FLOATS = PAYLOAD_FLOATS + FILLER_SYMS * 2;
static constexpr int TOTAL_SYMS   = PAYLOAD_SYMBOLS + FILLER_SYMS;

struct RxState {
    std::string received;
    int callCount = 0;
};

static void onTextRx(rade_text_t, const char* txt, int len, void* state)
{
    auto* s = reinterpret_cast<RxState*>(state);
    s->received.assign(txt, len);
    s->callCount++;
}

// Add Gaussian noise to a float symbol array.
static void addNoiseToSyms(float* syms, int nfloats, float sigma, std::mt19937& rng)
{
    std::normal_distribution<float> nd(0.0f, sigma);
    for (int i = 0; i < nfloats; i++)
        syms[i] += nd(rng);
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Encode callsign, optionally add noise, then decode and return whether the
// callsign was recovered.  Uses TOTAL_FLOATS / TOTAL_SYMS so that the noise
// estimator inside rade_text_rx has filler symbols to work with.
static bool roundTrip(const char* callsign, float sigma = 0.0f, unsigned seed = 42)
{
    rade_text_t tx = rade_text_create();
    rade_text_t rx = rade_text_create();
    assert(tx && rx);
    rade_text_enable_stats_output(tx, 0);
    rade_text_enable_stats_output(rx, 0);

    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);

    float syms[TOTAL_FLOATS];
    memset(syms, 0, sizeof(syms));
    rade_text_generate_tx_string(tx, callsign, (int)strlen(callsign), syms, TOTAL_FLOATS);

    if (sigma > 0.0f) {
        std::mt19937 rng(seed);
        addNoiseToSyms(syms, TOTAL_FLOATS, sigma, rng);
    }

    rade_text_rx(rx, syms, TOTAL_SYMS);

    rade_text_destroy(tx);
    rade_text_destroy(rx);

    return state.callCount == 1 && state.received == callsign;
}

// ---------------------------------------------------------------------------
// Test 1: Perfect noiseless round-trip for a set of representative callsigns
// ---------------------------------------------------------------------------
static bool test1_noiseless_callsigns()
{
    printf("=== Test 1: noiseless round-trip for representative callsigns ===\n");

    const char* callsigns[] = {
        "K6AQ",      // short US callsign
        "W1AW",      // ARRL HQ callsign
        "VK2TGP",    // Australian callsign (6 chars)
        "AA0ZZ",     // US callsign with digit in prefix
        "N0CALL",    // 6-char callsign
        "KA1BCD",    // 6-char callsign
        "W4XYZ567",  // 8-char max-length callsign
    };

    bool ok = true;
    for (const char* cs : callsigns) {
        bool passed = roundTrip(cs);
        printf("  %-10s  %s\n", cs, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("Noiseless round-trip: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: Lowercase input is treated as uppercase
// ---------------------------------------------------------------------------
static bool test2_lowercase_normalized()
{
    printf("=== Test 2: lowercase input normalised to uppercase ===\n");

    struct { const char* input; const char* expected; } cases[] = {
        {"k6aq",    "K6AQ"},
        {"w1aw",    "W1AW"},
        {"vk2tgp",  "VK2TGP"},
    };

    bool ok = true;
    for (auto& c : cases) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, c.input, (int)strlen(c.input), syms, TOTAL_FLOATS);
        rade_text_rx(rx, syms, TOTAL_SYMS);

        rade_text_destroy(tx);
        rade_text_destroy(rx);

        bool passed = (state.callCount == 1 && state.received == c.expected);
        printf("  '%s' -> '%s' (expected '%s')  %s\n",
               c.input, state.received.c_str(), c.expected, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("Lowercase normalisation: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 3: Heavy noise causes decode failure (callback never fired)
// ---------------------------------------------------------------------------
static bool test3_heavy_noise_no_callback()
{
    printf("=== Test 3: heavy noise – callback must not fire ===\n");

    // At sigma=5.0 the raw BER is ~50% and LDPC will almost certainly fail to
    // converge.  Even if it does, the CRC provides a second layer of protection.
    const char* cs = "K6AQ";
    int false_callbacks = 0;
    const int TRIALS = 30;

    for (unsigned seed = 0; seed < (unsigned)TRIALS; seed++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TOTAL_FLOATS);

        std::mt19937 rng(seed * 1234567u);
        addNoiseToSyms(syms, TOTAL_FLOATS, 5.0f, rng);

        rade_text_rx(rx, syms, TOTAL_SYMS);

        if (state.callCount > 0) false_callbacks++;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    bool ok = (false_callbacks <= TRIALS / 20);
    printf("False callbacks: %d / %d\n", false_callbacks, TRIALS);
    printf("Heavy-noise no-callback: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 4: CRC prevents a wrong-but-converged decode from firing the callback
// ---------------------------------------------------------------------------
static bool test4_crc_blocks_wrong_callsign()
{
    printf("=== Test 4: CRC blocks wrong-callsign callback ===\n");

    // Transmit "K6AQ" but corrupt exactly 2 adjacent floats in the payload
    // region so that, if LDPC wrongly converges to a different codeword,
    // the CRC will catch it.  We repeat over several corruption patterns and
    // count how often we get a callback that delivers a *wrong* callsign.
    const char* cs = "K6AQ";
    int wrong_rx = 0;
    const int TRIALS = PAYLOAD_FLOATS;

    for (int flip = 0; flip < TRIALS; flip++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TOTAL_FLOATS);

        // Negate one float inside the payload region.
        syms[flip % PAYLOAD_FLOATS] = -syms[flip % PAYLOAD_FLOATS];

        rade_text_rx(rx, syms, TOTAL_SYMS);

        if (state.callCount > 0 && state.received != cs)
            wrong_rx++;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    bool ok = (wrong_rx == 0);
    printf("Wrong callsign deliveries: %d / %d\n", wrong_rx, TRIALS);
    printf("CRC protection: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 5: Encode/decode consistency – generate then receive without touching
//         symbols must always fire the callback with the original callsign
// ---------------------------------------------------------------------------
static bool test5_idempotent_generate_receive()
{
    printf("=== Test 5: idempotent generate→receive (callback always fires correctly) ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL", "KA1BCD"};
    bool ok = true;

    for (const char* cs : callsigns) {
        for (int repeat = 0; repeat < 3; repeat++) {
            // Re-create objects each time to exercise fresh state.
            rade_text_t tx = rade_text_create();
            rade_text_t rx = rade_text_create();
            rade_text_enable_stats_output(tx, 0);
            rade_text_enable_stats_output(rx, 0);

            RxState state;
            rade_text_set_rx_callback(rx, onTextRx, &state);

            float syms[TOTAL_FLOATS];
            memset(syms, 0, sizeof(syms));
            rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TOTAL_FLOATS);
            rade_text_rx(rx, syms, TOTAL_SYMS);

            bool passed = (state.callCount == 1 && state.received == cs);
            if (!passed) {
                printf("  FAIL callsign='%s' repeat=%d callCount=%d received='%s'\n",
                       cs, repeat, state.callCount, state.received.c_str());
                ok = false;
            }

            rade_text_destroy(tx);
            rade_text_destroy(rx);
        }
    }
    printf("Idempotent generate→receive: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 6: Filler symbols in symSize > 56 path are included without breakage
// ---------------------------------------------------------------------------
static bool test6_filler_symbols_no_crash()
{
    printf("=== Test 6: filler symbols path (symSize > 56) ===\n");

    bool ok = true;
    // Vary filler counts: 1 to 40 extra symbols.
    for (int extra = 1; extra <= 40; extra++) {
        int tx_floats  = PAYLOAD_FLOATS + extra * 2;
        int rx_symbols = PAYLOAD_SYMBOLS + extra;

        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        // Stack-allocate a generous buffer.
        float syms[512];
        assert(tx_floats <= (int)(sizeof(syms)/sizeof(syms[0])));
        memset(syms, 0, sizeof(syms));

        rade_text_generate_tx_string(tx, "K6AQ", 4, syms, tx_floats);
        rade_text_rx(rx, syms, rx_symbols);

        bool passed = (state.callCount == 1 && state.received == "K6AQ");
        if (!passed) {
            printf("  FAIL extra=%d callCount=%d received='%s'\n",
                   extra, state.callCount, state.received.c_str());
            ok = false;
        }

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }
    printf("Filler symbols path: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 7: Low-level character encoding round-trip
//         Encode callsign to OTA then decode back; check identity.
//         This is tested indirectly through a generate→receive cycle that uses
//         each character type: letters, digits, ASCII 38-46 punctuation, '/'.
// ---------------------------------------------------------------------------
static bool test7_character_encoding_coverage()
{
    printf("=== Test 7: character encoding coverage ===\n");

    // Characters in the 6-bit OTA alphabet:
    //   ASCII 38-46 ('&','\'','(',')','*','+',',','-','.') -> OTA 1-9  (skip 0=null)
    //   ASCII '0'-'9'                                       -> OTA 10-19
    //   ASCII 'A'-'Z'                                      -> OTA 20-45
    //   ASCII '/'                                          -> OTA 46
    //
    // We test a sample from each range as part of the callsign.
    // (Real callsigns only use letters and digits but the code supports the
    // full 6-bit character set, so we exercise that here.)
    struct { const char* label; const char* cs; } cases[] = {
        {"digits only",   "1234567"},
        {"letters only",  "ABCDEFG"},
        {"mixed",         "W4AB123"},
        {"single char",   "K"},
        {"8 chars",       "KA1BCDE7"},
        {"slash (portable)", "K6AQ/5"},
    };

    bool ok = true;
    for (auto& c : cases) {
        bool passed = roundTrip(c.cs);
        printf("  %-15s %-10s  %s\n", c.label, c.cs, passed ? "PASS" : "FAIL");
        ok &= passed;
    }

    // Exhaustively test every individual character in the 6-bit OTA alphabet
    // (ASCII 38-46 punctuation, '0'-'9', 'A'-'Z', and '/') round-trips on its
    // own as a single-character string.
    printf("  -- exhaustive single-character sweep --\n");
    std::string allChars;
    for (char ch = 38; ch <= 46; ch++) allChars.push_back(ch);
    for (char ch = '0'; ch <= '9'; ch++) allChars.push_back(ch);
    for (char ch = 'A'; ch <= 'Z'; ch++) allChars.push_back(ch);
    allChars.push_back('/');

    int single_fail = 0;
    for (char ch : allChars) {
        char cs[2] = { ch, 0 };
        bool passed = roundTrip(cs);
        if (!passed) {
            printf("  FAIL char='%c' (0x%02X)\n", ch, (unsigned char)ch);
            single_fail++;
        }
    }
    bool singleOk = (single_fail == 0);
    printf("  Single-character sweep: %d/%zu passed  %s\n",
           (int)allChars.size() - single_fail, allChars.size(), singleOk ? "PASS" : "FAIL");
    ok &= singleOk;

    // Group every valid character into 8-char (max-length) strings to also
    // exercise multi-character combinations of punctuation, digits, letters,
    // and '/' together.
    printf("  -- grouped 8-character sweep --\n");
    constexpr size_t MAX_CALLSIGN_LEN = 8;  // matches RADE_TEXT_MAX_LENGTH in rade_text.cpp
    int group_fail = 0;
    int group_total = 0;
    for (size_t i = 0; i < allChars.size(); i += MAX_CALLSIGN_LEN) {
        std::string chunk = allChars.substr(i, MAX_CALLSIGN_LEN);
        group_total++;
        bool passed = roundTrip(chunk.c_str());
        printf("  %-10s  %s\n", chunk.c_str(), passed ? "PASS" : "FAIL");
        if (!passed) group_fail++;
    }
    bool groupOk = (group_fail == 0);
    printf("  Grouped sweep: %d/%d passed  %s\n",
           group_total - group_fail, group_total, groupOk ? "PASS" : "FAIL");
    ok &= groupOk;

    printf("Character encoding coverage: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 8: Mild noise – at sigma=0.1 the decoder should still succeed
// ---------------------------------------------------------------------------
static bool test8_mild_noise()
{
    printf("=== Test 8: mild noise (sigma=0.1, ~20 dB SNR) ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP"};
    bool ok = true;

    for (const char* cs : callsigns) {
        int pass = 0;
        const int TRIALS = 10;
        for (unsigned seed = 0; seed < (unsigned)TRIALS; seed++) {
            if (roundTrip(cs, 0.1f, seed)) pass++;
        }
        bool passed = (pass >= (int)(TRIALS * 0.9));  // >=90% success rate
        printf("  %-10s  %d/%d  %s\n", cs, pass, TRIALS, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("Mild noise: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Helpers for noise-level tests
// ---------------------------------------------------------------------------

struct NoiseTrialResult {
    int correct;    // callback fired and callsign matched
    int cb_any;     // callback fired (for any reason)
    int cb_wrong;   // callback fired with the WRONG callsign
    int trials;
};

// Run TRIALS encode→noise→decode cycles for one callsign at one sigma.
// Returns tallied counts.  Uses 40 filler symbols to give the noise
// estimator inside rade_text_rx a better variance sample.
static NoiseTrialResult noiseTrials(const char* cs, float sigma,
                                    int trials, unsigned base_seed = 0)
{
    // Larger filler count for better noise estimation at higher sigma.
    constexpr int FILLER     = 40;
    constexpr int TX_FLOATS  = PAYLOAD_FLOATS + FILLER * 2;
    constexpr int RX_SYMBOLS = PAYLOAD_SYMBOLS + FILLER;

    NoiseTrialResult r{};
    r.trials = trials;

    for (int t = 0; t < trials; t++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TX_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TX_FLOATS);

        std::mt19937 rng(base_seed + (unsigned)t * 131071u + 3u);
        addNoiseToSyms(syms, TX_FLOATS, sigma, rng);

        rade_text_rx(rx, syms, RX_SYMBOLS);

        if (state.callCount > 0) {
            r.cb_any++;
            if (state.received == cs) r.correct++;
            else                       r.cb_wrong++;
        }

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Test 9: sigma=0.2 (~14 dB SNR) – robust above the floor
// ---------------------------------------------------------------------------
static bool test9_sigma02_robust()
{
    printf("=== Test 9: sigma=0.2 (~14 dB) – should decode reliably ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "AA0ZZ"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.2f, 20, 1000u);
        bool passed = (r.correct >= 19) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.2 robust: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 10: sigma=0.3 (~10 dB SNR) – near the reliable operational limit
// ---------------------------------------------------------------------------
static bool test10_sigma03_reliable()
{
    printf("=== Test 10: sigma=0.3 (~10 dB) – should still succeed most of the time ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.3f, 30, 2000u);
        // Require ≥90% success and zero wrong-callsign deliveries.
        bool passed = (r.correct >= 27) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.3 reliable: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 11: sigma=0.5 (~6 dB SNR) – performance cliff; require ≥75% success
// ---------------------------------------------------------------------------
static bool test11_sigma05_cliff()
{
    printf("=== Test 11: sigma=0.5 (~6 dB) – performance cliff, >=75%% expected ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.5f, 40, 3000u);
        // Require ≥75% success and zero wrong-callsign deliveries.
        bool passed = (r.correct >= 30) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.5 cliff: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 12: sigma=0.7 – marginal noise; callback must never deliver wrong callsign
//
// At this level the LDPC decoder mostly fails to converge (known limitation of
// the current phi() implementation at high SNR causing BP messages to collapse).
// Success rate may be low; what we guarantee is the CRC layer prevents any
// incorrectly decoded callsign from reaching the application.
// ---------------------------------------------------------------------------
static bool test12_sigma07_no_false_positive()
{
    printf("=== Test 12: sigma=0.7 – no false-positive callsigns at marginal noise ===\n");

    // Test with several callsigns to cover a range of bit patterns.
    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL", "KA1BCD", "W4XYZ567"};
    int total_wrong = 0;
    int total_cb    = 0;
    int total_pass  = 0;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.7f, 30, 4000u);
        total_wrong += r.cb_wrong;
        total_cb    += r.cb_any;
        total_pass  += r.correct;
        printf("  %-10s  %2d/%d correct  %d/%d cb  %d wrong\n",
               cs, r.correct, r.trials, r.cb_any, r.trials, r.cb_wrong);
    }

    bool ok = (total_wrong == 0);
    printf("Total correct=%d  cb_any=%d  cb_wrong=%d\n",
           total_pass, total_cb, total_wrong);
    printf("No-false-positive at sigma=0.7: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 13: sigma=1.0, 1.5, 2.0 – beyond operational limit
//
// At these noise levels the raw bit-error rate overwhelms the LDPC code and
// decode almost always fails.  The essential invariant is that the CRC layer
// never lets a spurious wrong-callsign delivery reach the application.
// ---------------------------------------------------------------------------
static bool test13_high_noise_no_false_positive()
{
    printf("=== Test 13: sigma=1.0/1.5/2.0 – beyond limit, never wrong callsign ===\n");

    struct { float sigma; const char* label; } levels[] = {
        {1.0f, "1.0 (~0 dB)"},
        {1.5f, "1.5 (~-3 dB)"},
        {2.0f, "2.0 (~-6 dB)"},
    };
    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "AA0ZZ", "N0CALL"};

    bool ok = true;
    for (auto& lv : levels) {
        int wrong = 0, cb = 0, pass = 0;
        for (const char* cs : callsigns) {
            auto r = noiseTrials(cs, lv.sigma, 20, 5000u);
            wrong += r.cb_wrong;
            cb    += r.cb_any;
            pass  += r.correct;
        }
        bool level_ok = (wrong == 0);
        printf("  sigma=%-12s  correct=%d  cb_any=%d  cb_wrong=%d  %s\n",
               lv.label, pass, cb, wrong, level_ok ? "PASS" : "FAIL");
        ok &= level_ok;
    }
    printf("High-noise no-false-positive: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 14: Noise sweep diagnostic (informational – not in pass/fail)
//
// Prints a concise sigma vs. success-rate table so regressions in the
// performance curve are visible in CI output even without a hard threshold.
// ---------------------------------------------------------------------------
static void test14_noise_sweep_diagnostic()
{
    printf("=== Test 14: noise sweep diagnostic (INFORMATIONAL) ===\n");
    printf("  %-8s  %-12s  %s\n", "sigma", "correct/total", "cb_wrong");

    const char* cs = "K6AQ";
    for (float sigma : {0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 1.0f, 1.5f, 2.0f}) {
        auto r = noiseTrials(cs, sigma, 50, 9000u);
        printf("  %-8.1f  %2d/%-10d  %d\n",
               sigma, r.correct, r.trials, r.cb_wrong);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Reference framer: builds the on-air symbols for an arbitrary 8-bit CRC and
// eight 6-bit character codes, independently of rade_text, so tests can
// send frames the encoder never would (a wrong CRC, undefined codes).
// Format: [CRC (8 bits, LSB first) | 8 x 6-bit codes (LSB first)] ->
// LDPC(112,56) -> interleave (b = 37 over 56 symbols) -> QPSK.
// ---------------------------------------------------------------------------
static unsigned char referenceCrc8(const unsigned char* codes, int count)
{
    unsigned char crc = 0;
    for (int i = 0; i < count && codes[i] != 0; i++) {
        crc ^= codes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x1D) : (unsigned char)(crc << 1);
    }
    return crc;
}

static void referenceFrame(unsigned char crc, const unsigned char codes[8], float* syms, int nfloats)
{
    std::array<uint8_t, 56> info{};
    for (int i = 0; i < 8; i++)
        info[i] = (crc >> i) & 1;
    for (int i = 0; i < 48; i++)
        info[8 + i] = (codes[i / 6] >> (i % 6)) & 1;
    auto codeword = ldpc_encode(info);

    char interleaved[PAYLOAD_FLOATS];
    for (int i = 0; i < PAYLOAD_SYMBOLS; i++) {
        int to = (37 * i) % PAYLOAD_SYMBOLS;
        interleaved[2 * to] = codeword[2 * i];
        interleaved[2 * to + 1] = codeword[2 * i + 1];
    }

    for (int i = 0; i < PAYLOAD_SYMBOLS; i++) {
        int bits = (interleaved[2 * i] << 1) | interleaved[2 * i + 1];
        static const float map[4][2] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
        syms[2 * i] = map[bits][0];
        syms[2 * i + 1] = map[bits][1];
    }
    // Known filler after the payload, as the transmitter sends.
    for (int i = PAYLOAD_FLOATS; i < nfloats; i++)
        syms[i] = i % 2 ? 0 : 1;
}

// 6-bit code for a callsign character (A-Z, 0-9 here).
static unsigned char otaCode(char c)
{
    return (c >= '0' && c <= '9') ? (unsigned char)(c - '0' + 10) : (unsigned char)(c - 'A' + 20);
}

static RxState receiveFrame(const float* syms)
{
    float copy[TOTAL_FLOATS];
    memcpy(copy, syms, sizeof(copy));
    rade_text_t rx = rade_text_create();
    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);
    rade_text_rx(rx, copy, TOTAL_SYMS);
    rade_text_destroy(rx);
    return state;
}

// ---------------------------------------------------------------------------
// Test 15: the encoder's on-air format matches the reference framer, so
//          changes that would break decoding by other FreeDV versions show up
// ---------------------------------------------------------------------------
static bool test15_on_air_format()
{
    printf("=== Test 15: on-air format matches the reference framer ===\n");

    unsigned char codes[8] = {otaCode('K'), otaCode('6'), otaCode('A'), otaCode('Q')};
    float expected[TOTAL_FLOATS];
    referenceFrame(referenceCrc8(codes, 8), codes, expected, TOTAL_FLOATS);

    rade_text_t tx = rade_text_create();
    float syms[TOTAL_FLOATS];
    memset(syms, 0, sizeof(syms));
    rade_text_generate_tx_string(tx, "K6AQ", 4, syms, TOTAL_FLOATS);
    rade_text_destroy(tx);

    bool ok = memcmp(syms, expected, sizeof(syms)) == 0;
    RxState state = receiveFrame(expected);
    ok &= state.callCount == 1 && state.received == "K6AQ";

    printf("On-air format: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 16: a frame that decodes cleanly but whose CRC doesn't match its text
//          (as when noise turns it into a different valid codeword) is
//          dropped rather than shown as a callsign
// ---------------------------------------------------------------------------
static bool test16_valid_codeword_bad_crc_rejected()
{
    printf("=== Test 16: valid codeword with a wrong CRC is rejected ===\n");

    unsigned char codes[8] = {otaCode('K'), otaCode('6'), otaCode('A'), otaCode('Q')};
    unsigned char crc = referenceCrc8(codes, 8);
    bool ok = true;
    for (int bit = 0; bit < 8; bit++) {
        float syms[TOTAL_FLOATS];
        referenceFrame(crc ^ (1 << bit), codes, syms, TOTAL_FLOATS);
        RxState state = receiveFrame(syms);
        if (state.callCount != 0) {
            printf("  FAIL: CRC bit %d flipped, got '%s'\n", bit, state.received.c_str());
            ok = false;
        }
    }

    printf("Wrong CRC rejected: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 17: undefined 6-bit codes (47 = space, which the encoder never sends,
//          and 48-63) in an otherwise valid frame are left out of the text
// ---------------------------------------------------------------------------
static bool test17_undefined_codes_dropped()
{
    printf("=== Test 17: undefined character codes are dropped on receive ===\n");

    unsigned char codes[8] = {otaCode('K'), 47, otaCode('6'), 48, otaCode('A'), 63, otaCode('Q'), 0};
    float syms[TOTAL_FLOATS];
    referenceFrame(referenceCrc8(codes, 8), codes, syms, TOTAL_FLOATS);
    RxState state = receiveFrame(syms);

    bool ok = state.callCount == 1 && state.received == "K6AQ";
    printf("Received '%s' (%d callbacks)\n", state.received.c_str(), state.callCount);
    printf("Undefined codes dropped: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 18: characters outside the 6-bit set are skipped when encoding, and
//          the rest of the callsign still gets through
// ---------------------------------------------------------------------------
static bool test18_unencodable_characters_skipped()
{
    printf("=== Test 18: characters outside the character set are skipped ===\n");

    // ' ', '!', '#', '_', '~' and '@' have no 6-bit code.
    const char* input = "K6 !#_~@";
    rade_text_t tx = rade_text_create();
    float syms[TOTAL_FLOATS];
    memset(syms, 0, sizeof(syms));
    rade_text_generate_tx_string(tx, input, (int)strlen(input), syms, TOTAL_FLOATS);
    rade_text_destroy(tx);
    RxState state = receiveFrame(syms);

    bool ok = state.callCount == 1 && state.received == "K6";
    printf("Received '%s' (%d callbacks)\n", state.received.c_str(), state.callCount);
    printf("Unencodable characters skipped: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 19: a callsign longer than 8 characters is cut to the first 8, and an
//          empty one is sent and received as empty
// ---------------------------------------------------------------------------
static bool test19_long_and_empty_callsigns()
{
    printf("=== Test 19: over-long and empty callsigns ===\n");

    struct Case { const char* sent; const char* expected; };
    const Case cases[] = {
        {"W4XYZ5678AB", "W4XYZ567"}, // 11 characters: only 8 fit in a frame
        {"VK3TPM/P/MM", "VK3TPM/P"},
        {"", ""},
    };

    bool ok = true;
    for (const auto& c : cases) {
        rade_text_t tx = rade_text_create();
        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, c.sent, (int)strlen(c.sent), syms, TOTAL_FLOATS);
        rade_text_destroy(tx);

        RxState state = receiveFrame(syms);
        bool passed = state.callCount == 1 && state.received == c.expected;
        printf("  '%s' -> '%s' (%d callbacks): %s\n", c.sent, state.received.c_str(), state.callCount,
               passed ? "PASS" : "FAIL");
        ok &= passed;
    }

    printf("Over-long and empty callsigns: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}
// ---------------------------------------------------------------------------
// Test 20: strlength larger than the string: generation must stop at the
//          string's own terminator, so the frame decodes back to "AB"
// ---------------------------------------------------------------------------
static bool test20_string_shorter_than_strlength()
{
    printf("=== Test 20: strlength larger than the string ===\n");

    rade_text_t tx = rade_text_create();
    float syms[TOTAL_FLOATS];
    memset(syms, 0, sizeof(syms));
    rade_text_generate_tx_string(tx, "AB", 8, syms, TOTAL_FLOATS);
    rade_text_destroy(tx);

    RxState state = receiveFrame(syms);
    bool ok = state.callCount == 1 && state.received == "AB";
    printf("Received '%s' (%d callbacks): %s\n\n", state.received.c_str(), state.callCount,
           ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 21: a valid frame arrives on an instance that never had a rx callback
//          set: the text is decoded but nothing is reported
// ---------------------------------------------------------------------------
static bool test21_valid_frame_without_rx_callback()
{
    printf("=== Test 21: valid frame received with no callback set ===\n");

    rade_text_t tx = rade_text_create();
    float syms[TOTAL_FLOATS];
    memset(syms, 0, sizeof(syms));
    rade_text_generate_tx_string(tx, "K6AQ", 4, syms, TOTAL_FLOATS);
    rade_text_destroy(tx);

    // Same frame through a normal receiver, to prove it is decodable.
    RxState state = receiveFrame(syms);
    bool decoded = state.callCount == 1 && state.received == "K6AQ";

    // A receiver without a callback must swallow it silently.
    rade_text_t rx = rade_text_create();
    float copy[TOTAL_FLOATS];
    memcpy(copy, syms, sizeof(copy));
    rade_text_rx(rx, copy, TOTAL_SYMS);
    rade_text_destroy(rx);

    bool ok = decoded && state.callCount == 1;
    printf("Callback receiver: '%s' (%d callbacks); callback-less receiver: silent: %s\n\n",
           state.received.c_str(), state.callCount, ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 22: symSize exactly equal to the LDPC payload (112 floats): no filler
//          may be written past the payload, and the payload itself is the
//          same as with a larger symSize
// ---------------------------------------------------------------------------
static bool test22_sym_size_equal_to_payload()
{
    printf("=== Test 22: symSize equal to the LDPC payload size ===\n");

    rade_text_t tx = rade_text_create();
    float full[TOTAL_FLOATS];
    memset(full, 0, sizeof(full));
    rade_text_generate_tx_string(tx, "K6AQ", 4, full, TOTAL_FLOATS);

    float trimmed[TOTAL_FLOATS];
    memset(trimmed, 0, sizeof(trimmed));
    trimmed[PAYLOAD_FLOATS] = 7.0f; // sentinel: must survive, nothing written there
    rade_text_generate_tx_string(tx, "K6AQ", 4, trimmed, PAYLOAD_FLOATS);
    rade_text_destroy(tx);

    bool ok = trimmed[PAYLOAD_FLOATS] == 7.0f &&
              memcmp(trimmed, full, PAYLOAD_FLOATS * sizeof(float)) == 0;
    printf("Payload identical to full-size generation, nothing written past float 112: %s\n\n",
           ok ? "PASS" : "FAIL");
    return ok;
}


int main()
{
    bool success = true;

    success &= test1_noiseless_callsigns();
    success &= test2_lowercase_normalized();
    success &= test3_heavy_noise_no_callback();
    success &= test4_crc_blocks_wrong_callsign();
    success &= test5_idempotent_generate_receive();
    success &= test6_filler_symbols_no_crash();
    success &= test7_character_encoding_coverage();
    success &= test8_mild_noise();
    success &= test9_sigma02_robust();
    success &= test10_sigma03_reliable();
    success &= test11_sigma05_cliff();
    success &= test12_sigma07_no_false_positive();
    success &= test13_high_noise_no_false_positive();
    test14_noise_sweep_diagnostic();   // informational, not in success
    success &= test15_on_air_format();
    success &= test16_valid_codeword_bad_crc_rejected();
    success &= test17_undefined_codes_dropped();
    success &= test18_unencodable_characters_skipped();
    success &= test19_long_and_empty_callsigns();
    success &= test20_string_shorter_than_strlength();
    success &= test21_valid_frame_without_rx_callback();
    success &= test22_sym_size_equal_to_payload();

    printf("=== Overall: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
