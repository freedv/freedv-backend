//==========================================================================
// Name:            rade_text.h
//
// Purpose:         Handles reliable text (e.g. text with FEC).
// Created:         December 7, 2024
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

#ifndef RADE_TEXT_H
#define RADE_TEXT_H

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus

    /* Hide internals of rade_text_t. */
    typedef void *rade_text_t;

    /* Function type for callback (when full reliable text has been received). */
    typedef void (*on_text_rx_t)(rade_text_t rt, const char *txt_ptr, int length, void *state);

    /* Allocate rade_text object. */
    rade_text_t rade_text_create();

    /* Destroy rade_text object. */
    void rade_text_destroy(rade_text_t ptr);

    /* Encode str into the internal repeating LDPC-coded bitstream that
       rade_text_tx_next_symbol() streams out, one bit per modem frame. */
    void rade_text_generate_tx_string(rade_text_t ptr, const char *str, int strlength);

    /* Returns the next BPSK data symbol (+1.0 / -1.0) to pass to
       rade_tx_set_data_symbol(). Call once per rade_tx() frame; the
       encoded text loops continuously for as long as this is called. */
    float rade_text_tx_next_symbol(rade_text_t ptr);

    /* Set text RX callback. */
    void rade_text_set_rx_callback(rade_text_t ptr, on_text_rx_t text_rx_fn, void *state);

    /* Feed one soft-decision BPSK data symbol obtained from
       rade_rx_get_data_symbol(). Call once per successful (return > 0)
       rade_rx(). May invoke the RX callback if a valid decode results. */
    void rade_text_rx_symbol(rade_text_t ptr, float sym);

    /* Whether to enable output of stats (i.e. BER). */
    void rade_text_enable_stats_output(rade_text_t ptr, int enable);

    /* Discard all RX state (partially received symbols, combined copies and
       partially reassembled messages). Call when the modem loses sync, since
       the position within the transmitter's cycle is unknown afterward. */
    void rade_text_rx_reset(rade_text_t ptr);

    /* Whether to coherently combine repeated copies of the transmitted
       cycle to decode at lower SNR (enabled by default). */
    void rade_text_enable_rx_combining(rade_text_t ptr, int enable);

    /* Whether a decode that only receive combining produced must be
       confirmed by a second decode of the same block at the same alignment
       before it's delivered (disabled by default). Cancels out the extra
       false decodes combining otherwise adds, at the cost of up to ~1 dB
       and one more codeword (~4.5s) of latency near threshold. */
    void rade_text_enable_rx_combine_confirm(rade_text_t ptr, int enable);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // RADE_TEXT_H