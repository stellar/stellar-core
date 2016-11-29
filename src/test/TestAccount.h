#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{

class Application;
class LedgerDelta;

class TestAccount
{
public:
    static TestAccount createRoot(Application& app);

    explicit TestAccount(Application& app, SecretKey sk, SequenceNumber sn) :
        mApp(app),
        mSk{std::move(sk)},
        mSn{sn}
    {}

    TestAccount create(SecretKey const& secretKey, uint64_t initialBalance);
    TestAccount create(std::string const& name, uint64_t initialBalance);

    void changeTrust(Asset const &asset, int64_t limit);
    uint64_t manageOffer(LedgerDelta & delta, uint64_t offerID, Asset const& selling, Asset const& buying, Price const& price, int64_t amount, ManageOfferEffect expectedEffect = MANAGE_OFFER_CREATED);

    operator SecretKey () const { return getSecretKey(); }

    SecretKey getSecretKey() const { return mSk; }
    PublicKey getPublicKey() const { return getSecretKey().getPublicKey(); }
    SequenceNumber getLastSequenceNumber() const { return mSn; }
    SequenceNumber nextSequenceNumber() { return ++mSn; }

private:
    Application& mApp;
    SecretKey mSk;
    SequenceNumber mSn;

};

}
