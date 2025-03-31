#include "transactions/EventManager.h"
#include "crypto/KeyUtils.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"

namespace stellar
{

DiagnosticEventBuffer::DiagnosticEventBuffer(Config const& config)
    : mConfig(config)
{
}

void
DiagnosticEventBuffer::pushDiagnosticEvents(
    xdr::xvector<DiagnosticEvent> const& evts)
{
    mBuffer.insert(mBuffer.end(), evts.begin(), evts.end());
}

void
DiagnosticEventBuffer::pushSimpleDiagnosticError(SCErrorType ty,
                                                 SCErrorCode code,
                                                 std::string&& message,
                                                 xdr::xvector<SCVal>&& args)
{
    ContractEvent ce;
    ce.type = DIAGNOSTIC;
    ce.body.v(0);

    SCVal sym = makeSymbolSCVal("error"), err;
    err.type(SCV_ERROR);
    err.error().type(ty);
    err.error().code() = code;
    ce.body.v0().topics.assign({std::move(sym), std::move(err)});

    if (args.empty())
    {
        ce.body.v0().data.type(SCV_STRING);
        ce.body.v0().data.str().assign(std::move(message));
    }
    else
    {
        ce.body.v0().data.type(SCV_VEC);
        ce.body.v0().data.vec().activate();
        ce.body.v0().data.vec()->reserve(args.size() + 1);
        ce.body.v0().data.vec()->emplace_back(
            makeStringSCVal(std::move(message)));
        std::move(std::begin(args), std::end(args),
                  std::back_inserter(*ce.body.v0().data.vec()));
    }
    mBuffer.emplace_back(false, std::move(ce));
}

void
DiagnosticEventBuffer::pushApplyTimeDiagnosticError(SCErrorType ty,
                                                    SCErrorCode code,
                                                    std::string&& message,
                                                    xdr::xvector<SCVal>&& args)
{
    if (mConfig.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        pushSimpleDiagnosticError(ty, code, std::move(message),
                                  std::move(args));
    }
}

void
DiagnosticEventBuffer::flush(xdr::xvector<DiagnosticEvent>& buf)
{
    std::move(mBuffer.begin(), mBuffer.end(), std::back_inserter(buf));
    mBuffer.clear();
};

void
pushDiagnosticError(DiagnosticEventBuffer* ptr, SCErrorType ty,
                    SCErrorCode code, std::string&& message,
                    xdr::xvector<SCVal>&& args)
{
    if (ptr)
    {
        ptr->pushSimpleDiagnosticError(ty, code, std::move(message),
                                       std::move(args));
    }
}

void
pushValidationTimeDiagnosticError(DiagnosticEventBuffer* ptr, SCErrorType ty,
                                  SCErrorCode code, std::string&& message,
                                  xdr::xvector<SCVal>&& args)
{
    if (ptr && ptr->mConfig.ENABLE_DIAGNOSTICS_FOR_TX_SUBMISSION)
    {
        ptr->pushSimpleDiagnosticError(ty, code, std::move(message),
                                       std::move(args));
    }
}

OpEventManager::OpEventManager(TxEventManager& parentTxEventManager,
                               OperationFrame const& op)
    : mParent(parentTxEventManager), mOp(op)
{
}

DiagnosticEventBuffer&
OpEventManager::getDiagnosticEventsBuffer()
{
    return mParent.getDiagnosticEventsBuffer();
}

void
OpEventManager::pushContractEvents(xdr::xvector<ContractEvent> const& evts)
{
    auto& ces = mContractEvents;
    ces.insert(ces.end(), evts.begin(), evts.end());
}

void
OpEventManager::flushContractEvents(xdr::xvector<ContractEvent>& buf)
{
    std::move(mContractEvents.begin(), mContractEvents.end(),
              std::back_inserter(buf));
    mContractEvents.clear();
};

TxEventManager::TxEventManager(uint32_t protocolVersion, Hash const& networkID,
                               Config const& config,
                               TransactionFrameBase const& tx)
    : mProtocolVersion(protocolVersion)
    , mNetworkID(networkID)
    , mConfig(config)
    , mTx(tx)
    , mDiagnosticEvents(DiagnosticEventBuffer(config))
{
}

OpEventManager
TxEventManager::createNewOpEventManager(OperationFrame const& op)
{
    return OpEventManager(*this, op);
}

DiagnosticEventBuffer&
TxEventManager::getDiagnosticEventsBuffer()
{
    return mDiagnosticEvents;
}

void
TxEventManager::flushDiagnosticEvents(xdr::xvector<DiagnosticEvent>& buf)
{
    mDiagnosticEvents.flush(buf);
};

} // namespace stellar