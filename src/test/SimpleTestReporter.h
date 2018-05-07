#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"

namespace Catch
{

struct SimpleTestReporter : public ConsoleReporter
{
    SimpleTestReporter(ReporterConfig const& _config) : ConsoleReporter(_config)
    {
    }

    ~SimpleTestReporter();

    static std::string
    getDescription()
    {
        return "Reports minimal information on tests";
    }

    ReporterPreferences
    getPreferences() const override
    {
        ReporterPreferences prefs;
        prefs.shouldRedirectStdOut = false;
        return prefs;
    }

    void
    testCaseStarting(TestCaseInfo const& ti) override
    {
        ConsoleReporter::testCaseStarting(ti);
        stream << "\"" << ti.name << "\" " << ti.lineInfo << std::endl;
    }

    void
    sectionStarting(SectionInfo const& _sectionInfo) override
    {
        printDot();
        ConsoleReporter::sectionStarting(_sectionInfo);
    }

    void
    assertionStarting(AssertionInfo const& ai) override
    {
        mLastAssertInfo = std::make_unique<AssertionInfo>(ai);
    }

    bool
    assertionEnded(AssertionStats const& _assertionStats) override
    {
        bool res = _assertionStats.assertionResult.isOk();
        if (!res)
        {
            ConsoleReporter::assertionStarting(*mLastAssertInfo);
            res = ConsoleReporter::assertionEnded(_assertionStats);
        }
        mLastAssertInfo.reset();
        return res;
    }

    void
    testCaseEnded(TestCaseStats const&) override
    {
        stream << "<done>";
        printNewLine();
    }

  private:
    int mDots{0};

    std::unique_ptr<AssertionInfo> mLastAssertInfo;

    void
    printDot()
    {
        stream << '.' << std::flush;
        mDots++;
        if (mDots == 40)
        {
            printNewLine();
        }
    }

    void
    printNewLine()
    {
        stream << '\n';
        mDots = 0;
    }
};

CATCH_REGISTER_REPORTER("simple", SimpleTestReporter)
}
