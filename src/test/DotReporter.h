#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace Catch
{

struct DotReporter : public ConsoleReporter
{
    DotReporter(ReporterConfig const& _config) : ConsoleReporter(_config)
    {
    }

    ~DotReporter();

    static std::string
    getDescription()
    {
        return "Reports each assertion as a dot";
    }

    ReporterPreferences
    getPreferences() const override
    {
        ReporterPreferences prefs;
        prefs.shouldRedirectStdOut = false;
        return prefs;
    }

    void
    noMatchingTestCases(std::string const& spec) override
    {
        stream << "No test cases matched '" << spec << "'" << std::endl;
    }

    void
    assertionStarting(AssertionInfo const& ai) override
    {
        printDot();
        mLastAssertInfo = ai;
    }

    bool
    assertionEnded(AssertionStats const& _assertionStats) override
    {
        AssertionResult const& result = _assertionStats.assertionResult;

        if (result.isOk())
            return true;
        assertionStarting(mLastAssertInfo);
        return ConsoleReporter::assertionEnded(_assertionStats);
    }

    void
    testCaseEnded(TestCaseStats const&) override
    {
        printNewLine();
    }

  private:
    int mDots{0};

    AssertionInfo mLastAssertInfo;

    void
    printDot()
    {
        stream << '.';
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

INTERNAL_CATCH_REGISTER_REPORTER("dot", DotReporter)
}
