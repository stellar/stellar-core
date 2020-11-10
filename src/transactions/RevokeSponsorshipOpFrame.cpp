// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/RevokeSponsorshipOpFrame.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

RevokeSponsorshipOpFrame::RevokeSponsorshipOpFrame(Operation const& op,
                                                   OperationResult& res,
                                                   TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mRevokeSponsorshipOp(mOperation.body.revokeSponsorshipOp())
{
}

bool
RevokeSponsorshipOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 14;
}

static AccountID const&
getAccountID(LedgerEntry const& le)
{
    switch (le.data.type())
    {
    case ACCOUNT:
        return le.data.account().accountID;
    case TRUSTLINE:
        return le.data.trustLine().accountID;
    case OFFER:
        return le.data.offer().sellerID;
    case DATA:
        return le.data.data().accountID;
    case CLAIMABLE_BALANCE:
        return *le.ext.v1().sponsoringID;
    default:
        abort();
    }
}

bool
RevokeSponsorshipOpFrame::processSponsorshipResult(SponsorshipResult sr)
{
    switch (sr)
    {
    case SponsorshipResult::SUCCESS:
        return true;
    case SponsorshipResult::LOW_RESERVE:
        innerResult().code(REVOKE_SPONSORSHIP_LOW_RESERVE);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORING:
        mResult.code(opTOO_MANY_SPONSORING);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORED:
        // This is impossible right now because there is a limit on sub
        // entries, fall through and throw
    default:
        throw std::runtime_error("unexpected sponsorship result");
    }
}

