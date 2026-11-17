//=========================================================================
// Name:            CsvReporter.cpp
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

#include <cassert>
#include <cstdlib>
#include <cinttypes>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>
#include "CsvReporter.h"
#include "../util/logging/ulog.h"

namespace
{

// Quotes a field (RFC 4180) if it contains a comma, quote or line break.
// Callsigns come from over-the-air text, whose character set includes ','.
std::string csvField(std::string const& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos)
    {
        return value;
    }

    std::string quoted = "\"";
    for (char c : value)
    {
        if (c == '"')
        {
            quoted += '"';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

} // namespace

namespace
{

const char* CSV_HEADER = "date,time,callsign,mode,frequency_hz,snr_db,sideband";

// The header of files started before the sideband column was added.
const char* CSV_HEADER_WITHOUT_SIDEBAND = "date,time,callsign,mode,frequency_hz,snr_db";

// A file's first line, without a trailing '\r' (or "" if it can't be read).
std::string readHeader(std::string const& filename)
{
    std::ifstream existing(filename);
    std::string header;
    std::getline(existing, header);
    if (!header.empty() && header.back() == '\r')
    {
        header.pop_back();
    }
    return header;
}

// One record of a CSV file, and how many fields it has.
struct CsvRecord
{
    std::string text;
    int fields;
};

// Splits CSV text into its records (RFC 4180: a quoted field can hold
// commas, quotes and line breaks, as callsigns heard over the air can),
// without their line endings.
std::vector<CsvRecord> splitRecords(std::string const& contents)
{
    std::vector<CsvRecord> records;
    std::string current;
    int fields = 1;
    bool quoted = false;
    for (char c : contents)
    {
        if (c == '"')
        {
            // ("" inside a quoted field toggles twice, staying quoted.)
            quoted = !quoted;
        }
        else if (c == ',' && !quoted)
        {
            fields++;
        }
        else if (c == '\n' && !quoted)
        {
            if (!current.empty() && current.back() == '\r')
            {
                current.pop_back();
            }
            records.push_back({current, fields});
            current.clear();
            fields = 1;
            continue;
        }
        current += c;
    }
    if (!current.empty())
    {
        // A last record without a line ending.
        records.push_back({current, fields});
    }
    return records;
}

} // namespace

bool CsvReporter::upgradeToSidebandColumn_(std::string const& filename)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    std::string contents;
    {
        std::ifstream original(filename, std::ios::binary);
        if (!original)
        {
            return false;
        }
        contents.assign(std::istreambuf_iterator<char>(original), std::istreambuf_iterator<char>());
        if (original.bad())
        {
            return false;
        }
    }

    auto records = splitRecords(contents);
    if (records.empty() || records[0].text != CSV_HEADER_WITHOUT_SIDEBAND)
    {
        return false;
    }

    // The backup: a name not already in use, so as not to replace one kept
    // from before.
    std::string backup = filename + ".bak";
    for (int index = 1; fs::exists(backup, ec) && index < 100; index++)
    {
        backup = filename + ".bak." + std::to_string(index);
    }
    if (fs::exists(backup, ec) || !fs::copy_file(filename, backup, ec))
    {
        log_warn("CsvReporter: could not back up %s to add the sideband column", filename.c_str());
        return false;
    }

    // Written in full alongside, then put in place of the original, so that
    // the log is never left half written.
    std::string temp = filename + ".tmp";
    bool written = false;
    {
        std::ofstream upgraded(temp, std::ios::binary | std::ios::trunc);
        if (upgraded)
        {
            upgraded << CSV_HEADER << "\n";
            for (size_t index = 1; index < records.size(); index++)
            {
                // Records of the old six columns get an empty sideband; any
                // other line (there shouldn't be any) is kept as it was
                // rather than guessed at.
                upgraded << records[index].text << (records[index].fields == 6 ? "," : "") << "\n";
            }
            upgraded.flush();
            written = upgraded.good();
        }
    }

    if (!written || (fs::rename(temp, filename, ec), ec))
    {
        // E.g. the folder can't be written to, or (on Windows) another
        // program has the log open.
        log_warn("CsvReporter: could not add the sideband column to %s: %s", filename.c_str(),
                 written ? ec.message().c_str() : "the new copy couldn't be written");
        fs::remove(temp, ec);
        fs::remove(backup, ec);
        return false;
    }

    log_info("CsvReporter: added the sideband column to %s (the original is kept as %s)",
             filename.c_str(), backup.c_str());
    return true;
}

CsvReporter::CsvReporter(std::string const& filename)
    : hasSidebandColumn_(true)
    , sideband_(Sideband::UNKNOWN)
{
    // Before opening it to add to: a file from before the sideband column
    // was added is given it, if it can be.
    if (readHeader(filename) == CSV_HEADER_WITHOUT_SIDEBAND)
    {
        upgradeToSidebandColumn_(filename);
    }

    file_.open(filename, std::ios::app);
    if (!file_.is_open())
    {
        log_warn("CsvReporter: could not open log file %s", filename.c_str());
        return;
    }

    log_info("CsvReporter: opening log file %s", filename.c_str());

    // Write header only when creating a new (empty) file. Seek to the end
    // first: right after opening in append mode, some C++ libraries (e.g. on
    // Windows) report position 0 until the first write, even for a
    // non-empty file.
    file_.seekp(0, std::ios::end);
    if (file_.tellp() == 0)
    {
        file_ << CSV_HEADER << "\n";
    }
    else
    {
        // A file without the sideband column that couldn't be given it (see
        // above): records go on matching its header.
        hasSidebandColumn_ = (readHeader(filename) == CSV_HEADER);
    }
}

void CsvReporter::sidebandChange(Sideband sideband)
{
    sideband_.store(sideband, std::memory_order_relaxed);
}

void CsvReporter::freqChange(uint64_t) {}
void CsvReporter::transmit(std::string, bool) {}
void CsvReporter::inAnalogMode(bool) {}
void CsvReporter::send() {}

void CsvReporter::addReceiveRecord(std::string callsign, std::string mode, uint64_t frequency, signed char snr)
{
    if (!file_.is_open())
    {
        return;
    }

    int snrInt = static_cast<int>(snr);
    std::time_t now = std::time(nullptr);
    struct tm* curTime = (struct tm*)malloc(sizeof(struct tm));
    assert(curTime != NULL);
#if defined(_WIN32)
    gmtime_s(curTime, &now);
#else
    gmtime_r(&now, curTime);
#endif // defined(_WIN32) 

    char dateBuf[100]; // YYYY-MM-DD\0
    char timeBuf[100];  // HH:MM:SS\0
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", curTime);
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", curTime);

    free(curTime);

    log_info("CsvReporter: adding record for callsign %s, mode %s, frequency %" PRIu64 ", SNR %d",
             callsign.c_str(), mode.c_str(), frequency, snrInt);

    file_ << dateBuf << ","
          << timeBuf << ","
          << csvField(callsign) << ","
          << csvField(mode) << ","
          << frequency << ","
          << snrInt;
    if (hasSidebandColumn_)
    {
        file_ << "," << SidebandName(sideband_.load(std::memory_order_relaxed));
    }
    file_ << "\n";
    file_.flush();
}
