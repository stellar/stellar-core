// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include <vector>

namespace stellar
{
class AbstractLedgerTxn;
class LedgerTxnEntry;
class LedgerTxnHeader;

uint32_t getNumSponsored(LedgerEntry const& le);
uint32_t getNumSponsoring(LedgerEntry const& le);

enum class IsSignerSponsoredResult
{
    DOES_NOT_EXIST,
    NOT_SPONSORED,
    SPONSORED
};

IsSignerSponsoredResult
isSignerSponsored(std::vector<Signer>::const_iterator const& signerIt,
                  LedgerEntry const& sponsoringAcc,
                  LedgerEntry const& sponsoredAcc);

enum class SponsorshipResult
{
    SUCCESS,
    LOW_RESERVE,
    TOO_MANY_SUBENTRIES,
    TOO_MANY_SPONSORING,
    TOO_MANY_SPONSORED
};

SponsorshipResult canEstablishEntrySponsorship(LedgerHeader const& lh,
                                               LedgerEntry const& le,
                                               LedgerEntry const& sponsoringAcc,
                                               LedgerEntry const* sponsoredAcc);
SponsorshipResult canRemoveEntrySponsorship(LedgerHeader const& lh,
                                            LedgerEntry const& le,
                                            LedgerEntry const& sponsoringAcc,
                                            LedgerEntry const* sponsoredAcc);
SponsorshipResult
canTransferEntrySponsorship(LedgerHeader const& lh, LedgerEntry const& le,
                            LedgerEntry const& oldSponsoringAcc,
                            LedgerEntry const& newSponsoringAcc);

void establishEntrySponsorship(LedgerEntry& le, LedgerEntry& sponsoringAcc,
                               LedgerEntry* sponsoredAcc);
void removeEntrySponsorship(LedgerEntry& le, LedgerEntry& sponsoringAcc,
                            LedgerEntry* sponsoredAcc);
void transferEntrySponsorship(LedgerEntry& le, LedgerEntry& oldSponsoringAcc,
                              LedgerEntry& newSponsoringAcc);

SponsorshipResult canEstablishSignerSponsorship(
    LedgerHeader const& lh, std::vector<Signer>::const_iterator const& signerIt,
    LedgerEntry const& sponsoringAcc, LedgerEntry const& sponsoredAcc);
SponsorshipResult canRemoveSignerSponsorship(
    LedgerHeader const& lh, std::vector<Signer>::const_iterator const& signerIt,
    LedgerEntry const& sponsoringAcc, LedgerEntry const& sponsoredAcc);
SponsorshipResult canTransferSignerSponsorship(
    LedgerHeader const& lh, std::vector<Signer>::const_iterator const& signerIt,
    LedgerEntry const& oldSponsoringAcc, LedgerEntry const& newSponsoringAcc,
    LedgerEntry const& sponsoredAcc);

void
establishSignerSponsorship(std::vector<Signer>::const_iterator const& signerIt,
                           LedgerEntry& sponsoringAcc,
                           LedgerEntry& sponsoredAcc);
void
removeSignerSponsorship(std::vector<Signer>::const_iterator const& signerIt,
                        LedgerEntry& sponsoringAcc, LedgerEntry& sponsoredAcc);
void
transferSignerSponsorship(std::vector<Signer>::const_iterator const& signerIt,
                          LedgerEntry& oldSponsoringAcc,
                          LedgerEntry& newSponsoringAcc,
                          LedgerEntry& sponsoredAcc);

SponsorshipResult canCreateEntryWithoutSponsorship(LedgerHeader const& lh,
                                                   LedgerEntry const& le,
                                                   LedgerEntry const& acc);

void createEntryWithoutSponsorship(LedgerEntry& le, LedgerEntry& acc);

SponsorshipResult
createEntryWithPossibleSponsorship(AbstractLedgerTxn& ltx,
                                   LedgerTxnHeader const& header,
                                   LedgerEntry& le, LedgerTxnEntry& acc);
void removeEntryWithPossibleSponsorship(AbstractLedgerTxn& ltx,
                                        LedgerTxnHeader const& header,
                                        LedgerEntry& le, LedgerTxnEntry& acc);

SponsorshipResult createSignerWithPossibleSponsorship(
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
    std::vector<Signer>::const_iterator const& signerIt, LedgerTxnEntry& acc);
void removeSignerWithPossibleSponsorship(
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
    std::vector<Signer>::const_iterator const& signerIt, LedgerTxnEntry& acc);
}
