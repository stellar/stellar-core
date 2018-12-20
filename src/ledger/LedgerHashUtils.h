#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSliceHasher.h"
#include "xdr/Stellar-ledger.h"
#include <functional>

// implements a default hasher for "LedgerKey"
namespace std
{
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
            switch (lk.trustLine().asset.type())
            {
            case stellar::ASSET_TYPE_NATIVE:
                break;
            case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
            {
                auto& tl4 = tl.asset.alphaNum4();
                res ^= stellar::shortHash::computeHash(
                    stellar::ByteSlice(tl4.issuer.ed25519().data(), 8));
                res ^= tl4.assetCode[0];
                break;
            }
            case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
            {
                auto& tl12 = tl.asset.alphaNum12();
                res ^= stellar::shortHash::computeHash(
                    stellar::ByteSlice(tl12.issuer.ed25519().data(), 8));
                res ^= tl12.assetCode[0];
                break;
            }
            default:
                abort();
            }
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
        default:
            abort();
        }
        return res;
    }
};
}
