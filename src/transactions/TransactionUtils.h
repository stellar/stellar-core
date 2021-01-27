#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include <algorithm>

namespace stellar
{

class Application;
class ConstLedgerTxnEntry;
class ConstTrustLineWrapper;
class AbstractLedgerTxn;
class LedgerTxnEntry;
class LedgerTxnHeader;
class TrustLineWrapper;
class InternalLedgerKey;
struct LedgerHeader;
struct LedgerKey;
struct TransactionEnvelope;
struct MuxedAccount;

template <typename IterType>
std::pair<IterType, bool>
findSignerByKey(IterType begin, IterType end, SignerKey const& key)
{
    auto it =
        std::find_if(begin, end, [&](auto const& x) { return !(x.key < key); });
    bool found = (it != end && it->key == key);
    return {it, found};
}

AccountEntryExtensionV1& prepareAccountEntryExtensionV1(AccountEntry& ae);
AccountEntryExtensionV2& prepareAccountEntryExtensionV2(AccountEntry& ae);
TrustLineEntry::_ext_t::_v1_t&
prepareTrustLineEntryExtensionV1(TrustLineEntry& tl);
LedgerEntryExtensionV1& prepareLedgerEntryExtensionV1(LedgerEntry& le);

AccountEntryExtensionV2& getAccountEntryExtensionV2(AccountEntry& ae);
LedgerEntryExtensionV1& getLedgerEntryExtensionV1(LedgerEntry& le);

LedgerKey accountKey(AccountID const& accountID);
LedgerKey trustlineKey(AccountID const& accountID, Asset const& asset);
LedgerKey offerKey(AccountID const& sellerID, uint64_t offerID);
LedgerKey dataKey(AccountID const& accountID, std::string const& dataName);
LedgerKey claimableBalanceKey(ClaimableBalanceID const& balanceID);
InternalLedgerKey sponsorshipKey(AccountID const& sponsoredID);
InternalLedgerKey sponsorshipCounterKey(AccountID const& sponsoringID);

uint32_t const FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS = 11;
uint32_t const ACCOUNT_SUBENTRY_LIMIT = 1000;
int32_t const EXPECTED_CLOSE_TIME_MULT = 2;
size_t const MAX_OFFERS_TO_CROSS = 1000;

LedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx, AccountID const& accountID);

ConstLedgerTxnEntry loadAccountWithoutRecord(AbstractLedgerTxn& ltx,
                                             AccountID const& accountID);

LedgerTxnEntry loadData(AbstractLedgerTxn& ltx, AccountID const& accountID,
                        std::string const& dataName);

LedgerTxnEntry loadOffer(AbstractLedgerTxn& ltx, AccountID const& sellerID,
                         int64_t offerID);

LedgerTxnEntry loadClaimableBalance(AbstractLedgerTxn& ltx,
                                    ClaimableBalanceID const& balanceID);

TrustLineWrapper loadTrustLine(AbstractLedgerTxn& ltx,
                               AccountID const& accountID, Asset const& asset);

ConstTrustLineWrapper loadTrustLineWithoutRecord(AbstractLedgerTxn& ltx,
                                                 AccountID const& accountID,
                                                 Asset const& asset);

TrustLineWrapper loadTrustLineIfNotNative(AbstractLedgerTxn& ltx,
                                          AccountID const& accountID,
                                          Asset const& asset);

ConstTrustLineWrapper loadTrustLineWithoutRecordIfNotNative(
    AbstractLedgerTxn& ltx, AccountID const& accountID, Asset const& asset);

LedgerTxnEntry loadSponsorship(AbstractLedgerTxn& ltx,
                               AccountID const& sponsoredID);

LedgerTxnEntry loadSponsorshipCounter(AbstractLedgerTxn& ltx,
                                      AccountID const& sponsoringID);

void acquireLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                        LedgerTxnEntry const& offer);

bool addBalanceSkipAuthorization(LedgerTxnHeader const& header,
                                 LedgerTxnEntry& entry, int64_t amount);

bool addBalance(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                int64_t delta);

bool addBuyingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                          int64_t delta);

bool addSellingLiabilities(LedgerTxnHeader const& header, LedgerTxnEntry& entry,
                           int64_t delta);

uint64_t generateID(LedgerTxnHeader& header);

