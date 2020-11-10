#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ShortHash.h"
#include "ledger/InternalLedgerEntry.h"
#include "xdr/Stellar-ledger.h"
#include <functional>

// implements a default hasher for "LedgerKey"
namespace std
{
template <> class hash<stellar::Asset>
{
  public:
    size_t
    operator()(stellar::Asset const& asset) const
    {
        size_t res = asset.type();
        switch (asset.type())
        {
        case stellar::ASSET_TYPE_NATIVE:
            break;
        case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        {
            auto& a4 = asset.alphaNum4();
            res ^= stellar::shortHash::computeHash(
                stellar::ByteSlice(a4.issuer.ed25519().data(), 8));
            res ^= a4.assetCode[0];
            break;
        }
        case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        {
            auto& a12 = asset.alphaNum12();
            res ^= stellar::shortHash::computeHash(
                stellar::ByteSlice(a12.issuer.ed25519().data(), 8));
            res ^= a12.assetCode[0];
            break;
        }
        }
        return res;
    }
};

template <> class hash<stellar::LedgerKey>
{
  public:
    size_t
    operator()(stellar::LedgerKey const& lk) const
    {
        size_t res;
        switch (lk.type())
        {
        case stellar::ACCOUNT:
            res = stellar::shortHash::computeHash(
                stellar::ByteSlice(lk.account().accountID.ed25519().data(), 8));
            break;
        case stellar::TRUSTLINE:
        {
            auto& tl = lk.trustLine();
            res = stellar::shortHash::computeHash(
                stellar::ByteSlice(tl.accountID.ed25519().data(), 8));
            res ^= hash<stellar::Asset>()(tl.asset);
            break;
        }
        case stellar::DATA:
            res = stellar::shortHash::computeHash(
                stellar::ByteSlice(lk.data().accountID.ed25519().data(), 8));
            res ^= stellar::shortHash::computeHash(stellar::ByteSlice(
                lk.data().dataName.data(), lk.data().dataName.size()));
            break;
        case stellar::OFFER:
            res = stellar::shortHash::computeHash(stellar::ByteSlice(
                &lk.offer().offerID, sizeof(lk.offer().offerID)));
            break;
        case stellar::CLAIMABLE_BALANCE:
            res = stellar::shortHash::computeHash(stellar::ByteSlice(
                lk.claimableBalance().balanceID.v0().data(), 8));
            break;
        default:
            abort();
        }
        return res;
    }
};

template <> class hash<stellar::InternalLedgerKey>
{
  public:
    size_t
    operator()(stellar::InternalLedgerKey const& glk) const
    {
        switch (glk.type())
        {
        case stellar::InternalLedgerEntryType::LEDGER_ENTRY:
            return hash<stellar::LedgerKey>()(glk.ledgerKey());
        case stellar::InternalLedgerEntryType::SPONSORSHIP:
            return stellar::shortHash::computeHash(stellar::ByteSlice(
                glk.sponsorshipKey().sponsoredID.ed25519().data(), 8));
        case stellar::InternalLedgerEntryType::SPONSORSHIP_COUNTER:
            return stellar::shortHash::computeHash(stellar::ByteSlice(
                glk.sponsorshipCounterKey().sponsoringID.ed25519().data(), 8));
        default:
            abort();
        }
    }
};
}
