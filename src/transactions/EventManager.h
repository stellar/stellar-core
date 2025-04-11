#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
class TransactionFrameBase;

// If the event was emitted by an SAC, return the asset. Otherwise, return
// nullopt.
std::optional<Asset> getAssetFromEvent(ContractEvent const& event,
                                       Hash const& networkID);

// Buffer of diagnostic events corresponding to a transaction.
// This stores the events, so it can be used in the contexts where the events
// are stored in different data structures (or not stored at all).
// The buffer may be disabled, thus making all the calls that add events
// no-ops. Whether it is enabled or not is determined by the context and the
// configuration flags.
class DiagnosticEventBuffer
{
  public:
    // Create the diagnostic event buffer for applying the transaction.
    // Since the diagnostic events are stored in the meta, this depends on
    // whether the meta is enabled.
    static DiagnosticEventBuffer createForApply(bool metaEnabled,
                                                TransactionFrameBase const& tx,
                                                Config const& config);
    // Create the diagnostic event buffer for validating the transaction.
    static DiagnosticEventBuffer createForValidation(Config const& config);
    // Create a disabled diagnostic event buffer.
    // Useful for validating  transactions without propagating the diagnostic
    // errors and for tests.
    static DiagnosticEventBuffer createDisabled();

    // Adds an event to the buffer.
    void pushEvent(DiagnosticEvent&& event);
    // Adds a simple error diagnostic event to the buffer.
    void pushError(SCErrorType ty, SCErrorCode code, std::string&& message,
                   xdr::xvector<SCVal>&& args = {});

    // Returns whether the buffer is enabled.
    bool isEnabled() const;

    // Moves the buffered events out from the buffer.
    xdr::xvector<DiagnosticEvent> finalize();

  private:
    DiagnosticEventBuffer(bool enabled);

    xdr::xvector<DiagnosticEvent> mBuffer;
    bool mEnabled = false;
};

// Event manager for operation events.
// This stores a contract event buffer corresponding to a single operation and
// provides functions that build and add the events to the buffer.
// This can't be instantiated directly and is only accessible from the
// `OperationMetaBuilder` corresponding to the operation.
// This can be disabled, thus making all the calls that add events no-ops.
class OpEventManager
{
  public:
    // Returns the events collected so far.
    xdr::xvector<ContractEvent> const& getEvents();

    // Returns whether the event manager is enabled.
    bool isEnabled() const;

    // Sets all the events corresponding to the operation.
    // Note, that this can be called just once per operation and is not
    // compatible with the event generators.
    void setEvents(xdr::xvector<ContractEvent>&& events);

    // Creates transfer events corresponding to provided claim atoms.
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

    // Moves the buffered events out from the event manager.
    xdr::xvector<ContractEvent> finalize();

  private:
    friend class OperationMetaBuilder;

    OpEventManager(bool metaEnabled, bool isSoroban, uint32_t protocolVersion,
                   Hash const& networkID, Memo const& mMemo,
                   Config const& config);

    Hash const& mNetworkID;
    Memo const& mMemo;
    bool mEnabled = false;
    bool mUpdateSACEventsToProtocol23Format = false;
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
