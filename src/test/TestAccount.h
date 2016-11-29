#pragma once

#include "crypto/SecretKey.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class Application;

class TestAccount
{
public:
    static TestAccount createRoot(Application& app);

    explicit TestAccount(SecretKey sk, SequenceNumber sn) :
        mSk{std::move(sk)},
        mSn{sn}
    {}

    operator SecretKey () const { return getSecretKey(); }

    SecretKey getSecretKey() const { return mSk; }
    PublicKey getPublicKey() const { return getSecretKey().getPublicKey(); }
    SequenceNumber getLastSequenceNumber() const { return mSn; }
    SequenceNumber nextSequenceNumber() { return ++mSn; }

private:
    SecretKey mSk;
    SequenceNumber mSn;

};

}
