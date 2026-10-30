//==========================================================================
// Name:            RadeTextUnitTest.cpp
//
// Purpose:         Unit tests for rade_text encode/decode without the full
//                  RADE audio pipeline.  Tests exercise character encoding,
//                  LDPC encode/decode, interleaving, CRC validation, and the
//                  complete generate->stream->receive round-trip over the
//                  RADEV2 continuous 25 bits/s data-symbol channel.
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
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Codeword length: LDPC(112,56): 112 BPSK symbols = 112 floats (one float
// per bit). A decode is only attempted once rade_text_rx_symbol() has seen
// this many symbols; matches LDPC_TOTAL_SIZE_BITS in rade_text.cpp.
static constexpr int CODEWORD_SYMS = 112;

// rade_text_rx_symbol() spreads its rotation-sweep search across several
// calls rather than deciding the instant the buffer fills (see
// ROTATIONS_PER_CALL in rade_text.cpp), so tests need to feed some margin
// of symbols beyond one codeword before a decode can complete. This margin
// is generous enough to accommodate that internal chunk size without
// hardcoding it here.
static constexpr int SWEEP_MARGIN = 20;
static constexpr int SWEEP_COMPLETE_SYMS = CODEWORD_SYMS + SWEEP_MARGIN;

struct RxState {
    std::string received;
    int callCount = 0;
    std::vector<std::string> allReceived;
};

static void onTextRx(rade_text_t, const char* txt, int len, void* state)
{
    auto* s = reinterpret_cast<RxState*>(state);
    s->received.assign(txt, len);
    s->callCount++;
    s->allReceived.emplace_back(txt, len);
}

// Pull `count` streamed BPSK symbols out of a freshly generated tx object.
static std::vector<float> pullSymbols(rade_text_t tx, int count)
{
    std::vector<float> syms(count);
    for (int i = 0; i < count; i++)
        syms[i] = rade_text_tx_next_symbol(tx);
    return syms;
}

// Number of RADE_TEXT_CHARS_PER_BLOCK=8-character blocks a callsign needs
// (matches rade_text.cpp's chunking; every character used in these tests is
// in the supported alphabet, so length maps 1:1 to OTA symbol count).
static int expectedBlocks(const char* callsign)
{
    int len = (int)strlen(callsign);
    int blocks = std::max(1, (len + 7) / 8);
    return std::min(blocks, 4); // matches RADE_TEXT_MAX_BLOCKS in rade_text.cpp
}

// Symbol budget for a full round trip of a callsign that may span multiple
// blocks: one extra full multi-block cycle beyond the initial fill/sweep is
// enough to guarantee every block has been seen at least once via the
// naturally-sliding fallback, even in the worst case where the rotation
// sweep first locks onto the last block in the sequence.
static int symbolsNeededFor(const char* callsign)
{
    int numBlocks = expectedBlocks(callsign);
    return CODEWORD_SYMS * (numBlocks + 1) + SWEEP_MARGIN;
}

// Add Gaussian noise to a float symbol array.
static void addNoiseToSyms(std::vector<float>& syms, float sigma, std::mt19937& rng)
{
    std::normal_distribution<float> nd(0.0f, sigma);
    for (auto& s : syms)
        s += nd(rng);
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Encode callsign, optionally add noise, then stream enough symbols for the
// rotation sweep to complete (aligned to the codeword boundary, so the
// sweep's very first chunk contains the correct rotation) into a freshly
// created rx object. Returns whether the callsign was recovered and every
// delivered decode (there may be more than one -- the sweep's decision
// plus any subsequent fallback hits on the trailing margin symbols) was
// correct.
static bool roundTrip(const char* callsign, float sigma = 0.0f, unsigned seed = 42, int totalSyms = SWEEP_COMPLETE_SYMS)
{
    rade_text_t tx = rade_text_create();
    rade_text_t rx = rade_text_create();
    assert(tx && rx);
    rade_text_enable_stats_output(tx, 0);
    rade_text_enable_stats_output(rx, 0);

    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);

    rade_text_generate_tx_string(tx, callsign, (int)strlen(callsign));
    auto syms = pullSymbols(tx, totalSyms);

    if (sigma > 0.0f) {
        std::mt19937 rng(seed);
        addNoiseToSyms(syms, sigma, rng);
    }

    for (float s : syms)
        rade_text_rx_symbol(rx, s);

    rade_text_destroy(tx);
    rade_text_destroy(rx);

    if (state.callCount < 1) return false;
    for (auto& r : state.allReceived) {
        if (r != callsign) return false;
    }
    return true;
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

        rade_text_generate_tx_string(tx, c.input, (int)strlen(c.input));
        auto syms = pullSymbols(tx, SWEEP_COMPLETE_SYMS);
        for (float s : syms) rade_text_rx_symbol(rx, s);

        bool passed = (state.callCount >= 1 && state.received == c.expected);
        printf("  '%s' -> '%s' (expected '%s')  %s\n",
               c.input, state.received.c_str(), c.expected, passed ? "PASS" : "FAIL");
        ok &= passed;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }
    printf("Lowercase normalisation: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 3: Heavy noise causes decode failure (callback never fires)
// ---------------------------------------------------------------------------
static bool test3_heavy_noise_no_callback()
{
    printf("=== Test 3: heavy noise - callback must not fire ===\n");

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

        rade_text_generate_tx_string(tx, cs, (int)strlen(cs));
        auto syms = pullSymbols(tx, SWEEP_COMPLETE_SYMS);

        std::mt19937 rng(seed * 1234567u);
        addNoiseToSyms(syms, 5.0f, rng);

        for (float s : syms) rade_text_rx_symbol(rx, s);

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
    const int TRIALS = CODEWORD_SYMS;

    for (int flip = 0; flip < TRIALS; flip++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        rade_text_generate_tx_string(tx, cs, (int)strlen(cs));
        auto syms = pullSymbols(tx, SWEEP_COMPLETE_SYMS);

        // Negate one float inside the payload region. This lands within
        // the first CODEWORD_SYMS symbols, which is exactly what the
        // rotation sweep snapshots and tests -- the margin symbols beyond
        // that are an uncorrupted continuation used only by the fallback
        // path once the sweep completes.
        syms[flip] = -syms[flip];

        for (float s : syms) rade_text_rx_symbol(rx, s);

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
// Test 5: Encode/decode consistency - generate then receive without touching
//         symbols must always fire the callback with the original callsign
// ---------------------------------------------------------------------------
static bool test5_idempotent_generate_receive()
{
    printf("=== Test 5: idempotent generate->receive (callback always fires correctly) ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL", "KA1BCD"};
    bool ok = true;

    for (const char* cs : callsigns) {
        for (int repeat = 0; repeat < 3; repeat++) {
            // Re-create objects each time to exercise fresh state.
            bool passed = roundTrip(cs);
            if (!passed) {
                printf("  FAIL callsign='%s' repeat=%d\n", cs, repeat);
                ok = false;
            }
        }
    }
    printf("Idempotent generate->receive: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 6: Continuous streaming - the transmitter loops the same codeword
//         indefinitely, so a receiver that keeps feeding symbols should
//         decode again on every subsequent full cycle, not just the first.
// ---------------------------------------------------------------------------
static bool test6_continuous_streaming_redetects()
{
    printf("=== Test 6: continuous streaming re-detects across multiple cycles ===\n");

    const int CYCLES = 5;
    rade_text_t tx = rade_text_create();
    rade_text_t rx = rade_text_create();
    rade_text_enable_stats_output(tx, 0);
    rade_text_enable_stats_output(rx, 0);

    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);

    rade_text_generate_tx_string(tx, "K6AQ", 4);
    auto syms = pullSymbols(tx, CODEWORD_SYMS * CYCLES);
    for (float s : syms) rade_text_rx_symbol(rx, s);

    // The window becomes codeword-aligned once per CODEWORD_SYMS symbols
    // once it first fills, so at least CYCLES decodes are expected (possibly
    // more if the code happens to also converge on a rotated window, which
    // is why this checks a floor rather than an exact count). The one
    // invariant that must always hold is that every delivered decode is
    // correct -- CRC guards against a rotated/misaligned window ever
    // delivering the wrong content.
    int wrongCount = 0;
    for (auto& s : state.allReceived) {
        if (s != "K6AQ") wrongCount++;
    }
    bool ok = (state.callCount >= CYCLES) && (wrongCount == 0);
    printf("  callCount=%d (expected >= %d), wrongCount=%d  %s\n",
           state.callCount, CYCLES, wrongCount, ok ? "PASS" : "FAIL");

    rade_text_destroy(tx);
    rade_text_destroy(rx);

    printf("Continuous streaming: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 7: Low-level character encoding round-trip
//         Encode callsign to OTA then decode back; check identity.
//         This is tested indirectly through a generate->receive cycle that uses
//         each character type: letters, digits, ASCII 38-46 punctuation, '/'.
// ---------------------------------------------------------------------------
static bool test7_character_encoding_coverage()
{
    printf("=== Test 7: character encoding coverage ===\n");

    // Characters in the 38-symbol OTA alphabet:
    //   ASCII '0'-'9' -> OTA 1-10
    //   ASCII 'A'-'Z' -> OTA 11-36
    //   ASCII '/'     -> OTA 37
    // (0 = null, used only as terminator/padding, not an enterable character.)
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

    // Exhaustively test every individual character in the 38-symbol OTA
    // alphabet ('0'-'9', 'A'-'Z', and '/') round-trips on its own as a
    // single-character string.
    printf("  -- exhaustive single-character sweep --\n");
    std::string allChars;
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

    // Group every valid character into 8-char (one block's worth) strings to
    // also exercise multi-character combinations of digits, letters, and '/'
    // together.
    printf("  -- grouped 8-character sweep --\n");
    constexpr size_t CHARS_PER_BLOCK = 8;  // matches RADE_TEXT_CHARS_PER_BLOCK in rade_text.cpp
    int group_fail = 0;
    int group_total = 0;
    for (size_t i = 0; i < allChars.size(); i += CHARS_PER_BLOCK) {
        std::string chunk = allChars.substr(i, CHARS_PER_BLOCK);
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
// Test 8: Mild noise - at sigma=0.1 the decoder should still succeed
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

// Run TRIALS encode->noise->decode cycles for one callsign at one sigma.
// Returns tallied counts.
static NoiseTrialResult noiseTrials(const char* cs, float sigma,
                                    int trials, unsigned base_seed = 0)
{
    NoiseTrialResult r{};
    r.trials = trials;

    for (int t = 0; t < trials; t++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        rade_text_generate_tx_string(tx, cs, (int)strlen(cs));
        auto syms = pullSymbols(tx, SWEEP_COMPLETE_SYMS);

        std::mt19937 rng(base_seed + (unsigned)t * 131071u + 3u);
        addNoiseToSyms(syms, sigma, rng);

        for (float s : syms) rade_text_rx_symbol(rx, s);

        if (state.callCount > 0) {
            r.cb_any++;

            // A trial can now deliver more than once (the sweep's decision
            // plus any fallback hits on the trailing margin symbols), so
            // classify by whether *any* delivery in the trial was wrong --
            // the safety property under test is "never let wrong content
            // through", not just "what was the last thing delivered".
            bool anyWrong = false;
            for (auto& rcv : state.allReceived) {
                if (rcv != cs) { anyWrong = true; break; }
            }
            if (anyWrong) r.cb_wrong++;
            else           r.correct++;
        }

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Test 9: sigma=0.2 (~14 dB SNR) - robust above the floor
// ---------------------------------------------------------------------------
static bool test9_sigma02_robust()
{
    printf("=== Test 9: sigma=0.2 (~14 dB) - should decode reliably ===\n");

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
// Test 10: sigma=0.3 (~10 dB SNR) - near the reliable operational limit
// ---------------------------------------------------------------------------
static bool test10_sigma03_reliable()
{
    printf("=== Test 10: sigma=0.3 (~10 dB) - should still succeed most of the time ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.3f, 30, 2000u);
        // Require >=90% success and zero wrong-callsign deliveries.
        bool passed = (r.correct >= 27) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.3 reliable: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 11: sigma=0.5 (~6 dB SNR) - performance cliff; require >=40% success
//
// BPSK's exact closed-form LLR (2*a*r/sigma^2) produces larger-magnitude LLRs
// than QPSK's max-log-map approximation did at the same sigma, which pushes
// this operating point closer to the known phi() precision limitation that
// collapses BP messages at high |LLR|. Wrong-callsign deliveries should
// still be rare here (see Test 12/13 for the accepted small residual rate
// at even higher noise, a deliberate tradeoff for the exhaustive rotation
// search's faster acquisition -- see rade_text_rx_symbol()).
// ---------------------------------------------------------------------------
static bool test11_sigma05_cliff()
{
    printf("=== Test 11: sigma=0.5 (~6 dB) - performance cliff, >=40%% expected ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.5f, 40, 3000u);
        // Require >=40% success and zero wrong-callsign deliveries.
        bool passed = (r.correct >= 16) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.5 cliff: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 12: sigma=0.7 - marginal noise; wrong-callsign deliveries must stay rare
//
// At this level the LDPC decoder mostly fails to converge (known limitation of
// the current phi() implementation at high SNR causing BP messages to collapse).
// Success rate may be low; the CRC layer (plus the uniqueness/fast-convergence
// gates on the exhaustive rotation search -- see rade_text_rx_symbol()) keeps
// incorrectly decoded callsigns from reaching the application in all but a
// small fraction of attempts. That small residual is an accepted tradeoff:
// the exhaustive rotation search tests LDPC_TOTAL_SIZE_BITS hypotheses against
// the same noisy samples in one burst (versus one hypothesis at a time
// previously), so CRC8's ~1/256 false-accept rate gets more chances to slip
// through per acquisition attempt at extreme noise. It only shows up this far
// past the reliable operating range (Tests 9-11 remain at zero wrong
// deliveries), so a MAX_WRONG_RATIO ceiling is used here instead of requiring
// exactly zero.
// ---------------------------------------------------------------------------
static bool test12_sigma07_no_false_positive()
{
    printf("=== Test 12: sigma=0.7 - wrong-callsign deliveries must stay rare at marginal noise ===\n");

    // Test with several callsigns to cover a range of bit patterns.
    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL", "KA1BCD", "W4XYZ567"};
    constexpr float MAX_WRONG_RATIO = 0.05f; // accepted residual: <=5% of callbacks wrong
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

    bool ok = (total_cb == 0) || ((float)total_wrong / total_cb <= MAX_WRONG_RATIO);
    printf("Total correct=%d  cb_any=%d  cb_wrong=%d\n",
           total_pass, total_cb, total_wrong);
    printf("Wrong-callsign rate at sigma=0.7: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 13: sigma=1.0, 1.5, 2.0 - beyond operational limit
//
// At these noise levels the raw bit-error rate overwhelms the LDPC code and
// decode almost always fails. As with Test 12, a small residual chance of a
// wrong-callsign delivery is an accepted tradeoff of the exhaustive rotation
// search's faster acquisition at extreme noise (see comment above Test 12);
// a MAX_WRONG_RATIO ceiling is used rather than requiring exactly zero.
// ---------------------------------------------------------------------------
static bool test13_high_noise_no_false_positive()
{
    printf("=== Test 13: sigma=1.0/1.5/2.0 - beyond limit, wrong-callsign rate stays low ===\n");

    struct { float sigma; const char* label; } levels[] = {
        {1.0f, "1.0 (~0 dB)"},
        {1.5f, "1.5 (~-3 dB)"},
        {2.0f, "2.0 (~-6 dB)"},
    };
    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "AA0ZZ", "N0CALL"};
    constexpr float MAX_WRONG_RATIO = 0.15f; // accepted residual: <=15% of callbacks wrong

    bool ok = true;
    for (auto& lv : levels) {
        int wrong = 0, cb = 0, pass = 0;
        for (const char* cs : callsigns) {
            auto r = noiseTrials(cs, lv.sigma, 20, 5000u);
            wrong += r.cb_wrong;
            cb    += r.cb_any;
            pass  += r.correct;
        }
        bool level_ok = (cb == 0) || ((float)wrong / cb <= MAX_WRONG_RATIO);
        printf("  sigma=%-12s  correct=%d  cb_any=%d  cb_wrong=%d  %s\n",
               lv.label, pass, cb, wrong, level_ok ? "PASS" : "FAIL");
        ok &= level_ok;
    }
    printf("High-noise wrong-callsign rate: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 14: Noise sweep diagnostic (informational - not in pass/fail)
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
// Test 15: Mid-cycle join decodes without waiting for a full extra cycle.
//
// A receiver that starts listening partway through the repeating codeword
// (rather than exactly at its start) must still decode well within one
// extra cycle -- not after waiting up to another full CODEWORD_SYMS-symbol
// cycle for the window to naturally slide back into alignment. This
// directly validates the rotation-sweep optimization in
// rade_text_rx_symbol(), which -- since it spreads its rotation search
// across several calls rather than testing everything the instant the
// buffer fills (to bound the real-time cost added to any single modem
// frame) -- completes slightly after CODEWORD_SYMS symbols rather than
// exactly at CODEWORD_SYMS. SWEEP_COMPLETE_SYMS gives it enough room to
// finish regardless of the internal chunk size, without hardcoding that
// size here.
// ---------------------------------------------------------------------------
static bool test15_rotated_first_shot()
{
    printf("=== Test 15: mid-cycle join decodes without a full extra cycle ===\n");

    bool ok = true;
    const char* cs = "K6AQ";

    // Spread of join offsets across the cycle, including 0 (already
    // exercised elsewhere) and near the far end.
    int offsets[] = {0, 1, 17, 55, 56, 90, 111};

    for (int off : offsets) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        rade_text_generate_tx_string(tx, cs, (int)strlen(cs));
        auto syms = pullSymbols(tx, CODEWORD_SYMS);

        // Simulate joining `off` symbols into the cycle: feed the
        // cyclically-rotated sequence a real receiver would see instead
        // of the one starting at the codeword's logical position 0. Feed
        // enough total symbols (beyond one cycle) for the rotation sweep
        // to complete; `syms` is periodic with period CODEWORD_SYMS so
        // indexing with `% CODEWORD_SYMS` extends it seamlessly.
        std::vector<float> rotatedSyms(SWEEP_COMPLETE_SYMS);
        for (int i = 0; i < SWEEP_COMPLETE_SYMS; i++)
            rotatedSyms[i] = syms[(i + off) % CODEWORD_SYMS];

        for (float s : rotatedSyms) rade_text_rx_symbol(rx, s);

        // A pass here means the decode succeeded well within one extra
        // cycle of the theoretical CODEWORD_SYMS floor, regardless of join
        // offset -- i.e. via the rotation sweep, not the natural-alignment
        // fallback (which could take up to another full cycle on its own).
        // Feeding SWEEP_MARGIN extra symbols past sweep completion can let
        // the (now-active) fallback also hit a naturally-aligned window
        // and deliver a second, redundant-but-correct decode -- that's
        // expected given this test's earlier-established "fire on every
        // successful decode" callback policy, not a failure, so this
        // checks "at least one delivery, all of them correct" rather than
        // requiring exactly one.
        bool passed = (state.callCount >= 1);
        for (auto& r : state.allReceived) {
            if (r != cs) passed = false;
        }
        printf("  offset=%-4d  callCount=%d  received='%s'  %s\n",
               off, state.callCount, state.received.c_str(), passed ? "PASS" : "FAIL");
        ok &= passed;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    printf("Mid-cycle join first-shot: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 16: Multi-block round-trip for compound callsigns (prefix/suffix
//          combinations longer than one block's 8 characters), plus the
//          32-character (RADE_TEXT_MAX_LENGTH) boundary and truncation
//          beyond it.
// ---------------------------------------------------------------------------
static bool test16_multi_block_compound_callsigns()
{
    printf("=== Test 16: multi-block round-trip for compound callsigns ===\n");

    const char* callsigns[] = {
        "VE3/KG6AOV",                          // 10 chars, 2 blocks -- country prefix
        "KG6AOV/MM",                           // 9 chars, 2 blocks -- maritime mobile suffix
        "VE3/KG6AOV/MM",                       // 13 chars, 2 blocks -- prefix and suffix stacked
        "F/G3ABC/P",                           // 9 chars, 2 blocks
        "AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD",    // 32 chars, exactly 4 full blocks (RADE_TEXT_MAX_LENGTH)
        "AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDE",   // 33 chars -- one past max, must truncate to 32
    };

    bool ok = true;
    for (const char* cs : callsigns) {
        int numBlocks = expectedBlocks(cs);
        int totalSyms = symbolsNeededFor(cs);

        std::string expected(cs);
        if (expected.size() > 32) expected.resize(32);

        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        rade_text_generate_tx_string(tx, cs, (int)strlen(cs));
        auto syms = pullSymbols(tx, totalSyms);
        for (float s : syms) rade_text_rx_symbol(rx, s);

        bool passed = (state.callCount >= 1);
        for (auto& r : state.allReceived) {
            if (r != expected) passed = false;
        }
        printf("  %-36s (%d blocks)  received='%s'  %s\n",
               cs, numBlocks, state.received.c_str(), passed ? "PASS" : "FAIL");
        ok &= passed;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    printf("Multi-block compound callsigns: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 17: Multi-block reassembly across mid-cycle join offsets.
//
// A receiver joining partway through a multi-block cycle may have its
// rotation sweep lock onto any block in the sequence first (not
// necessarily block 0), so reassembly must handle blocks arriving out of
// order -- this validates rade_text_ingest_block_()'s out-of-order handling
// directly, complementing test15's single-block coverage of the rotation
// sweep itself.
// ---------------------------------------------------------------------------
static bool test17_multi_block_mid_cycle_join()
{
    printf("=== Test 17: multi-block reassembly across mid-cycle join offsets ===\n");

    bool ok = true;
    const char* cs = "VE3/KG6AOV"; // 10 chars -> 2 blocks
    int numBlocks = expectedBlocks(cs);
    int cycleLen = CODEWORD_SYMS * numBlocks;

    // Offsets spanning the first block, the second block, and the seam
    // between them, so the rotation sweep's first lock can land on either
    // block depending on where in the cycle the receiver joins.
    int offsets[] = {0, 1, CODEWORD_SYMS - 1, CODEWORD_SYMS, CODEWORD_SYMS + 1, cycleLen - 1};

    for (int off : offsets) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        rade_text_generate_tx_string(tx, cs, (int)strlen(cs));
        auto syms = pullSymbols(tx, cycleLen);

        int totalSyms = symbolsNeededFor(cs);
        std::vector<float> rotatedSyms(totalSyms);
        for (int i = 0; i < totalSyms; i++)
            rotatedSyms[i] = syms[(i + off) % cycleLen];

        for (float s : rotatedSyms) rade_text_rx_symbol(rx, s);

        bool passed = (state.callCount >= 1);
        for (auto& r : state.allReceived) {
            if (r != cs) passed = false;
        }
        printf("  offset=%-4d  callCount=%d  received='%s'  %s\n",
               off, state.callCount, state.received.c_str(), passed ? "PASS" : "FAIL");
        ok &= passed;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    printf("Multi-block mid-cycle join: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 18: Receive combining recovers a callsign below the single-copy
// threshold.
//
// At sigma=1.2 (Es/N0 ~ -4.6 dB) a single codeword essentially never
// decodes, but the transmitter repeats it continuously, so the receiver's
// combining accumulators should build up enough SNR over several cycles to
// recover it -- with or without the confirmation filter, which only adds
// one codeword of latency. The same noisy symbols are fed to a receiver with
// combining disabled for comparison (informational only -- it's expected to
// mostly fail).
// ---------------------------------------------------------------------------
static bool test18_rx_combining_low_snr()
{
    printf("=== Test 18: receive combining below single-copy threshold ===\n");

    const char* callsign = "KG6AOV";
    const float sigma = 1.2f;
    const int TRIALS = 6;
    const int CYCLES = 12;
    int combinedOk = 0, confirmedOk = 0, plainOk = 0, wrong = 0;

    rade_text_t tx = rade_text_create();
    rade_text_generate_tx_string(tx, callsign, (int)strlen(callsign));
    auto clean = pullSymbols(tx, CODEWORD_SYMS);
    rade_text_destroy(tx);

    for (int t = 0; t < TRIALS; t++)
    {
        std::mt19937 rng(1800 + t);
        std::vector<float> syms(CODEWORD_SYMS * CYCLES);
        for (int i = 0; i < (int)syms.size(); i++)
            syms[i] = clean[(i + 37 * t) % CODEWORD_SYMS]; // vary join point
        addNoiseToSyms(syms, sigma, rng);

        // 0 = no combining, 1 = combining, 2 = combining + confirmation filter
        for (int combine = 0; combine <= 2; combine++)
        {
            rade_text_t rx = rade_text_create();
            rade_text_enable_rx_combining(rx, combine != 0);
            rade_text_enable_rx_combine_confirm(rx, combine == 2);
            RxState state;
            rade_text_set_rx_callback(rx, onTextRx, &state);
            for (float s : syms)
                rade_text_rx_symbol(rx, s);
            rade_text_destroy(rx);

            for (auto& r : state.allReceived)
                if (r != callsign) wrong++;
            if (state.received == callsign)
                (combine == 2 ? confirmedOk : combine == 1 ? combinedOk : plainOk)++;
        }
    }

    bool ok = combinedOk >= TRIALS - 1 && confirmedOk >= TRIALS - 1 && wrong == 0;
    printf("  combining: %d/%d  with confirm: %d/%d  without: %d/%d  wrong decodes: %d\n",
           combinedOk, TRIALS, confirmedOk, TRIALS, plainOk, TRIALS, wrong);
    printf("Receive combining low SNR: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 19: rade_text_rx_reset() discards everything from before the reset.
//
// After receiving one callsign, a reset followed by a different callsign
// must deliver only the new one -- nothing left in the raw window,
// combining accumulators or reassembly state may resurface the old one.
// ---------------------------------------------------------------------------
static bool test19_rx_reset()
{
    printf("=== Test 19: rx reset discards previous state ===\n");

    rade_text_t txA = rade_text_create();
    rade_text_t txB = rade_text_create();
    rade_text_generate_tx_string(txA, "W1AW", 4);
    rade_text_generate_tx_string(txB, "K6AQ", 4);
    auto symsA = pullSymbols(txA, CODEWORD_SYMS * 4);
    auto symsB = pullSymbols(txB, CODEWORD_SYMS * 4);
    rade_text_destroy(txA);
    rade_text_destroy(txB);

    rade_text_t rx = rade_text_create();
    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);

    for (float s : symsA)
        rade_text_rx_symbol(rx, s);
    bool gotA = state.received == "W1AW";

    rade_text_rx_reset(rx);
    state = RxState();

    for (float s : symsB)
        rade_text_rx_symbol(rx, s);
    rade_text_destroy(rx);

    bool onlyB = state.callCount > 0;
    for (auto& r : state.allReceived)
        if (r != "K6AQ") onlyB = false;

    bool ok = gotA && onlyB;
    printf("  before reset: '%s'  after reset: %d callbacks, all K6AQ: %s\n",
           gotA ? "W1AW" : "(none)", state.callCount, onlyB ? "yes" : "no");
    printf("RX reset: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Reference encoder: builds one block's on-air BPSK symbols independently
// of rade_text, from the documented layout, so tests can pin the format and
// send blocks the encoder never would (e.g. a wrong CRC).
// Payload (56 bits, LSB first per field): [CRC-8 | last_block | block_index
// (2) | 8 characters packed base 38 (42) | reserved (3)] -> LDPC(112,56) ->
// bit interleave (b = 37 over 112 bits) -> BPSK (0 -> +1, 1 -> -1).
// The CRC (poly 0x1D) covers the framing byte (block_index | last_block<<2)
// followed by the 8 character codes.
// ---------------------------------------------------------------------------
static uint8_t referenceBlockCrc(const uint8_t chars[8], int blockIndex, bool lastBlock)
{
    uint8_t buf[9];
    buf[0] = (uint8_t)(blockIndex | (lastBlock ? 0x4 : 0));
    memcpy(&buf[1], chars, 8);
    uint8_t crc = 0;
    for (uint8_t byte : buf) {
        crc ^= byte;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1);
    }
    return crc;
}

static std::vector<float> referenceBlock(const uint8_t chars[8], int blockIndex, bool lastBlock, uint8_t crc)
{
    uint64_t packed = 0;
    for (int i = 0; i < 8; i++)
        packed = packed * 38 + chars[i];

    std::array<uint8_t, 56> info{};
    for (int i = 0; i < 8; i++) info[i] = (crc >> i) & 1;
    info[8] = lastBlock ? 1 : 0;
    for (int i = 0; i < 2; i++) info[9 + i] = (blockIndex >> i) & 1;
    for (int i = 0; i < 42; i++) info[11 + i] = (packed >> i) & 1;
    auto codeword = ldpc_encode(info);

    std::vector<float> syms(CODEWORD_SYMS);
    for (int i = 0; i < CODEWORD_SYMS; i++)
        syms[(37 * i) % CODEWORD_SYMS] = codeword[i] ? -1.0f : 1.0f;
    return syms;
}

// Character code in the 38-symbol alphabet ('0'-'9' -> 1-10, 'A'-'Z' -> 11-36, '/' -> 37).
static uint8_t otaCode(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 1);
    if (c == '/') return 37;
    return (uint8_t)(c - 'A' + 11);
}

// Streams `syms` repeatedly (as the transmitter does) into a fresh receiver.
static RxState receiveRepeated(const std::vector<float>& syms, int totalSyms)
{
    rade_text_t rx = rade_text_create();
    rade_text_enable_stats_output(rx, 0);
    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);
    for (int i = 0; i < totalSyms; i++)
        rade_text_rx_symbol(rx, syms[i % syms.size()]);
    rade_text_destroy(rx);
    return state;
}

// ---------------------------------------------------------------------------
// Test 20: the encoder's on-air format matches the reference encoder, so
//          changes that would break decoding by other FreeDV versions show up
// ---------------------------------------------------------------------------
static bool test20_on_air_format()
{
    printf("=== Test 20: on-air format matches the reference encoder ===\n");

    const uint8_t chars[8] = {otaCode('K'), otaCode('6'), otaCode('A'), otaCode('Q')};
    auto expected = referenceBlock(chars, 0, true, referenceBlockCrc(chars, 0, true));

    rade_text_t tx = rade_text_create();
    rade_text_enable_stats_output(tx, 0);
    rade_text_generate_tx_string(tx, "K6AQ", 4);
    auto got = pullSymbols(tx, CODEWORD_SYMS);
    rade_text_destroy(tx);

    bool ok = got == expected;
    RxState state = receiveRepeated(expected, SWEEP_COMPLETE_SYMS);
    ok &= state.callCount >= 1 && state.received == "K6AQ";

    printf("On-air format: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 21: a block that decodes cleanly but whose CRC doesn't match its
//          contents (as when noise turns it into a different valid
//          codeword) is dropped rather than shown as a callsign
// ---------------------------------------------------------------------------
static bool test21_valid_codeword_bad_crc_rejected()
{
    printf("=== Test 21: valid codeword with a wrong CRC is rejected ===\n");

    const uint8_t chars[8] = {otaCode('K'), otaCode('6'), otaCode('A'), otaCode('Q')};
    uint8_t crc = referenceBlockCrc(chars, 0, true);
    bool ok = true;
    for (int bit = 0; bit < 8; bit++) {
        auto syms = referenceBlock(chars, 0, true, crc ^ (1 << bit));
        RxState state = receiveRepeated(syms, 4 * CODEWORD_SYMS);
        if (state.callCount != 0) {
            printf("  FAIL: CRC bit %d flipped, got '%s'\n", bit, state.received.c_str());
            ok = false;
        }
    }

    printf("Wrong CRC rejected: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 22: characters outside the alphabet are skipped when encoding, and
//          the rest of the callsign still gets through
// ---------------------------------------------------------------------------
static bool test22_unencodable_characters_skipped()
{
    printf("=== Test 22: characters outside the alphabet are skipped ===\n");

    // ' ', '!', '#', '_', '~', '@' and '-' aren't in the 38-symbol alphabet.
    const char* input = "K6 !#_~@-AQ";
    rade_text_t tx = rade_text_create();
    rade_text_enable_stats_output(tx, 0);
    rade_text_generate_tx_string(tx, input, (int)strlen(input));
    auto syms = pullSymbols(tx, SWEEP_COMPLETE_SYMS);
    rade_text_destroy(tx);
    RxState state = receiveRepeated(syms, SWEEP_COMPLETE_SYMS);

    bool ok = state.callCount >= 1 && state.received == "K6AQ";
    printf("Received '%s' (%d callbacks)\n", state.received.c_str(), state.callCount);
    printf("Unencodable characters skipped: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 23: an empty callsign is sent as one block and received as empty
// ---------------------------------------------------------------------------
static bool test23_empty_callsign()
{
    printf("=== Test 23: empty callsign ===\n");

    rade_text_t tx = rade_text_create();
    rade_text_enable_stats_output(tx, 0);
    rade_text_generate_tx_string(tx, "", 0);
    auto syms = pullSymbols(tx, SWEEP_COMPLETE_SYMS);
    rade_text_destroy(tx);
    RxState state = receiveRepeated(syms, SWEEP_COMPLETE_SYMS);

    bool ok = state.callCount >= 1 && state.received.empty();
    printf("Received '%s' (%d callbacks)\n", state.received.c_str(), state.callCount);
    printf("Empty callsign: %s\n\n", ok ? "PASS" : "FAIL");
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
    success &= test6_continuous_streaming_redetects();
    success &= test7_character_encoding_coverage();
    success &= test8_mild_noise();
    success &= test9_sigma02_robust();
    success &= test10_sigma03_reliable();
    success &= test11_sigma05_cliff();
    success &= test12_sigma07_no_false_positive();
    success &= test13_high_noise_no_false_positive();
    test14_noise_sweep_diagnostic();   // informational, not in success
    success &= test15_rotated_first_shot();
    success &= test16_multi_block_compound_callsigns();
    success &= test17_multi_block_mid_cycle_join();
    success &= test18_rx_combining_low_snr();
    success &= test19_rx_reset();
    success &= test20_on_air_format();
    success &= test21_valid_codeword_bad_crc_rejected();
    success &= test22_unencodable_characters_skipped();
    success &= test23_empty_callsign();

    printf("=== Overall: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
