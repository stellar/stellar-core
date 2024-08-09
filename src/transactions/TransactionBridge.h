// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "xdr/Stellar-transaction.h"
#include <memory>

namespace stellar
{
class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;

#ifdef BUILD_TESTS
class TransactionTestFrame;
typedef std::shared_ptr<TransactionTestFrame> TransactionTestFramePtr;
#endif

namespace txbridge
{

TransactionEnvelope convertForV13(TransactionEnvelope const& input);

xdr::xvector<DecoratedSignature, 20> const&
getSignatures(TransactionEnvelope const& env);
xdr::xvector<DecoratedSignature, 20>& getSignatures(TransactionEnvelope& env);
xdr::xvector<DecoratedSignature, 20>&
getSignaturesInner(TransactionEnvelope& env);
xdr::xvector<Operation, MAX_OPS_PER_TX>&
getOperations(TransactionEnvelope& env);

#ifdef BUILD_TESTS
xdr::xvector<DecoratedSignature, 20>& getSignatures(TransactionTestFramePtr tx);

void setSeqNum(TransactionTestFramePtr tx, int64_t seq);

void setFullFee(TransactionTestFramePtr tx, uint32_t fee);

// only works on Soroban tx
void setSorobanFees(TransactionTestFramePtr tx, uint32_t totalFee,
                    int64 resourceFee);

void setMemo(TransactionTestFramePtr tx, Memo memo);

void setMinTime(TransactionTestFramePtr tx, TimePoint minTime);

void setMaxTime(std::shared_ptr<TransactionTestFrame const> tx,
                TimePoint maxTime);
#endif
}
}