bool
RevokeSponsorshipOpFrame::updateLedgerEntrySponsorship(AbstractLedgerTxn& ltx)
{
    auto ltxe = ltx.load(mRevokeSponsorshipOp.ledgerKey());
    if (!ltxe)
    {
        innerResult().code(REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
        return false;
    }
    auto& le = ltxe.current();

    bool wasEntrySponsored = false;
    if (le.ext.v() == 1)
    {
        auto& extV1 = le.ext.v1();

        if (extV1.sponsoringID)
        {
            if (!(*extV1.sponsoringID == getSourceID()))
            {
                // The entry is sponsored, so the sponsor would have to be the
                // source account
                innerResult().code(REVOKE_SPONSORSHIP_NOT_SPONSOR);
                return false;
            }

            wasEntrySponsored = true;
        }
        else if (!(getAccountID(le) == getSourceID()))
        {
            // The entry is not sponsored, so the owner would have to be the
            // source account
            innerResult().code(REVOKE_SPONSORSHIP_NOT_SPONSOR);
            return false;
        }
    }
    else if (!(getAccountID(le) == getSourceID()))
    {
        // The entry is not sponsored, so the owner would have to be the source
        // account
        innerResult().code(REVOKE_SPONSORSHIP_NOT_SPONSOR);
        return false;
    }

    // getSourceID() = A
    // getAccountID(le) = B
    //
    // SponsoringFutureReserves(A) = <null> -> Sponsor(le) = B
    // SponsoringFutureReserves(A) = B      -> Sponsor(le) = B
    // SponsoringFutureReserves(A) = C != B -> Sponsor(le) = C

    bool willEntryBeSponsored = false;
    auto sponsorship = loadSponsorship(ltx, getSourceID());
    if (sponsorship)
    {
        auto const& se = sponsorship.currentGeneralized().sponsorshipEntry();
        if (!(se.sponsoringID == getAccountID(le)))
        {
            willEntryBeSponsored = true;
        }
    }

    if (!willEntryBeSponsored && le.data.type() == CLAIMABLE_BALANCE)
    {
        innerResult().code(REVOKE_SPONSORSHIP_ONLY_TRANSFERABLE);
        return false;
    }

    auto header = ltx.loadHeader();
    if (wasEntrySponsored && willEntryBeSponsored)
    {
        // Transfer sponsorship
        auto oldSponsoringAcc = loadAccount(ltx, *le.ext.v1().sponsoringID);
        auto const& se = sponsorship.currentGeneralized().sponsorshipEntry();
        auto newSponsoringAcc = loadAccount(ltx, se.sponsoringID);
        auto res = canTransferEntrySponsorship(header.current(), le,
                                               oldSponsoringAcc.current(),
                                               newSponsoringAcc.current());
        if (!processSponsorshipResult(res))
        {
            return false;
        }
        transferEntrySponsorship(le, oldSponsoringAcc.current(),
                                 newSponsoringAcc.current());
    }
    else if (wasEntrySponsored && !willEntryBeSponsored)
    {
        // Remove sponsorship
        auto oldSponsoringAcc = loadAccount(ltx, *le.ext.v1().sponsoringID);
        if (le.data.type() == ACCOUNT)
        {
            if (!tryRemoveEntrySponsorship(ltx, header, le,
                                           oldSponsoringAcc.current(), le))
            {
                return false;
            }
        }
        else
        {
            auto sponsoredAcc = loadAccount(ltx, getAccountID(le));
            if (!tryRemoveEntrySponsorship(ltx, header, le,
                                           oldSponsoringAcc.current(),
                                           sponsoredAcc.current()))
            {
                return false;
            }
        }
    }
    else if (!wasEntrySponsored && willEntryBeSponsored)
    {
        // Establish sponsorship
        auto const& se = sponsorship.currentGeneralized().sponsorshipEntry();
        auto sponsoringAcc = loadAccount(ltx, se.sponsoringID);
        if (le.data.type() == ACCOUNT)
        {
            if (!tryEstablishEntrySponsorship(ltx, header, le,
                                              sponsoringAcc.current(), le))
            {
                return false;
            }
        }
        else
        {
            auto sponsoredAcc = loadAccount(ltx, getAccountID(le));
            if (!tryEstablishEntrySponsorship(ltx, header, le,
                                              sponsoringAcc.current(),
                                              sponsoredAcc.current()))
            {
                return false;
            }
        }
    }
    else // (!wasEntrySponsored && !willEntryBeSponsored)
    {
        // No-op
    }

    innerResult().code(REVOKE_SPONSORSHIP_SUCCESS);
    return true;
}

bool
RevokeSponsorshipOpFrame::tryRemoveEntrySponsorship(
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header, LedgerEntry& le,
    LedgerEntry& sponsoringAcc, LedgerEntry& sponsoredAcc)
{
    auto res = canRemoveEntrySponsorship(header.current(), le, sponsoringAcc,
                                         &sponsoredAcc);
    if (!processSponsorshipResult(res))
    {
        return false;
    }
    removeEntrySponsorship(le, sponsoringAcc, &sponsoredAcc);
    return true;
}
bool
RevokeSponsorshipOpFrame::tryEstablishEntrySponsorship(
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header, LedgerEntry& le,
    LedgerEntry& sponsoringAcc, LedgerEntry& sponsoredAcc)
{
    auto res = canEstablishEntrySponsorship(header.current(), le, sponsoringAcc,
                                            &sponsoredAcc);
    if (!processSponsorshipResult(res))
    {
        return false;
    }
    establishEntrySponsorship(le, sponsoringAcc, &sponsoredAcc);
    return true;
}

bool
RevokeSponsorshipOpFrame::updateSignerSponsorship(AbstractLedgerTxn& ltx)
{
    auto const& accountID = mRevokeSponsorshipOp.signer().accountID;
    auto sponsoredAcc = loadAccount(ltx, accountID);
    if (!sponsoredAcc)
    {
        innerResult().code(REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
        return false;
    }
    auto& ae = sponsoredAcc.current().data.account();

    auto findRes = findSignerByKey(ae.signers.begin(), ae.signers.end(),
                                   mRevokeSponsorshipOp.signer().signerKey);
    if (!findRes.second)
    {
        innerResult().code(REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
        return false;
    }
    auto it = findRes.first;
    size_t index = it - ae.signers.begin();

    bool wasSignerSponsored = false;
    if (hasAccountEntryExtV2(ae))
    {
        auto& extV2 = ae.ext.v1().ext.v2();
        if (index >= extV2.signerSponsoringIDs.size())
        {
            throw std::runtime_error("bad signer sponsorships");
        }

        auto sid = extV2.signerSponsoringIDs.at(index);
        if (sid)
        {
            if (!(*sid == getSourceID()))
            {
                // The account is sponsored, so the sponsor would have to be the
                // source account
                innerResult().code(REVOKE_SPONSORSHIP_NOT_SPONSOR);
                return false;
            }

            wasSignerSponsored = true;
        }
        else if (!(accountID == getSourceID()))
        {
            // The account is paying its own reserve, so it would have to be the
            // source account
            innerResult().code(REVOKE_SPONSORSHIP_NOT_SPONSOR);
            return false;
        }
    }
    else if (!(accountID == getSourceID()))
    {
        // The account is paying its own reserve, so it would have to be the
        // source account
        innerResult().code(REVOKE_SPONSORSHIP_NOT_SPONSOR);
        return false;
    }

    // getSourceID() = A
    // accountID = B
    //
    // SponsoringFutureReserves(A) = <null> -> Sponsor(it) = B
    // SponsoringFutureReserves(A) = B      -> Sponsor(it) = B
    // SponsoringFutureResreves(A) = C != B -> Sponsor(it) = C

    bool willSignerBeSponsored = false;
    auto sponsorship = loadSponsorship(ltx, getSourceID());
    if (sponsorship)
    {
        auto const& se = sponsorship.currentGeneralized().sponsorshipEntry();
        if (!(se.sponsoringID == accountID))
        {
            willSignerBeSponsored = true;
        }
    }

    auto header = ltx.loadHeader();
    if (wasSignerSponsored && willSignerBeSponsored)
    {
        // Transfer sponsorship
        auto const& ssIDs = ae.ext.v1().ext.v2().signerSponsoringIDs;
        auto oldSponsoringAcc = loadAccount(ltx, *ssIDs.at(index));
        auto const& se = sponsorship.currentGeneralized().sponsorshipEntry();
        auto newSponsoringAcc = loadAccount(ltx, se.sponsoringID);
        auto res = canTransferSignerSponsorship(
            header.current(), it, oldSponsoringAcc.current(),
            newSponsoringAcc.current(), sponsoredAcc.current());
        if (!processSponsorshipResult(res))
        {
            return false;
        }
        transferSignerSponsorship(it, oldSponsoringAcc.current(),
                                  newSponsoringAcc.current(),
                                  sponsoredAcc.current());
    }
    else if (wasSignerSponsored && !willSignerBeSponsored)
    {
        // Remove sponsorship
        auto const& ssIDs = ae.ext.v1().ext.v2().signerSponsoringIDs;
        auto oldSponsoringAcc = loadAccount(ltx, *ssIDs.at(index));
        auto res = canRemoveSignerSponsorship(header.current(), it,
                                              oldSponsoringAcc.current(),
                                              sponsoredAcc.current());
        if (!processSponsorshipResult(res))
        {
            return false;
        }
        removeSignerSponsorship(it, oldSponsoringAcc.current(),
                                sponsoredAcc.current());
    }
    else if (!wasSignerSponsored && willSignerBeSponsored)
    {
        // Establish sponsorship
        auto const& se = sponsorship.currentGeneralized().sponsorshipEntry();
        auto sponsoringAcc = loadAccount(ltx, se.sponsoringID);
        auto res = canEstablishSignerSponsorship(header.current(), it,
                                                 sponsoringAcc.current(),
                                                 sponsoredAcc.current());
        if (!processSponsorshipResult(res))
        {
            return false;
        }
        establishSignerSponsorship(it, sponsoringAcc.current(),
                                   sponsoredAcc.current());
    }
    else // (!wasSignerSponsored && !willSignerBeSponsored)
    {
        // No-op
    }

    innerResult().code(REVOKE_SPONSORSHIP_SUCCESS);
    return true;
}

bool
RevokeSponsorshipOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    switch (mRevokeSponsorshipOp.type())
    {
    case REVOKE_SPONSORSHIP_LEDGER_ENTRY:
        return updateLedgerEntrySponsorship(ltx);
    case REVOKE_SPONSORSHIP_SIGNER:
        return updateSignerSponsorship(ltx);
    default:
        abort();
    }
}

bool
RevokeSponsorshipOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (ledgerVersion <= 14)
    {
        return true;
    }

    if (mRevokeSponsorshipOp.type() == REVOKE_SPONSORSHIP_LEDGER_ENTRY)
    {
        auto const& lk = mRevokeSponsorshipOp.ledgerKey();
        switch (lk.type())
        {
        case ACCOUNT:
            break;
        case TRUSTLINE:
        {
            auto const& tl = lk.trustLine();
            if (!isAssetValid(tl.asset) ||
                (tl.asset.type() == ASSET_TYPE_NATIVE) ||
                (getIssuer(tl.asset) == tl.accountID))
            {
                innerResult().code(REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
                return false;
            }
            break;
        }
        case OFFER:
            if (lk.offer().offerID <= 0)
            {
                innerResult().code(REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
                return false;
            }
            break;
        case DATA:
        {
            auto const& name = lk.data().dataName;
            if ((name.size() < 1) || !isString32Valid(name))
            {
                innerResult().code(REVOKE_SPONSORSHIP_DOES_NOT_EXIST);
                return false;
            }
            break;
        }
        case CLAIMABLE_BALANCE:
            break;
        default:
            abort();
        }
    }
    return true;
}
}
