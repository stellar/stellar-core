// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerStateSnapshot.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

LedgerEntryWrapper::LedgerEntryWrapper(ConstLedgerTxnEntry&& entry)
    : mEntry(std::move(entry))
{
}

LedgerEntryWrapper::LedgerEntryWrapper(LedgerTxnEntry&& entry)
    : mEntry(std::move(entry))
{
}

LedgerEntryWrapper::LedgerEntryWrapper(std::shared_ptr<LedgerEntry const> entry)
    : mEntry(entry)
{
}

LedgerEntry const&
LedgerEntryWrapper::current() const
{
    switch (mEntry.index())
    {
    case 0:
        return std::get<0>(mEntry).current();
    case 1:
        return std::get<1>(mEntry).current();
    case 2:
        return *std::get<2>(mEntry);
    default:
        throw std::runtime_error("Invalid LedgerEntryWrapper index");
    }
}

LedgerEntryWrapper::operator bool() const
{
    switch (mEntry.index())
    {
    case 0:
        return static_cast<bool>(std::get<0>(mEntry));
    case 1:
        return static_cast<bool>(std::get<1>(mEntry));
    case 2:
        return static_cast<bool>(std::get<2>(mEntry));
    default:
        throw std::runtime_error("Invalid LedgerEntryWrapper index");
    }
}

LedgerHeaderWrapper::LedgerHeaderWrapper(LedgerTxnHeader&& header)
    : mHeader(std::move(header))
{
}

LedgerHeaderWrapper::LedgerHeaderWrapper(std::shared_ptr<LedgerHeader> header)
    : mHeader(header)
{
}

LedgerHeader&
LedgerHeaderWrapper::currentToModify()
{
    switch (mHeader.index())
    {
    case 0:
        return std::get<0>(mHeader).current();
    case 1:
        return *std::get<1>(mHeader);
    default:
        throw std::runtime_error("Invalid LedgerHeaderWrapper index");
    }
}

LedgerHeader const&
LedgerHeaderWrapper::current() const
{
    switch (mHeader.index())
    {
    case 0:
        return std::get<0>(mHeader).current();
    case 1:
        return *std::get<1>(mHeader);
    default:
        throw std::runtime_error("Invalid LedgerHeaderWrapper index");
    }
}

LedgerTxnReadOnly::LedgerTxnReadOnly(AbstractLedgerTxn& ltx) : mLedgerTxn(ltx)
{
}

LedgerTxnReadOnly::~LedgerTxnReadOnly()
{
}

LedgerHeaderWrapper
LedgerTxnReadOnly::getLedgerHeader() const
{
    return LedgerHeaderWrapper(mLedgerTxn.loadHeader());
}

LedgerEntryWrapper
LedgerTxnReadOnly::getAccount(AccountID const& account) const
{
    return LedgerEntryWrapper(loadAccountWithoutRecord(mLedgerTxn, account));
}

LedgerEntryWrapper
LedgerTxnReadOnly::getAccount(LedgerHeaderWrapper const& header,
                              TransactionFrame const& tx) const
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        return LedgerEntryWrapper(
            tx.loadSourceAccount(mLedgerTxn, header.getLedgerTxnHeader()));
    }

    return getAccount(tx.getSourceID());
}

LedgerEntryWrapper
LedgerTxnReadOnly::getAccount(LedgerHeaderWrapper const& header,
                              TransactionFrame const& tx,
                              AccountID const& account) const
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        return LedgerEntryWrapper(
            tx.loadAccount(mLedgerTxn, header.getLedgerTxnHeader(), account));
    }

    return getAccount(account);
}

LedgerEntryWrapper
LedgerTxnReadOnly::load(LedgerKey const& key) const
{
    return LedgerEntryWrapper(mLedgerTxn.loadWithoutRecord(key));
}

void
LedgerTxnReadOnly::executeWithMaybeInnerSnapshot(
    std::function<void(LedgerSnapshot const& ls)> f) const
{
    LedgerTxn inner(mLedgerTxn);
    LedgerSnapshot lsg(inner);
    return f(lsg);
}

BucketSnapshotState::BucketSnapshotState(SearchableSnapshotConstPtr snapshot)
    : mSnapshot(snapshot)
    , mLedgerHeader(LedgerHeaderWrapper(
          std::make_shared<LedgerHeader>(mSnapshot->getLedgerHeader())))
{
}

BucketSnapshotState::~BucketSnapshotState()
{
}

LedgerHeaderWrapper
BucketSnapshotState::getLedgerHeader() const
{
    return LedgerHeaderWrapper(std::get<1>(mLedgerHeader.mHeader));
}

LedgerEntryWrapper
BucketSnapshotState::getAccount(AccountID const& account) const
{
    return LedgerEntryWrapper(mSnapshot->load(accountKey(account)));
}

LedgerEntryWrapper
BucketSnapshotState::getAccount(LedgerHeaderWrapper const& header,
                                TransactionFrame const& tx) const
{
    return getAccount(tx.getSourceID());
}

LedgerEntryWrapper
BucketSnapshotState::getAccount(LedgerHeaderWrapper const& header,
                                TransactionFrame const& tx,
                                AccountID const& AccountID) const
{
    return getAccount(AccountID);
}

LedgerEntryWrapper
BucketSnapshotState::load(LedgerKey const& key) const
{
    return LedgerEntryWrapper(mSnapshot->load(key));
}

void
BucketSnapshotState::executeWithMaybeInnerSnapshot(
    std::function<void(LedgerSnapshot const& ls)> f) const
{
    throw std::runtime_error(
        "BucketSnapshotState::executeWithMaybeInnerSnapshot is illegal: "
        "BucketSnapshotState has no nested snapshots");
}

LedgerSnapshot::LedgerSnapshot(AbstractLedgerTxn& ltx)
    : mGetter(std::make_unique<LedgerTxnReadOnly>(ltx))
{
}

LedgerSnapshot::LedgerSnapshot(Application& app)
{
    releaseAssert(threadIsMain());
#ifdef BUILD_TESTS
    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        // Legacy read-only SQL transaction
        mLegacyLedgerTxn = std::make_unique<LedgerTxn>(
            app.getLedgerTxnRoot(), /* shouldUpdateLastModified*/ false,
            TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
        mGetter = std::make_unique<LedgerTxnReadOnly>(*mLegacyLedgerTxn);
    }
    else
#endif
        mGetter = std::make_unique<BucketSnapshotState>(
            app.getLedgerManager().getCurrentLedgerStateSnaphot());
}

LedgerHeaderWrapper
LedgerSnapshot::getLedgerHeader() const
{
    return mGetter->getLedgerHeader();
}

LedgerEntryWrapper
LedgerSnapshot::getAccount(AccountID const& account) const
{
    return mGetter->getAccount(account);
}

LedgerEntryWrapper
LedgerSnapshot::load(LedgerKey const& key) const
{
    return mGetter->load(key);
}

void
LedgerSnapshot::executeWithMaybeInnerSnapshot(
    std::function<void(LedgerSnapshot const& ls)> f) const
{
    return mGetter->executeWithMaybeInnerSnapshot(f);
}
}
