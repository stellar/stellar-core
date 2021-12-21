// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InMemoryLedgerTxn.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerTxnImpl.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/XDROperators.h"

namespace stellar
{

InMemoryLedgerTxn::InMemoryLedgerTxn(InMemoryLedgerTxnRoot& parent,
                                     Database& db)
    : LedgerTxn(parent), mDb(db)
{
}

InMemoryLedgerTxn::~InMemoryLedgerTxn()
{
}

void
InMemoryLedgerTxn::addChild(AbstractLedgerTxn& child, TransactionMode mode)
{
    if (mTransaction)
    {
        throw std::runtime_error(
            "Adding child to already-open InMemoryLedgerTxn");
    }
    LedgerTxn::addChild(child, mode);
    if (mode == TransactionMode::READ_WRITE_WITH_SQL_TXN)
    {
        mTransaction = std::make_unique<soci::transaction>(mDb.getSession());
    }
}

void
InMemoryLedgerTxn::updateLedgerKeyMap(InternalLedgerKey const& genKey,
                                      bool add) noexcept
{
    if (genKey.type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const& ledgerKey = genKey.ledgerKey();

        if ((ledgerKey.type() == TRUSTLINE &&
             ledgerKey.trustLine().asset.type() == ASSET_TYPE_POOL_SHARE) ||
            ledgerKey.type() == OFFER)
        {
            auto const& accountID = ledgerKey.type() == OFFER
                                        ? ledgerKey.offer().sellerID
                                        : ledgerKey.trustLine().accountID;

            if (add)
            {
                mOffersAndPoolShareTrustlineKeys[accountID].emplace(ledgerKey);
            }
            else
            {
                auto it = mOffersAndPoolShareTrustlineKeys.find(accountID);
                if (it != mOffersAndPoolShareTrustlineKeys.end())
                {
                    auto& keySet = it->second;
                    keySet.erase(ledgerKey);
                    if (keySet.empty())
                    {
                        mOffersAndPoolShareTrustlineKeys.erase(it);
                    }
                }
            }
        }
    }
}

void
InMemoryLedgerTxn::updateLedgerKeyMap(EntryIterator iter)
{
    for (; (bool)iter; ++iter)
    {
        auto const& genKey = iter.key();
        updateLedgerKeyMap(genKey, iter.entryExists());
    }
}

void
InMemoryLedgerTxn::commitChild(EntryIterator iter,
                               LedgerTxnConsistency cons) noexcept
{
    if (!mTransaction)
    {
        printErrorAndAbort("Committing child to non-open InMemoryLedgerTxn");
    }
    try
    {
        updateLedgerKeyMap(iter);

        LedgerTxn::commitChild(iter, cons);
        mTransaction->commit();
        mTransaction.reset();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("fatal error during commit to InMemoryLedgerTxn: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error during commit to InMemoryLedgerTxn");
    }
}

void
InMemoryLedgerTxn::rollbackChild() noexcept
{
    try
    {
        LedgerTxn::rollbackChild();
        if (mTransaction)
        {
            mTransaction->rollback();
            mTransaction.reset();
        }
    }
    catch (std::exception& e)
    {
        printErrorAndAbort(
            "fatal error when rolling back child of InMemoryLedgerTxn: ",
            e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "unknown fatal error when rolling back child of InMemoryLedgerTxn");
    }
}

void
InMemoryLedgerTxn::createWithoutLoading(InternalLedgerEntry const& entry)
{
    LedgerTxn::createWithoutLoading(entry);
    updateLedgerKeyMap(entry.toKey(), true);
}

void
InMemoryLedgerTxn::updateWithoutLoading(InternalLedgerEntry const& entry)
{
    LedgerTxn::updateWithoutLoading(entry);
    updateLedgerKeyMap(entry.toKey(), true);
}

void
InMemoryLedgerTxn::eraseWithoutLoading(InternalLedgerKey const& key)
{
    LedgerTxn::eraseWithoutLoading(key);
    updateLedgerKeyMap(key, false);
}

LedgerTxnEntry
InMemoryLedgerTxn::create(InternalLedgerEntry const& entry)
{
    throw std::runtime_error("called create on InMemoryLedgerTxn");
}

void
InMemoryLedgerTxn::erase(InternalLedgerKey const& key)
{
    throw std::runtime_error("called erase on InMemoryLedgerTxn");
}

LedgerTxnEntry
InMemoryLedgerTxn::load(InternalLedgerKey const& key)
{
    throw std::runtime_error("called load on InMemoryLedgerTxn");
}

ConstLedgerTxnEntry
InMemoryLedgerTxn::loadWithoutRecord(InternalLedgerKey const& key)
{
    throw std::runtime_error("called loadWithoutRecord on InMemoryLedgerTxn");
}

UnorderedMap<LedgerKey, LedgerEntry>
InMemoryLedgerTxn::getOffersByAccountAndAsset(AccountID const& account,
                                              Asset const& asset)
{
    auto it = mOffersAndPoolShareTrustlineKeys.find(account);
    if (it == mOffersAndPoolShareTrustlineKeys.end())
    {
        return {};
    }

    UnorderedMap<LedgerKey, LedgerEntry> res;

    auto const& ledgerKeys = it->second;
    for (auto const& key : ledgerKeys)
    {
        if (key.ledgerKey().type() != OFFER)
        {
            continue;
        }

        auto newest = getNewestVersion(key);
        if (!newest)
        {
            throw std::runtime_error("Invalid ledger state");
        }

        auto const& offer = newest->ledgerEntry().data.offer();
        if (offer.buying == asset || offer.selling == asset)
        {
            res.emplace(key.ledgerKey(), newest->ledgerEntry());
        }
    }

    return res;
}

UnorderedMap<LedgerKey, LedgerEntry>
InMemoryLedgerTxn::getPoolShareTrustLinesByAccountAndAsset(
    AccountID const& account, Asset const& asset)
{
    auto it = mOffersAndPoolShareTrustlineKeys.find(account);
    if (it == mOffersAndPoolShareTrustlineKeys.end())
    {
        return {};
    }

    UnorderedMap<LedgerKey, LedgerEntry> res;

    auto const& ledgerKeys = it->second;
    for (auto const& key : ledgerKeys)
    {
        if (key.ledgerKey().type() != TRUSTLINE ||
            key.ledgerKey().trustLine().asset.type() != ASSET_TYPE_POOL_SHARE)
        {
            continue;
        }

        auto pool = getNewestVersion(liquidityPoolKey(
            key.ledgerKey().trustLine().asset.liquidityPoolID()));
        if (!pool)
        {
            throw std::runtime_error("Invalid ledger state");
        }

        auto const& lp = pool->ledgerEntry().data.liquidityPool();
        auto const& cp = lp.body.constantProduct();
        if (cp.params.assetA == asset || cp.params.assetB == asset)
        {
            auto newest = getNewestVersion(key);
            if (!newest)
            {
                throw std::runtime_error("Invalid ledger state");
            }

            res.emplace(key.ledgerKey(), newest->ledgerEntry());
        }
    }

    return res;
}

}
