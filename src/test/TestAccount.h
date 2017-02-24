#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "ledger/OfferFrame.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{

class Application;

namespace txtest
{
struct ThresholdSetter;
}

class TestAccount
{
  public:
    static TestAccount createRoot(Application& app);

    explicit TestAccount(Application& app, SecretKey sk, SequenceNumber sn)
        : mApp(app), mSk{std::move(sk)}, mSn{sn}
    {
    }

    TestAccount create(SecretKey const& secretKey, uint64_t initialBalance);
    TestAccount create(std::string const& name, uint64_t initialBalance);
    void merge(PublicKey const& into);

    void changeTrust(Asset const& asset, int64_t limit);
    void allowTrust(Asset const& asset, PublicKey const& trustor);
    void denyTrust(Asset const& asset, PublicKey const& trustor);

    void setOptions(AccountID* inflationDest, uint32_t* setFlags,
                    uint32_t* clearFlags, txtest::ThresholdSetter* thrs,
                    Signer* signer, std::string* homeDomain);

    void manageData(std::string const& name, DataValue* value);

    OfferFrame::pointer loadOffer(uint64_t offerID) const;
    bool hasOffer(uint64_t offerID) const;
    uint64_t
    manageOffer(uint64_t offerID, Asset const& selling, Asset const& buying,
                Price const& price, int64_t amount,
                ManageOfferEffect expectedEffect = MANAGE_OFFER_CREATED);
    uint64_t
    createPassiveOffer(Asset const& selling, Asset const& buying,
                       Price const& price, int64_t amount,
                       ManageOfferEffect expectedEffect = MANAGE_OFFER_CREATED);
    void pay(SecretKey const& destination, int64_t amount);
    void pay(PublicKey const& destination, Asset const& selling,
             int64_t amount);
    PathPaymentResult pay(PublicKey const& destination, Asset const& sendCur,
                          int64_t sendMax, Asset const& destCur,
                          int64_t destAmount, std::vector<Asset> const& path,
                          Asset* noIssuer = nullptr);

    operator SecretKey() const
    {
        return getSecretKey();
    }
    operator PublicKey() const
    {
        return getPublicKey();
    }

    const SecretKey&
    getSecretKey() const
    {
        return mSk;
    }
    PublicKey
    getPublicKey() const
    {
        return getSecretKey().getPublicKey();
    }
    SequenceNumber
    getLastSequenceNumber() const
    {
        return mSn;
    }
    SequenceNumber
    nextSequenceNumber()
    {
        return ++mSn;
    }

  private:
    Application& mApp;
    SecretKey mSk;
    SequenceNumber mSn;
};
}
