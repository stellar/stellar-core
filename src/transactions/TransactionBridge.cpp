// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionBridge.h"
#include "transactions/TransactionFrame.h"
#include "util/GlobalChecks.h"

#ifdef BUILD_TESTS
#include "transactions/test/TransactionTestFrame.h"
#endif

namespace stellar
{
namespace txbridge
{

TransactionEnvelope
convertForV13(TransactionEnvelope const& input)
{
    if (input.type() != ENVELOPE_TYPE_TX_V0)
    {
        return input;
    }

    auto const& envV0 = input.v0();
    auto const& txV0 = envV0.tx;

    TransactionEnvelope res(ENVELOPE_TYPE_TX);
    auto& envV1 = res.v1();
    auto& txV1 = envV1.tx;

    envV1.signatures = envV0.signatures;
    txV1.sourceAccount.ed25519() = txV0.sourceAccountEd25519;
    txV1.fee = txV0.fee;
    txV1.seqNum = txV0.seqNum;
    txV1.memo = txV0.memo;
    txV1.operations = txV0.operations;

    if (txV0.timeBounds)
    {
        txV1.cond.type(PRECOND_TIME);
        txV1.cond.timeBounds() = *txV0.timeBounds;
    }

    return res;
}

xdr::xvector<DecoratedSignature, 20> const&
getSignatures(TransactionEnvelope const& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return env.v0().signatures;
    case ENVELOPE_TYPE_TX:
        return env.v1().signatures;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return env.feeBump().signatures;
    default:
        abort();
    }
}

xdr::xvector<DecoratedSignature, 20>&
getSignatures(TransactionEnvelope& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return env.v0().signatures;
    case ENVELOPE_TYPE_TX:
        return env.v1().signatures;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return env.feeBump().signatures;
    default:
        abort();
    }
}

xdr::xvector<DecoratedSignature, 20>&
getSignaturesInner(TransactionEnvelope& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return env.v0().signatures;
    case ENVELOPE_TYPE_TX:
        return env.v1().signatures;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        return env.feeBump().tx.innerTx.v1().signatures;
    default:
        abort();
    }
}

xdr::xvector<Operation, MAX_OPS_PER_TX>&
getOperations(TransactionEnvelope& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        return env.v0().tx.operations;
    case ENVELOPE_TYPE_TX:
        return env.v1().tx.operations;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        releaseAssert(env.feeBump().tx.innerTx.type() == ENVELOPE_TYPE_TX);
        return env.feeBump().tx.innerTx.v1().tx.operations;
    default:
        abort();
    }
}

#ifdef BUILD_TESTS
xdr::xvector<DecoratedSignature, 20>&
getSignatures(TransactionTestFramePtr tx)
{
    return getSignatures(tx->getMutableEnvelope());
}

void
setSeqNum(TransactionTestFramePtr tx, int64_t seq)
{
    auto& env = tx->getMutableEnvelope();
    int64_t& s = env.type() == ENVELOPE_TYPE_TX_V0 ? env.v0().tx.seqNum
                                                   : env.v1().tx.seqNum;
    s = seq;
}

void
setFullFee(TransactionTestFramePtr tx, uint32_t totalFee)
{
    auto& env = tx->getMutableEnvelope();
    uint32_t& f =
        env.type() == ENVELOPE_TYPE_TX_V0 ? env.v0().tx.fee : env.v1().tx.fee;
    f = totalFee;
}

void
setSorobanFees(TransactionTestFramePtr tx, uint32_t totalFee, int64 resourceFee)
{
    setFullFee(tx, totalFee);
    auto& env = tx->getMutableEnvelope();
    auto& sorobanData = env.v1().tx.ext.sorobanData();
    sorobanData.resourceFee = resourceFee;
}

void
setMemo(TransactionTestFramePtr tx, Memo memo)
{
    auto& env = tx->getMutableEnvelope();
    Memo& m =
        env.type() == ENVELOPE_TYPE_TX_V0 ? env.v0().tx.memo : env.v1().tx.memo;
    m = memo;
}

void
setMinTime(TransactionTestFramePtr tx, TimePoint minTime)
{
    auto& env = tx->getMutableEnvelope();
    if (env.type() == ENVELOPE_TYPE_TX_V0)
    {
        env.v0().tx.timeBounds.activate().minTime = minTime;
    }
    else if (env.type() == ENVELOPE_TYPE_TX)
    {
        auto& cond = env.v1().tx.cond;
        switch (cond.type())
        {
        case PRECOND_NONE:
            cond.type(PRECOND_TIME);
        case PRECOND_TIME:
            cond.timeBounds().minTime = minTime;
            break;
        case PRECOND_V2:
            cond.v2().timeBounds.activate().minTime = minTime;
            break;
        }
    }
}

void
setMaxTime(std::shared_ptr<TransactionTestFrame const> tx, TimePoint maxTime)
{
    auto& env = tx->getMutableEnvelope();
    if (env.type() == ENVELOPE_TYPE_TX_V0)
    {
        env.v0().tx.timeBounds.activate().maxTime = maxTime;
    }
    else if (env.type() == ENVELOPE_TYPE_TX)
    {
        auto& cond = env.v1().tx.cond;
        switch (cond.type())
        {
        case PRECOND_NONE:
            cond.type(PRECOND_TIME);
        case PRECOND_TIME:
            cond.timeBounds().maxTime = maxTime;
            break;
        case PRECOND_V2:
            cond.v2().timeBounds.activate().maxTime = maxTime;
            break;
        }
    }
}
#endif
}
}
