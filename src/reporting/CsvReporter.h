#ifndef CSV_REPORTER_H
#define CSV_REPORTER_H

//=========================================================================
// Name:            CsvReporter.h
// Purpose:         Reports received callsigns to a CSV file.
//
// Authors:         Mooneer Salem
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

#include <atomic>
#include <fstream>
#include <string>
#include "IReporter.h"

class CsvReporter : public IReporter
{
public:
    CsvReporter(std::string const& filename);
    virtual ~CsvReporter() = default;

    virtual void freqChange(uint64_t frequency) override;

    // The sideband the radio's on, written in the sideband column of each
    // record from then on ("USB", "LSB", or empty if not known).
    virtual void sidebandChange(Sideband sideband) override;
    virtual void transmit(std::string mode, bool tx) override;
    virtual void inAnalogMode(bool inAnalog) override;
    virtual void addReceiveRecord(std::string callsign, std::string mode, uint64_t frequency, signed char snr) override;
    virtual void send() override;

private:
    std::ofstream file_;

    // Whether the file has the sideband column: files started before it
    // was added keep to the columns their header names.
    bool hasSidebandColumn_;
    std::atomic<Sideband> sideband_;
};

#endif // CSV_REPORTER_H
