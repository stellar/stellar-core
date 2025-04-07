#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class TransactionFrameBase;

std::optional<Asset> getAssetFromEvent(ContractEvent const& event,
                                       Hash const& networkID);

class DiagnosticEventBuffer
{
  public:
    static DiagnosticEventBuffer createForApply(bool metaEnabled,
                                                TransactionFrameBase const& tx,
                                                Config const& config);
    static DiagnosticEventBuffer createForValidation(Config const& config);
    static DiagnosticEventBuffer createDisabled();

    void pushEvent(DiagnosticEvent&& event);
    void pushError(SCErrorType ty, SCErrorCode code, std::string&& message,
                   xdr::xvector<SCVal>&& args = {});

    bool isEnabled() const;

    xdr::xvector<DiagnosticEvent> finalize();

  private:
    DiagnosticEventBuffer(bool enabled);

    xdr::xvector<DiagnosticEvent> mBuffer;
    bool mEnabled = false;
};

class OpEventManager
{
  public:
    void setEvents(xdr::xvector<ContractEvent>&& events);

 void
    eventsForClaimAtoms(MuxedAccount const& source,
                        xdr::xvector<stellar::ClaimAtom> const& claimAtoms);

    // This will check if the issuer is involved, and emit a mint/burn instead
    // of a transfer if so
    void eventForTransferWithIssuerCheck(Asset const& asset,
                                         SCAddress const& from,
                                         SCAddress const& to, int64 amount);

    // Adds a new "transfer" contractEvent in the form of:
    // contract: asset, topics: ["transfer", from:Address, to:Address,
    // sep0011_asset:String], data: { amount:i128 }
    void newTransferEvent(Asset const& asset, SCAddress const& from,
                          SCAddress const& to, int64 amount);

    // contract: asset, topics: ["mint", to:Address, sep0011_asset:String],
    // data: { amount:i128 }
    void newMintEvent(Asset const& asset, SCAddress const& to, int64 amount,
                      bool insertAtBeginning = false);

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

    xdr::xvector<ContractEvent> finalize();

  private:
    friend class OperationMetaBuilder;

    OpEventManager(bool metaEnabled, bool isSoroban, uint32_t protocolVersion,
                   Config const& config);

    bool mEnabled = false;
    xdr::xvector<ContractEvent> mContractEvents;
};

class TxEventManager
{
  public:
  private:
    friend class TransactionMetaBuilder;

    TxEventManager(bool metaEnabled, xdr::xvector<ContractEvent>& txEvents,
                   uint32_t protocolVersion, Hash const& networkID,
                   Config const& config, TransactionFrameBase const& tx);

    Hash const& mNetworkID;
    xdr::xvector<ContractEvent>& mTxEvents;
    bool mEnabled = false;
};

}
