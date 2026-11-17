#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../CsvReporter.h"
#include "../../util/test/TestSocketCompat.h"

namespace {

// In the temporary directory, so parallel or interrupted runs don't collide
// in (or litter) the working directory.
const std::string TEST_FILE_NEW_PATH = testMakeTempFile("csvnew");
const std::string TEST_FILE_APPEND_PATH = testMakeTempFile("csvapp");
const std::string TEST_FILE_QUOTED_PATH = testMakeTempFile("csvquo");
const std::string TEST_FILE_SIDEBAND_PATH = testMakeTempFile("csvsb");
const char* TEST_FILE_NEW = TEST_FILE_NEW_PATH.c_str();
const char* TEST_FILE_APPEND = TEST_FILE_APPEND_PATH.c_str();
const char* TEST_FILE_QUOTED = TEST_FILE_QUOTED_PATH.c_str();
const char* TEST_FILE_BAD = "/definitely/not/a/valid/dir/csv_reporter_test_bad.csv";

const char* CSV_HEADER = "date,time,callsign,mode,frequency_hz,snr_db,sideband";
// As files were started before the sideband column was added.
const char* CSV_HEADER_WITHOUT_SIDEBAND = "date,time,callsign,mode,frequency_hz,snr_db";

std::vector<std::string> readFileLines(const char* path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line))
    {
        lines.push_back(line);
    }
    return lines;
}

// Splits one CSV record (RFC 4180: fields may be quoted, "" is a quote).
std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        char c = line[i];
        if (quoted)
        {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"')
            {
                current += '"';
                i++;
            }
            else if (c == '"')
            {
                quoted = false;
            }
            else
            {
                current += c;
            }
        }
        else if (c == '"')
        {
            quoted = true;
        }
        else if (c == ',')
        {
            fields.push_back(current);
            current.clear();
        }
        else
        {
            current += c;
        }
    }
    fields.push_back(current);
    return fields;
}

std::string todayUtc()
{
    std::time_t now = std::time(nullptr);
    struct tm curTime;
#if defined(_WIN32)
    gmtime_s(&curTime, &now);
#else
    gmtime_r(&now, &curTime);
#endif // defined(_WIN32)
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &curTime);
    return std::string(buf);
}

bool isValidTimeField(const std::string& field)
{
    if (field.size() != 8)
    {
        return false;
    }
    int hours = -1, minutes = -1, seconds = -1;
    if (std::sscanf(field.c_str(), "%d:%d:%d", &hours, &minutes, &seconds) != 3)
    {
        return false;
    }
    return hours >= 0 && hours < 24 && minutes >= 0 && minutes < 60 && seconds >= 0 && seconds < 60;
}

