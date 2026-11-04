// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define CATCH_CONFIG_EXTERNAL_INTERFACES
#include "test/CovTestReporter.h"

#ifdef COV_LIBS_ENABLED

#include "crypto/Hex.h"
#include "crypto/SHA.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

extern "C"
{
void __llvm_profile_set_filename(char const*);
void __llvm_profile_reset_counters(void);
int __llvm_profile_write_file(void);
}

namespace
{
std::string
csvQuote(std::string const& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (auto c : s)
    {
        if (c == '"')
        {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string
profileFilenameForTest(std::string const& testName)
{
    auto h = stellar::sha256(stellar::ByteSlice(testName));
    auto hex = stellar::binToHex(stellar::ByteSlice(h));
    return (fs::path("profraw") / ("test-" + hex + ".profraw")).string();
}
}

namespace Catch
{

CovTestReporter::CovTestReporter(ReporterConfig const& _config)
    : ConsoleReporter(_config)
{
}

std::string
CovTestReporter::getDescription()
{
    return "Writes one LLVM .profraw file per test case";
}

ReporterPreferences
CovTestReporter::getPreferences() const
{
    ReporterPreferences prefs;
    // Don't redirect stdout/stderr: test output and logs can still be useful.
    prefs.shouldRedirectStdOut = false;
    return prefs;
}

void
CovTestReporter::testCaseStarting(TestCaseInfo const& testInfo)
{
    ConsoleReporter::testCaseStarting(testInfo);

    mCurrentTestName = testInfo.name;
    mCurrentProfileFilename = profileFilenameForTest(mCurrentTestName);

    stream << "\"" << testInfo.name << "\" " << testInfo.lineInfo << std::endl;

    std::error_code ec;
    fs::create_directories(fs::path(mCurrentProfileFilename).parent_path(), ec);

    __llvm_profile_set_filename(mCurrentProfileFilename.c_str());
    __llvm_profile_reset_counters();

    mTestStart = std::chrono::steady_clock::now();
}

void
CovTestReporter::testCaseEnded(TestCaseStats const& testCaseStats)
{
    auto const end = std::chrono::steady_clock::now();
    auto const durationUs =
        std::chrono::duration_cast<std::chrono::microseconds>(end - mTestStart)
            .count();

    (void)__llvm_profile_write_file();

    std::ofstream csv("profraw.csv", std::ios::app);
    if (csv)
    {
        csv << csvQuote(testCaseStats.testInfo.name) << ','
            << csvQuote(mCurrentProfileFilename) << ',' << durationUs << "\n";
    }

    ConsoleReporter::testCaseEnded(testCaseStats);

    stream << "<done in " << durationUs << " us>" << std::endl;
}

CATCH_REGISTER_REPORTER("covtest", CovTestReporter)

} // namespace Catch

#endif // COV_LIBS_ENABLED
