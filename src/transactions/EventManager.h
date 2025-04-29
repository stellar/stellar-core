#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{
class TxEventManager;
class TransactionFrameBase;

std::optional<Asset> getAssetFromEvent(ContractEvent const& event,
                                       Hash const& networkID);

struct DiagnosticEventBuffer
{
    xdr::xvector<DiagnosticEvent> mBuffer;
    Config const& mConfig;

    DiagnosticEventBuffer(Config const& config);
    void pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts);
    void pushSimpleDiagnosticError(SCErrorType ty, SCErrorCode code,
                                   std::string&& message,
                                   xdr::xvector<SCVal>&& args);
    void pushApplyTimeDiagnosticError(SCErrorType ty, SCErrorCode code,
                                      std::string&& message,
                                      xdr::xvector<SCVal>&& args = {});
    void flush(xdr::xvector<DiagnosticEvent>& buf);
};

// helper functions for emitting diagnostic events in the validation workflows
void pushDiagnosticError(DiagnosticEventBuffer* ptr, SCErrorType ty,
                         SCErrorCode code, std::string&& message,
                         xdr::xvector<SCVal>&& args);
void pushValidationTimeDiagnosticError(DiagnosticEventBuffer* ptr,
                                       SCErrorType ty, SCErrorCode code,
                                       std::string&& message,
                                       xdr::xvector<SCVal>&& args = {});

class OpEventManager
{
  private:
    xdr::xvector<ContractEvent> mContractEvents;
    TxEventManager& mParent;
    Memo const mMemo;

  public:
    OpEventManager(TxEventManager& parentTxEventManager, Memo const& memo);

    DiagnosticEventBuffer& getDiagnosticEventsBuffer();

    void pushContractEvents(xdr::xvector<ContractEvent> const& evts);

    xdr::xvector<ContractEvent> const& getContractEvents();

    void flushContractEvents(xdr::xvector<ContractEvent>& buf);

    void
    eventsForClaimAtoms(MuxedAccount const& source,
                        xdr::xvector<stellar::ClaimAtom> const& claimAtoms);

    // This will check if the issuer is involved, and emit a mint/burn instead
    // of a transfer if so
    void eventForTransferWithIssuerCheck(Asset const& asset,
                                         SCAddress const& from,
                                         SCAddress const& to, int64 amount,
                                         bool allowMuxedIdOrMemo);

    // Adds a new "transfer" contractEvent in the form of:
    // contract: asset, topics: ["transfer", from:Address, to:Address,
    // sep0011_asset:String], data: { amount:i128 }
    void newTransferEvent(Asset const& asset, SCAddress const& from,
                          SCAddress const& to, int64 amount,
                          bool allowMuxedIdOrMemo);

    // contract: asset, topics: ["mint", to:Address, sep0011_asset:String],
    // data: { amount:i128 }
    void newMintEvent(Asset const& asset, SCAddress const& to, int64 amount,
                      bool allowMuxedIdOrMemo, bool insertAtBeginning = false);

    // contract: asset, topics: ["burn", from:Address, sep0011_asset:String],
    // data: { amount:i128 }
    void newBurnEvent(Asset const& asset, SCAddress const& from, int64 amount);

    // contract: asset, topics: ["clawback", from:Address,
    // sep0011_asset:String], data: { amount:i128 }
    void newClawbackEvent(Asset const& asset, SCAddress const& from,
                          int64 amount);

    // contract: asset, topics: ["set_authorized", id:Address,
    // sep0011_asset:String], data: { authorize:bool }
    void newSetAuthorizedEvent(Asset const& asset, AccountID const& id,
                               bool authorize);
};

class TxEventManager
{
  private:
    uint32_t mProtocolVersion;
    Hash const& mNetworkID;
    Config const& mConfig;
    xdr::xvector<ContractEvent> mTxEvents;
    DiagnosticEventBuffer mDiagnosticEvents;

  public:
    TxEventManager(uint32_t protocolVersion, Hash const& networkID,
                   Config const& config);

    OpEventManager createNewOpEventManager(Memo const& memo);

    DiagnosticEventBuffer& getDiagnosticEventsBuffer();

    void flushDiagnosticEvents(xdr::xvector<DiagnosticEvent>& buf);

    void flushTxEvents(xdr::xvector<ContractEvent>& buf);

    Hash const& getNetworkID() const;
    uint32_t getProtocolVersion() const;
    Config const& getConfig() const;

    bool shouldEmitClassicEvents() const;

    void newFeeEvent(AccountID const& feeSource, int64 amount);

#ifdef BUILD_TESTS
    xdr::xvector<DiagnosticEvent> const&
    getDiagnosticEvents() const
    {
        return mDiagnosticEvents.mBuffer;
    }
#endif
};

}
