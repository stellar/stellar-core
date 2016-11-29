#pragma once

#include "crypto/SecretKey.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{

class Application;

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
