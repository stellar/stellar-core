// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "test/Catch2.h"

#include <stdexcept>
#include <string>

namespace stellar
{

// Standalone fuzz entry points do not run inside Catch's test runner, but many
// test helpers used by fuzz targets contain REQUIRE/CHECK assertions. Without a
// result capture installed, those assertions fail with Catch's internal "No
// result capture instance" error instead of reporting the real assertion.
//
// Install this at non-Catch fuzz entry points, such as FuzzMain or fuzz-one,
// around target->run(). It is intentionally a no-op when a real Catch capture is
// already present, so smoke/corpus unit tests keep their normal reporting.
class ScopedCatchResultCapture : public Catch::IResultCapture
{
  public:
    ScopedCatchResultCapture()
        : mPreviousCapture(Catch::getCurrentContext().getResultCapture())
    {
        if (mPreviousCapture == nullptr)
        {
            Catch::getCurrentMutableContext().setResultCapture(this);
            mInstalled = true;
        }
    }

    ~ScopedCatchResultCapture() override
    {
        if (mInstalled)
        {
            Catch::getCurrentMutableContext().setResultCapture(mPreviousCapture);
        }
    }

    bool
    sectionStarted(Catch::SectionInfo const&, Catch::Counts&) override
    {
        return true;
    }

    void
    sectionEnded(Catch::SectionEndInfo const&) override
    {
    }

    void
    sectionEndedEarly(Catch::SectionEndInfo const&) override
    {
    }

    Catch::IGeneratorTracker&
    acquireGeneratorTracker(Catch::StringRef,
                            Catch::SourceLineInfo const&) override
    {
        throw std::runtime_error("generators are unsupported in fuzz target");
    }

#if defined(CATCH_CONFIG_ENABLE_BENCHMARKING)
    void
    benchmarkPreparing(std::string const&) override
    {
    }

    void
    benchmarkStarting(Catch::BenchmarkInfo const&) override
    {
    }

    void
    benchmarkEnded(Catch::BenchmarkStats<> const&) override
    {
    }

    void
    benchmarkFailed(std::string const&) override
    {
        throw std::runtime_error("benchmark failed in fuzz target");
    }
#endif

    void
    pushScopedMessage(Catch::MessageInfo const&) override
    {
    }

    void
    popScopedMessage(Catch::MessageInfo const&) override
    {
    }

    void
    emplaceUnscopedMessage(Catch::MessageBuilder const&) override
    {
    }

    void
    handleFatalErrorCondition(Catch::StringRef message) override
    {
        throw std::runtime_error(static_cast<std::string>(message));
    }

    void
    handleExpr(Catch::AssertionInfo const& info,
               Catch::ITransientExpression const& expr,
               Catch::AssertionReaction& reaction) override
    {
        mLastAssertionPassed =
            expr.getResult() != Catch::isFalseTest(info.resultDisposition);
        if (!mLastAssertionPassed)
        {
            reaction.shouldThrow = true;
        }
    }

    void
    handleMessage(Catch::AssertionInfo const&, Catch::ResultWas::OfType result,
                  Catch::StringRef const& message,
                  Catch::AssertionReaction& reaction) override
    {
        mLastAssertionPassed = Catch::isOk(result);
        if (!mLastAssertionPassed)
        {
            reaction.shouldThrow = true;
            mLastMessage = static_cast<std::string>(message);
        }
    }

    void
    handleUnexpectedExceptionNotThrown(Catch::AssertionInfo const&,
                                       Catch::AssertionReaction& reaction) override
    {
        mLastAssertionPassed = false;
        reaction.shouldThrow = true;
    }

    void
    handleUnexpectedInflightException(Catch::AssertionInfo const&,
                                      std::string const& message,
                                      Catch::AssertionReaction& reaction) override
    {
        mLastAssertionPassed = false;
        reaction.shouldThrow = true;
        mLastMessage = message;
    }

    void
    handleIncomplete(Catch::AssertionInfo const&) override
    {
        mLastAssertionPassed = false;
    }

    void
    handleNonExpr(Catch::AssertionInfo const&, Catch::ResultWas::OfType result,
                  Catch::AssertionReaction& reaction) override
    {
        mLastAssertionPassed = Catch::isOk(result);
        if (!mLastAssertionPassed)
        {
            reaction.shouldThrow = true;
        }
    }

    bool
    lastAssertionPassed() override
    {
        return mLastAssertionPassed;
    }

    void
    assertionPassed() override
    {
        mLastAssertionPassed = true;
    }

    std::string
    getCurrentTestName() const override
    {
        return "fuzz target";
    }

    Catch::AssertionResult const*
    getLastResult() const override
    {
        return nullptr;
    }

    void
    exceptionEarlyReported() override
    {
    }

  private:
    Catch::IResultCapture* mPreviousCapture;
    bool mInstalled{false};
    bool mLastAssertionPassed{true};
    std::string mLastMessage;
};

} // namespace stellar
