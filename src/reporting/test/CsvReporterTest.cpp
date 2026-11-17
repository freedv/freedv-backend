#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
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
const std::string TEST_FILE_UPGRADE_PATH = testMakeTempFile("csvup");
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
        seed << CSV_HEADER << "\n";
        seed << "2026-01-01,00:00:00,OLD001,1600X,14000000,1,LSB\n";
    }

    {
        CsvReporter reporter(TEST_FILE_APPEND);
        // Constructor alone must not modify the file (nor back it up: it
        // already has the sideband column).
        auto untouched = readFileLines(TEST_FILE_APPEND);
        result &= (untouched.size() == 2);
        result &= !std::filesystem::exists(TEST_FILE_APPEND_PATH + ".bak");

        reporter.sidebandChange(IReporter::Sideband::USB);
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
        result &= (fields.size() == 7);
        if (fields.size() == 7)
        {
            result &= (fields[0] == todayUtc());
            result &= isValidTimeField(fields[1]);
            result &= (fields[2] == "K6AQ");
            result &= (fields[3] == "8PSK1250");
            result &= (fields[4] == "7100000");
            result &= (fields[5] == "12");
            result &= (fields[6] == "USB");
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

std::string readFile(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void removeWithBackups(std::string const& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".bak", ec);
    std::filesystem::remove(path + ".bak.1", ec);
    std::filesystem::remove(path + ".tmp", ec);
}

// A log from before the sideband column was added: records of six columns,
// one with a quoted comma, one with a quoted line break (over-the-air text
// can have either), and one ending in CRLF.
const std::string OLD_LOG =
    std::string(CSV_HEADER_WITHOUT_SIDEBAND) + "\n"
    "2026-01-01,00:00:00,OLD001,RADEV1,14236000,1\n"
    "2026-01-02,00:00:00,\"K6,AQ\",RADEV1,7177000,-3\n"
    "2026-01-03,00:00:00,\"N1\nDQ\",RADEV1,7177000,4\r\n";

bool testOldFileGetsSidebandColumn()
{
    std::cout << "Test 6 (a file without the sideband column is given it, and backed up): ";

    const std::string path = TEST_FILE_UPGRADE_PATH;
    removeWithBackups(path);
    {
        std::ofstream seed(path, std::ios::binary);
        seed << OLD_LOG;
    }

    {
        CsvReporter reporter(path);
        reporter.sidebandChange(IReporter::Sideband::LSB);
        reporter.addReceiveRecord("VK3TPM", "RADEV1", 7177000, 5);
    }

    std::string expectedStart =
        std::string(CSV_HEADER) + "\n"
        "2026-01-01,00:00:00,OLD001,RADEV1,14236000,1,\n"
        "2026-01-02,00:00:00,\"K6,AQ\",RADEV1,7177000,-3,\n"
        "2026-01-03,00:00:00,\"N1\nDQ\",RADEV1,7177000,4,\n";
    std::string upgraded = readFile(path);
    bool result = upgraded.compare(0, expectedStart.size(), expectedStart) == 0;
    std::string added = upgraded.substr(std::min(expectedStart.size(), upgraded.size()));
    auto fields = splitCsv(added.substr(0, added.find('\n')));
    result &= (fields.size() == 7) && fields[2] == "VK3TPM" && fields[6] == "LSB";

    // The original is kept, as it was.
    result &= (readFile(path + ".bak") == OLD_LOG);
    result &= !std::filesystem::exists(path + ".tmp");

    // Opened again: it has the column now, so nothing more is done to it.
    {
        CsvReporter reporter(path);
        reporter.addReceiveRecord("N1DQ", "RADEV1", 14236000, 2);
    }
    result &= !std::filesystem::exists(path + ".bak.1");
    result &= (readFile(path).compare(0, upgraded.size(), upgraded) == 0);
    result &= (readFile(path + ".bak") == OLD_LOG);

    removeWithBackups(path);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testExistingBackupIsKept()
{
    std::cout << "Test 7 (a backup already there isn't replaced): ";

    const std::string path = TEST_FILE_UPGRADE_PATH;
    removeWithBackups(path);
    {
        std::ofstream seed(path, std::ios::binary);
        seed << OLD_LOG;
        std::ofstream earlier(path + ".bak", std::ios::binary);
        earlier << "someone else's backup\n";
    }

    {
        CsvReporter reporter(path);
    }

    bool result = (readFile(path + ".bak") == "someone else's backup\n");
    result &= (readFile(path + ".bak.1") == OLD_LOG);
    result &= (readFile(path).rfind(CSV_HEADER, 0) == 0);

    removeWithBackups(path);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testUpgradeThatCantBeDoneLeavesFileAsItWas()
{
    std::cout << "Test 8 (if the column can't be added, the file's kept as it was): ";

#if defined(_WIN32)
    // (A folder can't be made read-only this way on Windows.)
    std::cout << "SKIPPED (Windows)\n";
    return true;
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path folder = fs::path(testMakeTempFile("csvro"));
    fs::remove(folder, ec);
    fs::create_directory(folder, ec);
    std::string path = (folder / "heard.csv").string();
    {
        std::ofstream seed(path, std::ios::binary);
        seed << OLD_LOG;
    }

    // A folder nothing new can be put in: no backup, and no new copy.
    fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec, ec);
    bool canStillWrite = std::ofstream((folder / "probe").string()).is_open();
    bool result = true;
    if (canStillWrite)
    {
        // (E.g. running as root, which isn't stopped by the permissions.)
        std::cout << "SKIPPED (folder permissions not enforced) ";
    }
    else
    {
        {
            CsvReporter reporter(path);
            reporter.sidebandChange(IReporter::Sideband::USB);
            reporter.addReceiveRecord("W1AW", "RADEV1", 3625000, 7);
        }

        // Still the six columns its header names, with the record added.
        std::string contents = readFile(path);
        result &= (contents.compare(0, OLD_LOG.size(), OLD_LOG) == 0);
        std::string added = contents.substr(std::min(OLD_LOG.size(), contents.size()));
        auto fields = splitCsv(added.substr(0, added.find('\n')));
        result &= (fields.size() == 6) && fields[2] == "W1AW";
        result &= !fs::exists(path + ".bak") && !fs::exists(path + ".tmp");
    }

    fs::permissions(folder, fs::perms::owner_all, ec);
    fs::remove_all(folder, ec);
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
#endif // defined(_WIN32)
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
    result &= testOldFileGetsSidebandColumn();
    result &= testExistingBackupIsKept();
    result &= testUpgradeThatCantBeDoneLeavesFileAsItWas();

    return result ? 0 : -1;
}