int64_t getAvailableBalance(LedgerHeader const& header, LedgerEntry const& le);
int64_t getAvailableBalance(LedgerTxnHeader const& header,
                            LedgerTxnEntry const& entry);
int64_t getAvailableBalance(LedgerTxnHeader const& header,
                            ConstLedgerTxnEntry const& entry);

int64_t getBuyingLiabilities(LedgerTxnHeader const& header,
                             LedgerEntry const& le);
int64_t getBuyingLiabilities(LedgerTxnHeader const& header,
                             LedgerTxnEntry const& offer);

int64_t getMaxAmountReceive(LedgerTxnHeader const& header,
                            LedgerEntry const& le);
int64_t getMaxAmountReceive(LedgerTxnHeader const& header,
                            LedgerTxnEntry const& entry);
int64_t getMaxAmountReceive(LedgerTxnHeader const& header,
                            ConstLedgerTxnEntry const& entry);

int64_t getMinBalance(LedgerHeader const& header, AccountEntry const& acc);
int64_t getMinBalance(LedgerHeader const& header, uint32_t numSubentries,
                      uint32_t numSponsoring, uint32_t numSponsored);

int64_t getMinimumLimit(LedgerTxnHeader const& header, LedgerEntry const& le);
int64_t getMinimumLimit(LedgerTxnHeader const& header,
                        LedgerTxnEntry const& entry);
int64_t getMinimumLimit(LedgerTxnHeader const& header,
                        ConstLedgerTxnEntry const& entry);

int64_t getOfferBuyingLiabilities(LedgerTxnHeader const& header,
                                  LedgerEntry const& entry);
int64_t getOfferBuyingLiabilities(LedgerTxnHeader const& header,
                                  LedgerTxnEntry const& entry);

int64_t getOfferSellingLiabilities(LedgerTxnHeader const& header,
                                   LedgerEntry const& entry);
int64_t getOfferSellingLiabilities(LedgerTxnHeader const& header,
                                   LedgerTxnEntry const& entry);

int64_t getSellingLiabilities(LedgerHeader const& header,
                              LedgerEntry const& le);
int64_t getSellingLiabilities(LedgerTxnHeader const& header,
                              LedgerTxnEntry const& offer);

uint64_t getStartingSequenceNumber(uint32_t ledgerSeq);
uint64_t getStartingSequenceNumber(LedgerTxnHeader const& header);

bool isAuthorized(LedgerEntry const& le);
bool isAuthorized(LedgerTxnEntry const& entry);
bool isAuthorized(ConstLedgerTxnEntry const& entry);

bool isAuthorizedToMaintainLiabilities(LedgerEntry const& le);
bool isAuthorizedToMaintainLiabilities(LedgerTxnEntry const& entry);
bool isAuthorizedToMaintainLiabilities(ConstLedgerTxnEntry const& entry);

bool isAuthRequired(ConstLedgerTxnEntry const& entry);

bool isClawbackEnabledOnTrustline(LedgerTxnEntry const& entry);
bool isClawbackEnabledOnAccount(LedgerEntry const& entry);
bool isClawbackEnabledOnAccount(LedgerTxnEntry const& entry);
bool isClawbackEnabledOnAccount(ConstLedgerTxnEntry const& entry);
bool isClawbackEnabledOnClaimableBalance(LedgerEntry const& entry);

bool isImmutableAuth(LedgerTxnEntry const& entry);

void releaseLiabilities(AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
                        LedgerTxnEntry const& offer);

AccountID toAccountID(MuxedAccount const& m);
MuxedAccount toMuxedAccount(AccountID const& a);

bool trustLineFlagIsValid(uint32_t flag, uint32_t ledgerVersion);
bool trustLineFlagIsValid(uint32_t flag, LedgerTxnHeader const& header);

bool accountFlagIsValid(uint32_t flag, uint32_t ledgerVersion);
bool accountFlagClawbackIsValid(uint32_t flag, uint32_t ledgerVersion);
bool accountFlagMaskCheckIsValid(uint32_t flag, uint32_t ledgerVersion);

bool hasMuxedAccount(TransactionEnvelope const& e);

uint64_t getUpperBoundCloseTimeOffset(Application& app, uint64_t lastCloseTime);

bool hasAccountEntryExtV2(AccountEntry const& ae);

Asset getAsset(AccountID const& issuer, AssetCode const& assetCode);

bool claimableBalanceFlagIsValid(ClaimableBalanceEntry const& cb);
}