bool testNewFileWritesHeaderOnce()
{
    std::cout << "Test 1 (new file gets header once, records append): ";

    std::remove(TEST_FILE_NEW);
    bool result = true;

    // Constructing on a fresh file must write the header even with no records.
    {
        CsvReporter reporter(TEST_FILE_NEW);
        reporter.freqChange(14236000);
        reporter.transmit("1600X", true);
        reporter.inAnalogMode(false);
        reporter.send();
    }

    auto lines = readFileLines(TEST_FILE_NEW);
    result &= (lines.size() == 1);
    result &= (lines.empty() ? false : lines[0] == CSV_HEADER);

    // Reopening the same (non-empty) file must not write a second header.
    {
        CsvReporter reporter(TEST_FILE_NEW);
        reporter.addReceiveRecord("N1DQ", "1600X", 14236000, -5);
    }

    lines = readFileLines(TEST_FILE_NEW);
    result &= (lines.size() == 2);
    if (lines.size() == 2)
    {
        auto fields = splitCsv(lines[1]);
        result &= (fields.size() == 7);
        if (fields.size() == 7)
        {
            result &= (fields[0] == todayUtc());
            result &= isValidTimeField(fields[1]);
            result &= (fields[2] == "N1DQ");
            result &= (fields[3] == "1600X");
            result &= (fields[4] == "14236000");
            result &= (fields[5] == "-5");
            result &= (fields[6] == ""); // sideband not known
        }
    }

    std::remove(TEST_FILE_NEW);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testAppendToExistingFileNoDuplicateHeader()
{
    std::cout << "Test 2 (append to existing file adds no second header): ";

    std::remove(TEST_FILE_APPEND);
    bool result = true;

    {
        std::ofstream seed(TEST_FILE_APPEND);
        seed << CSV_HEADER_WITHOUT_SIDEBAND << "\n";
        seed << "2026-01-01,00:00:00,OLD001,1600X,14000000,1\n";
    }

    {
        CsvReporter reporter(TEST_FILE_APPEND);
        // (A file without the sideband column keeps to its columns, even
        // when the sideband's known.)
        reporter.sidebandChange(IReporter::Sideband::USB);
        // Constructor alone must not modify the file.
        auto untouched = readFileLines(TEST_FILE_APPEND);
        result &= (untouched.size() == 2);

        reporter.addReceiveRecord("K6AQ", "8PSK1250", 7100000, 12);
    }

    auto lines = readFileLines(TEST_FILE_APPEND);
    result &= (lines.size() == 3);

    int headerCount = 0;
    for (const auto& line : lines)
    {
        if (line == CSV_HEADER || line == CSV_HEADER_WITHOUT_SIDEBAND) headerCount++;
    }
    result &= (headerCount == 1);

    if (lines.size() == 3)
    {
        auto fields = splitCsv(lines[2]);
        result &= (fields.size() == 6);
        if (fields.size() == 6)
        {
            result &= (fields[0] == todayUtc());
            result &= isValidTimeField(fields[1]);
            result &= (fields[2] == "K6AQ");
            result &= (fields[3] == "8PSK1250");
            result &= (fields[4] == "7100000");
            result &= (fields[5] == "12");
        }
    }

    std::remove(TEST_FILE_APPEND);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testUnopenableFileIsNoOp()
{
    std::cout << "Test 3 (unopenable path: no crash, no writes): ";

    bool result = true;
    {
        CsvReporter reporter(TEST_FILE_BAD);
        reporter.addReceiveRecord("N1DQ", "1600X", 14236000, -5);
    }

    std::ifstream probe(TEST_FILE_BAD);
    result &= !probe.good();

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testFieldsWithSpecialCharactersAreQuoted()
{
    std::cout << "Test 4 (a callsign with a comma or quote can't break the columns): ";

    // RADE text's character set includes ',', so a callsign received over
    // the air can contain one. It used to be written as-is, adding a column.
    std::remove(TEST_FILE_QUOTED);
    {
        CsvReporter reporter(TEST_FILE_QUOTED);
        reporter.addReceiveRecord("K6,AQ", "1600X", 14236000, 3);
        reporter.addReceiveRecord("N1\"DQ", "RADE,V1", 7100000, -2);
    }

    auto lines = readFileLines(TEST_FILE_QUOTED);
    bool result = (lines.size() == 3);
    if (lines.size() == 3)
    {
        auto first = splitCsv(lines[1]);
        result &= (first.size() == 7) && first[2] == "K6,AQ" && first[3] == "1600X" && first[4] == "14236000";
        auto second = splitCsv(lines[2]);
        result &= (second.size() == 7) && second[2] == "N1\"DQ" && second[3] == "RADE,V1" && second[5] == "-2";
    }

    std::remove(TEST_FILE_QUOTED);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testSidebandColumn()
{
    std::cout << "Test 5 (the radio's sideband in each record, in a file with the column): ";

    std::remove(TEST_FILE_SIDEBAND_PATH.c_str());
    {
        CsvReporter reporter(TEST_FILE_SIDEBAND_PATH);
        reporter.sidebandChange(IReporter::Sideband::LSB);
        reporter.addReceiveRecord("VK3TPM", "RADEV1", 7177000, 4);
        reporter.sidebandChange(IReporter::Sideband::USB);
        reporter.addReceiveRecord("K6AQ", "RADEV1", 14236000, 9);
    }
    {
        // Reopened: still the file with the column.
        CsvReporter reporter(TEST_FILE_SIDEBAND_PATH);
        reporter.sidebandChange(IReporter::Sideband::UNKNOWN);
        reporter.addReceiveRecord("N1DQ", "RADEV1", 14236000, 2);
    }

    auto lines = readFileLines(TEST_FILE_SIDEBAND_PATH.c_str());
    bool result = (lines.size() == 4);
    if (lines.size() == 4)
    {
        result &= (lines[0] == CSV_HEADER);
        auto first = splitCsv(lines[1]);
        auto second = splitCsv(lines[2]);
        auto third = splitCsv(lines[3]);
        result &= (first.size() == 7) && first[2] == "VK3TPM" && first[6] == "LSB";
        result &= (second.size() == 7) && second[2] == "K6AQ" && second[6] == "USB";
        result &= (third.size() == 7) && third[2] == "N1DQ" && third[6] == "";
    }

    std::remove(TEST_FILE_SIDEBAND_PATH.c_str());
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    bool result = true;

    result &= testNewFileWritesHeaderOnce();
    result &= testAppendToExistingFileNoDuplicateHeader();
    result &= testUnopenableFileIsNoOp();
    result &= testFieldsWithSpecialCharactersAreQuoted();
    result &= testSidebandColumn();

    return result ? 0 : -1;
}
