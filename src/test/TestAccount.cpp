// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestAccount.h"

#include "main/Application.h"
#include "test/TxTests.h"

namespace stellar
{

using namespace txtest;

TestAccount
TestAccount::createRoot(Application& app)
{
    auto secretKey = getRoot(app.getNetworkID());
    auto sequenceNumber = getAccountSeqNum(secretKey, app);
    return TestAccount{app, secretKey, sequenceNumber};
}

TestAccount
TestAccount::create(SecretKey const& secretKey, uint64_t initialBalance)
{
    applyCreateAccountTx(mApp, getSecretKey(), secretKey, nextSequenceNumber(),
                         initialBalance);
    auto sequenceNumber = getAccountSeqNum(secretKey, mApp);
    return TestAccount{mApp, secretKey, sequenceNumber};
}

TestAccount
TestAccount::create(std::string const& name, uint64_t initialBalance)
{
    return create(getAccount(name.c_str()), initialBalance);
}

void
TestAccount::merge(PublicKey const& into)
{
    applyAccountMerge(mApp, getSecretKey(), into, nextSequenceNumber());
}

void
TestAccount::changeTrust(Asset const& asset, int64_t limit)
{
    auto assetCode = std::string{};
    assetCodeToStr(asset.alphaNum4().assetCode, assetCode);
    applyChangeTrust(mApp, getSecretKey(), asset.alphaNum4().issuer,
                     nextSequenceNumber(), assetCode, limit);
}

void
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor)
{
    auto assetCode = std::string{};
    assetCodeToStr(asset.alphaNum4().assetCode, assetCode);
    applyAllowTrust(mApp, getSecretKey(), trustor, nextSequenceNumber(),
                    assetCode, true);
}

void
TestAccount::denyTrust(Asset const& asset, PublicKey const& trustor)
{
    auto assetCode = std::string{};
    assetCodeToStr(asset.alphaNum4().assetCode, assetCode);
    applyAllowTrust(mApp, getSecretKey(), trustor, nextSequenceNumber(),
                    assetCode, false);
}

void
TestAccount::setOptions(AccountID* inflationDest, uint32_t* setFlags,
                        uint32_t* clearFlags, ThresholdSetter* thrs,
                        Signer* signer, std::string* homeDomain)
{
    applySetOptions(mApp, getSecretKey(), nextSequenceNumber(), inflationDest,
                    setFlags, clearFlags, thrs, signer, homeDomain);
}

void
TestAccount::manageData(std::string const& name, DataValue* value)
{
    applyManageData(mApp, getSecretKey(), name, value, nextSequenceNumber());
}

OfferFrame::pointer
TestAccount::loadOffer(uint64_t offerID) const
{
    return txtest::loadOffer(getPublicKey(), offerID, mApp, true);
}

bool
TestAccount::hasOffer(uint64_t offerID) const
{
    return !!txtest::loadOffer(getPublicKey(), offerID, mApp, false);
}

uint64_t
TestAccount::manageOffer(uint64_t offerID, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount, ManageOfferEffect expectedEffect)
{
    return applyManageOffer(mApp, offerID, getSecretKey(), selling, buying,
                            price, amount, nextSequenceNumber(),
                            expectedEffect);
}

uint64_t
TestAccount::createPassiveOffer(Asset const& selling, Asset const& buying,
                                Price const& price, int64_t amount,
                                ManageOfferEffect expectedEffect)
{
    return applyCreatePassiveOffer(mApp, getSecretKey(), selling, buying, price,
                                   amount, nextSequenceNumber(),
                                   expectedEffect);
}

void
TestAccount::pay(SecretKey const& destination, int64_t amount)
{
    applyPaymentTx(mApp, getSecretKey(), destination, nextSequenceNumber(),
                   amount);
}

void
TestAccount::pay(PublicKey const& destination, Asset const& selling,
                 int64_t amount)
{
    applyCreditPaymentTx(mApp, getSecretKey(), destination, selling,
                         nextSequenceNumber(), amount);
}

PathPaymentResult
TestAccount::pay(PublicKey const& destination, Asset const& sendCur,
                 int64_t sendMax, Asset const& destCur, int64_t destAmount,
                 std::vector<Asset> const& path, Asset* noIssuer)
{
    return applyPathPaymentTx(mApp, getSecretKey(), destination, sendCur,
                              sendMax, destCur, destAmount,
                              nextSequenceNumber(), path, noIssuer);
}
};
