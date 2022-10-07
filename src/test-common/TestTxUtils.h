#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "overlay/StellarXDR.h"
#include "test-common/TestAccount.h"
#include "transactions/TransactionFrame.h"
#include <optional>

namespace stellar
{
namespace txtest
{

struct SetOptionsArguments
{
    std::optional<int> masterWeight;
    std::optional<int> lowThreshold;
    std::optional<int> medThreshold;
    std::optional<int> highThreshold;
    std::optional<Signer> signer;
    std::optional<uint32_t> setFlags;
    std::optional<uint32_t> clearFlags;
    std::optional<AccountID> inflationDest;
    std::optional<std::string> homeDomain;

    friend SetOptionsArguments operator|(SetOptionsArguments const& x,
                                         SetOptionsArguments const& y);
};

struct SetTrustLineFlagsArguments
{
    uint32_t setFlags = 0;
    uint32_t clearFlags = 0;
};

SetTrustLineFlagsArguments operator|(SetTrustLineFlagsArguments const& x,
                                     SetTrustLineFlagsArguments const& y);

SecretKey getRoot(Hash const& networkID);

SecretKey getAccount(std::string const& n);

Signer makeSigner(SecretKey key, int weight);

Operation changeTrust(Asset const& asset, int64_t limit);
Operation changeTrust(ChangeTrustAsset const& asset, int64_t limit);

Operation allowTrust(PublicKey const& trustor, Asset const& asset,
                     uint32_t authorize);

Operation allowTrust(PublicKey const& trustor, Asset const& asset,
                     uint32_t authorize);

Operation inflation();

Operation accountMerge(PublicKey const& dest);

Operation manageData(std::string const& name, DataValue* value);

Operation bumpSequence(SequenceNumber to);

Operation createAccount(PublicKey const& dest, int64_t amount);

Operation payment(PublicKey const& to, int64_t amount);

Operation payment(PublicKey const& to, Asset const& asset, int64_t amount);

Operation createClaimableBalance(Asset const& asset, int64_t amount,
                                 xdr::xvector<Claimant, 10> const& claimants);

Operation claimClaimableBalance(ClaimableBalanceID const& balanceID);

TransactionFramePtr transactionFromOperationsV0(
    Application& app, SecretKey const& from, SequenceNumber seq,
    std::vector<Operation> const& ops, uint32_t fee = 0);

TransactionFramePtr
transactionFromOperationsV1(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            std::vector<Operation> const& ops, uint32_t fee,
                            std::optional<PreconditionsV2> cond = std::nullopt);
TransactionFramePtr transactionFromOperations(Application& app,
                                              SecretKey const& from,
                                              SequenceNumber seq,
                                              std::vector<Operation> const& ops,
                                              uint32_t fee = 0);
TransactionFramePtr transactionWithV2Precondition(Application& app,
                                                  TestAccount& account,
                                                  int64_t sequenceDelta,
                                                  uint32_t fee,
                                                  PreconditionsV2 const& cond);

TransactionFramePtr createPaymentTx(Application& app, SecretKey const& from,
                                    PublicKey const& to, SequenceNumber seq,
                                    int64_t amount);

TransactionFramePtr createCreditPaymentTx(Application& app,
                                          SecretKey const& from,
                                          PublicKey const& to, Asset const& ci,
                                          SequenceNumber seq, int64_t amount);

Operation pathPayment(PublicKey const& to, Asset const& sendCur,
                      int64_t sendMax, Asset const& destCur, int64_t destAmount,
                      std::vector<Asset> const& path);

Operation pathPaymentStrictSend(PublicKey const& to, Asset const& sendCur,
                                int64_t sendAmount, Asset const& destCur,
                                int64_t destMin,
                                std::vector<Asset> const& path);

Operation manageOffer(int64 offerId, Asset const& selling, Asset const& buying,
                      Price const& price, int64_t amount);
Operation manageBuyOffer(int64 offerId, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount);

Operation createPassiveOffer(Asset const& selling, Asset const& buying,
                             Price const& price, int64_t amount);

Operation setOptions(SetOptionsArguments const& arguments);

SetOptionsArguments setMasterWeight(int master);
SetOptionsArguments setLowThreshold(int low);
SetOptionsArguments setMedThreshold(int med);
SetOptionsArguments setHighThreshold(int high);
SetOptionsArguments setSigner(Signer signer);
SetOptionsArguments setFlags(uint32_t setFlags);
SetOptionsArguments clearFlags(uint32_t clearFlags);
SetOptionsArguments setInflationDestination(AccountID inflationDest);
SetOptionsArguments setHomeDomain(std::string const& homeDomain);

Operation setTrustLineFlags(PublicKey const& trustor, Asset const& asset,
                            SetTrustLineFlagsArguments const& arguments);

SetTrustLineFlagsArguments setTrustLineFlags(uint32_t setFlags);
SetTrustLineFlagsArguments clearTrustLineFlags(uint32_t clearFlags);

Operation beginSponsoringFutureReserves(PublicKey const& sponsoredID);
Operation endSponsoringFutureReserves();
Operation revokeSponsorship(LedgerKey const& key);
Operation revokeSponsorship(AccountID const& accID, SignerKey const& key);

Operation clawback(AccountID const& from, Asset const& asset, int64_t amount);
Operation clawbackClaimableBalance(ClaimableBalanceID const& balanceID);

Operation liquidityPoolDeposit(PoolID const& poolID, int64_t maxAmountA,
                               int64_t maxAmountB, Price const& minPrice,
                               Price const& maxPrice);
Operation liquidityPoolWithdraw(PoolID const& poolID, int64_t amount,
                                int64_t minAmountA, int64_t minAmountB);

Asset makeNativeAsset();
Asset makeInvalidAsset();
Asset makeAsset(SecretKey const& issuer, std::string const& code);
Asset makeAssetAlphanum12(SecretKey const& issuer, std::string const& code);
}
}