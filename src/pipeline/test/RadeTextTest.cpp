//==========================================================================
// Name:            RadeTextTest.cpp
//
// Purpose:         Tests RADE callsign encoding and decoding.
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

#include <cassert>
#include <climits>
#include <random>
#include <string>
#include "../RADEReceiveStep.h"
#include "../RADETransmitStep.h"
#include "../rade_text.h"
#include "../../util/logging/ulog.h"

bool testPassed = false;
std::string utRxFeatureFile;
std::string utTxFeatureFile;

std::default_random_engine generator;
std::uniform_real_distribution<double> distribution(-0.789, 0.789);

void OnRadeTextRx(rade_text_t rt, const char* txt_ptr, int length, void* state) 
{
    log_info("Callsign received: %s, length = %d", txt_ptr, length);
    testPassed = !strncmp(txt_ptr, "K6AQ", length);
}

void addNoise(short* ptr, int size)
{
    for (int i = 0; i < size; i++)
    {
        double noise = 16384 * distribution(generator);
        noise = (noise + (double)ptr[i]) / 0.707;
        if (noise > SHRT_MAX) noise = SHRT_MAX;
        else if (noise < SHRT_MIN) noise = SHRT_MIN;
        ptr[i] = noise;
    }
}

int main()
{
    struct rade* rade;
    LPCNetEncState *encState;
    FARGANState fargan;

    // Initialize FARGAN
    float zeros[320] = {0};
    float in_features[5*NB_TOTAL_FEATURES] = {0};
    fargan_init(&fargan);
    fargan_cont(&fargan, zeros, in_features);
    encState = lpcnet_encoder_create();
    assert(encState != nullptr);

    // Initialize RADE
    char modelFile[1];
    modelFile[0] = 0;
    rade_initialize();
    rade = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_MODE_V2);
    assert(rade != nullptr);

    // Initialize RADE text
    rade_text_t txt = rade_text_create();
    assert(txt != nullptr);
    rade_text_enable_stats_output(txt, 1);
    rade_text_generate_tx_string(txt, "K6AQ", 4);
    rade_text_set_rx_callback(txt, OnRadeTextRx, nullptr);

    // Initialize RADE steps
    RADEReceiveStep* recvStep = new RADEReceiveStep(
        rade, &fargan, txt, +[](RADEReceiveStep*) FREEDV_NONBLOCKING { },
        +[]() FREEDV_NONBLOCKING { return 0.0f; /* no freq shift */ });
    assert(recvStep != nullptr);
    RADETransmitStep* txStep = new RADETransmitStep(rade, encState, txt);
    assert(txStep != nullptr);

    // "Transmit" several seconds of audio (including EOO) and immediately
    // receive it. The callsign streams continuously at ~25 bits/s, so this
    // needs to run well past one full 112-bit codeword cycle (~4.5s) to give
    // the receiver a chance to acquire sync and decode at least once.
    int nout = 0;
    int noutRx = 0;
    constexpr int SAMPLES_PER_TX = 1638; // 100ms @ 16 kHz sample rate
    short* inputSamples = new short[SAMPLES_PER_TX];
    assert(inputSamples != nullptr);
    memset(inputSamples, 0, sizeof(short) * SAMPLES_PER_TX);
    auto inputSamplesPtr = std::unique_ptr<short[]>(inputSamples);
    for (int count = 0; count < 100; count++)
    {
        auto outputSamples = txStep->execute(inputSamplesPtr.get(), SAMPLES_PER_TX, &nout);
        addNoise(outputSamples, nout);
        recvStep->execute(outputSamples, nout, &noutRx);
    }

    txStep->restartVocoder();
    while (nout > 0)
    {
        auto outputSamples = txStep->execute(inputSamplesPtr.get(), 0, &nout);
        addNoise(outputSamples, nout);
        recvStep->execute(outputSamples, nout, &noutRx);
    }

    // Send silence through to RX step to trigger EOO processing
    recvStep->execute(inputSamplesPtr.get(), SAMPLES_PER_TX, &noutRx);

    delete recvStep;
    delete txStep;

    rade_close(rade); 
    rade_finalize();

    rade_text_destroy(txt);
    lpcnet_encoder_destroy(encState);

    return testPassed ? 0 : 1;
}
