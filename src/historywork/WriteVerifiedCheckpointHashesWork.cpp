// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/WriteVerifiedCheckpointHashesWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/HistoryManager.h"
#include "historywork/BatchDownloadWork.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerRange.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "work/ConditionalWork.h"
#include <Tracy.hpp>
#include <algorithm>
#include <fmt/format.h>

namespace stellar
{

Hash
WriteVerifiedCheckpointHashesWork::loadHashFromJsonOutput(
    uint32_t seq, std::string const& filename)
{
    std::ifstream in(filename);
    if (!in)
    {
        throw std::runtime_error("error opening " + filename);
    }
    Json::Value root;
    Json::Reader rdr;
    if (!rdr.parse(in, root))
    {
        throw std::runtime_error("failed to parse JSON input " + filename);
    }
    if (!root.isArray())
    {
        throw std::runtime_error("expected top-level array in " + filename);
    }
    for (auto const& jpair : root)
    {
        if (!jpair.isArray() || (jpair.size() != 2))
        {
            throw std::runtime_error("expecting 2-element sub-array in " +
                                     filename);
        }
        if (jpair[0].asUInt() == seq)
        {
            return hexToBin256(jpair[1].asString());
        }
    }
    return Hash{};
}

WriteVerifiedCheckpointHashesWork::WriteVerifiedCheckpointHashesWork(
    Application& app, LedgerNumHashPair rangeEnd, std::string const& outputFile,
    uint32_t nestedBatchSize, std::shared_ptr<HistoryArchive> archive)
    : BatchWork(app, "write-verified-checkpoint-hashes")
    , mNestedBatchSize(nestedBatchSize)
    , mRangeEnd(rangeEnd)
    , mRangeEndPromise()
    , mRangeEndFuture(mRangeEndPromise.get_future().share())
    , mCurrCheckpoint(rangeEnd.first)
    , mArchive(archive)
    , mOutputFileName(outputFile)
{
    mRangeEndPromise.set_value(mRangeEnd);
    if (mArchive)
    {
        CLOG_INFO(History, "selected archive {}", mArchive->getName());
    }
    startOutputFile();
}

WriteVerifiedCheckpointHashesWork::~WriteVerifiedCheckpointHashesWork()
{
    endOutputFile();
}

bool
WriteVerifiedCheckpointHashesWork::hasNext() const
{
    return mCurrCheckpoint != LedgerManager::GENESIS_LEDGER_SEQ;
}

std::shared_ptr<BasicWork>
WriteVerifiedCheckpointHashesWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        throw std::runtime_error("nothing to iterate over");
    }

    auto const& hm = mApp.getHistoryManager();
    uint32_t const freq = hm.getCheckpointFrequency();

    auto const lclHe = mApp.getLedgerManager().getLastClosedLedgerHeader();
    LedgerNumHashPair const lcl(lclHe.header.ledgerSeq,
                                make_optional<Hash>(lclHe.hash));
    uint32_t const span = mNestedBatchSize * freq;
    uint32_t const last = mCurrCheckpoint;
    uint32_t const first =
        last <= span ? LedgerManager::GENESIS_LEDGER_SEQ
                     : hm.firstLedgerInCheckpointContaining(last - span);

    LedgerRange const ledgerRange = LedgerRange::inclusive(first, last);
    CheckpointRange const checkpointRange(ledgerRange, hm);

    std::string const checkpointStr = std::to_string(mCurrCheckpoint);

    // Clear out TmpDirs of any previous WorkSequences that are now done.
    {
        auto i = std::remove_if(
            mTmpDirs.begin(), mTmpDirs.end(),
            [](std::pair<std::shared_ptr<WorkSequence>, std::shared_ptr<TmpDir>>
                   pair) -> bool { return pair.first->isDone(); });
        mTmpDirs.erase(i, mTmpDirs.end());
    }

    auto tmpDir = std::make_shared<TmpDir>(
        mApp.getTmpDirManager().tmpDir("verify-" + checkpointStr));
    auto getWork = std::make_shared<BatchDownloadWork>(
        mApp, checkpointRange, HISTORY_FILE_TYPE_LEDGER, *tmpDir, mArchive);

    // When we have a previous-work, we grab a future attached to the promise it
    // will fulfill when it runs. This promise might not have a value _yet_ but
    // a shared reference to it will allow the currWork we're building to read
    // the value when it is filled in (which happens before the currWork is
    // allowed to run).
    //
    // When we don't have a previous-work we're at the start of the chain, where
    // we use the local promise (and accompanying shared_future) we built from
    // the trusted mRangeEnd value we were constructed with).
    std::shared_future<LedgerNumHashPair> prevTrusted =
        (mPrevVerifyWork ? mPrevVerifyWork->getVerifiedMinLedgerPrev()
                         : mRangeEndFuture);

    auto currWork = std::make_shared<VerifyLedgerChainWork>(
        mApp, *tmpDir, ledgerRange, lcl, prevTrusted, mOutputFile);
    auto prevWork = mPrevVerifyWork;
    auto predicate = [prevWork]() {
        if (!prevWork)
        {
            return true;
        }
        return (prevWork->getState() == State::WORK_SUCCESS);
    };

    auto condWork = std::make_shared<ConditionalWork>(
        mApp, "await-input-to-verify-" + checkpointStr, predicate, currWork);

    std::vector<std::shared_ptr<BasicWork>> seq{getWork, condWork};
    auto workSeq = std::make_shared<WorkSequence>(
        mApp, "download-verify-ledger-" + checkpointStr, seq);

    mTmpDirs.emplace_back(workSeq, tmpDir);
    releaseAssert(first >= 1);
    mCurrCheckpoint = std::max(LedgerManager::GENESIS_LEDGER_SEQ, first - 1);
    mPrevVerifyWork = currWork;
    return workSeq;
}

void
WriteVerifiedCheckpointHashesWork::startOutputFile()
{
    releaseAssert(!mOutputFile);
    auto mode = std::ios::out | std::ios::trunc;
    mOutputFile = std::make_shared<std::ofstream>(mOutputFileName, mode);
    if (!*mOutputFile)
    {
        throw std::runtime_error("error opening output file " +
                                 mOutputFileName);
    }
    (*mOutputFile) << "[";
}

void
WriteVerifiedCheckpointHashesWork::endOutputFile()
{
    if (mOutputFile && mOutputFile->is_open())
    {
        // Each line of output made by a VerifyLedgerChainWork has a trailing
        // comma, and trailing commas are not a valid end of a JSON array; so we
        // terminate the array here with an entry that does _not_ have a
        // trailing comma (and identifies an invalid ledger number anyways).
        (*mOutputFile) << "\n[0, \"\"]\n]\n";
        mOutputFile->close();
        mOutputFile.reset();
    }
}

void
WriteVerifiedCheckpointHashesWork::resetIter()
{
    mCurrCheckpoint = mRangeEnd.first;
    mTmpDirs.clear();
    endOutputFile();
    startOutputFile();
}
}
