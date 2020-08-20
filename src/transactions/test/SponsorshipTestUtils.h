// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include <functional>

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class TestAccount;

void checkSponsorship(AbstractLedgerTxn& ltx, LedgerKey const& lk, int leExt,
                      AccountID const* sponsoringID);

void checkSponsorship(AbstractLedgerTxn& ltx, AccountID const& accountID,
                      SignerKey const& signerKey, int aeExt,
                      AccountID const* sponsoringID);

void checkSponsorship(AbstractLedgerTxn& ltx, AccountID const& acc, int leExt,
                      AccountID const* sponsoringID, uint32_t numSubEntries,
                      int aeExt, uint32_t numSponsoring, uint32_t numSponsored);

void createSponsoredEntryButSponsorHasInsufficientBalance(
    Application& app, TestAccount& sponsoringAcc, TestAccount& sponsoredAcc,
    Operation const& op, std::function<bool(OperationResult const&)> check);

void createModifyAndRemoveSponsoredEntry(
    Application& app, TestAccount& sponsoredAcc, Operation const& opCreate,
    Operation const& opModify1, Operation const& opModify2,
    Operation const& opRemove, LedgerKey const& lk);

void createModifyAndRemoveSponsoredEntry(
    Application& app, TestAccount& sponsoredAcc, Operation const& opCreate,
    Operation const& opModify1, Operation const& opModify2,
    Operation const& opRemove, SignerKey const& signerKey);

void tooManySponsoring(Application& app, TestAccount& sponsoredAcc,
                       Operation const& successfulOp, Operation const& failOp);

void tooManySponsoring(Application& app, TestAccount& successfulOpAcc,
                       TestAccount& failOpAcc, Operation const& successfulOp,
                       Operation const& failOp);
}
