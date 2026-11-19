// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "test/Catch2.h"

#ifdef COV_LIBS_ENABLED

#include <chrono>
#include <string>

namespace Catch
{

struct CovTestReporter : public ConsoleReporter
{
    CovTestReporter(ReporterConfig const& _config);

    static std::string getDescription();

    ReporterPreferences getPreferences() const override;

    void testCaseStarting(TestCaseInfo const& testInfo) override;
    void testCaseEnded(TestCaseStats const& testCaseStats) override;

  private:
    std::string mCurrentTestName;
    std::string mCurrentProfileFilename;
    std::chrono::steady_clock::time_point mTestStart;
};

} // namespace Catch

#endif // COV_LIBS_ENABLED
