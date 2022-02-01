// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "crypto/SecretKey.h"
#include "ledger/LedgerTxn.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{
namespace txsimulation
{
// generateScaledSecret functions generate new key pairs based on some pubkey
// (typically used in production) and a partition number
SecretKey generateScaledSecret(AccountID const& key, uint32_t partition);
SecretKey generateScaledSecret(MuxedAccount const& key, uint32_t partition);
int64_t generateScaledOfferID(int64_t offerId, uint32_t partition);
int64_t generateScaledOfferID(OperationResult const& result,
                              uint32_t partition);
Hash generateScaledClaimableBalanceID(OperationResult const& result,
                                      uint32_t partition);

Hash generateScaledClaimableBalanceID(Hash const& balanceID,
                                      uint32_t partition);
void generateScaledLiveEntries(
    std::vector<LedgerEntry>& entries,
    std::vector<LedgerEntry> const& oldEntries,
    UnorderedMap<PoolID, LiquidityPoolConstantProductParameters>& poolIDToParam,
    uint32_t partition);

void generateScaledDeadEntries(
    std::vector<LedgerKey>& keys, std::vector<LedgerKey> const& oldKeys,
    UnorderedMap<PoolID, LiquidityPoolConstantProductParameters> const&
        poolIDToParam,
    uint32_t partition);

SignerKey generateScaledEd25519Signer(Signer const& signer, uint32_t partition);

SecretKey mutateScaledAccountID(AccountID& acc, uint32_t partition);
SecretKey mutateScaledAccountID(MuxedAccount& acc, uint32_t partition);
void mutateScaledOperation(
    Operation& op, AbstractLedgerTxn& ltx,
    UnorderedMap<PoolID, LiquidityPoolParameters>& ctPoolIdToParam,
    uint32_t partition);
}
}
