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

// Event manager for diagnostic events corresponding to a single transaction.
// This stores a buffer of diagnostic events and it can be used in the contexts
// where the events are stored in different data structures (or not stored at
// all).
// The event manager may be disabled, thus making all the calls that add
// events no-ops. Whether it is enabled or not is determined by the context and
// the configuration flags at creation time.
class DiagnosticEventManager
{
  public:
    // Create the diagnostic event manager for applying the transaction.
    // Since the diagnostic events are stored in the meta, this depends on
    // whether the meta is enabled.
    static DiagnosticEventManager createForApply(bool metaEnabled,
                                                 TransactionFrameBase const& tx,
                                                 Config const& config);
    // Create the diagnostic event manager for validating the transaction.
    static DiagnosticEventManager createForValidation(Config const& config);
    // Create a disabled diagnostic event manager.
    // Useful for validating  transactions without propagating the diagnostic
    // errors and for tests.
    static DiagnosticEventManager createDisabled();

    // Adds an event to the event buffer.
    void pushEvent(DiagnosticEvent&& event);
    // Creates and adds a simple error diagnostic event to the buffer.
    void pushError(SCErrorType ty, SCErrorCode code, std::string&& message,
                   xdr::xvector<SCVal>&& args = {});

    // Returns whether the event manager is enabled.
    bool isEnabled() const;

    // Moves the buffered events out from the event manager.
    xdr::xvector<DiagnosticEvent> finalize();

    // Log all the events in the buffer at DEBUG log level.
    void debugLogEvents() const;

  private:
    DiagnosticEventManager(bool enabled);

    xdr::xvector<DiagnosticEvent> mBuffer;
    bool mEnabled = false;
    bool mFinalized = false;
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
    void eventsForClaimAtoms(
        MuxedAccount const& source,
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
    ContractEvent makeMintEvent(Asset const& asset, SCAddress const& to,
                                int64 amount, bool allowMuxedIdOrMemo);

    // contract: asset, topics: ["burn", from:Address, sep0011_asset:String],
    // data: { amount:i128 }
    ContractEvent makeBurnEvent(Asset const& asset, SCAddress const& from,
                                int64 amount);

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
    bool mFinalized = false;
};

// Event manager for transaction-level events (currently only fee events).
// This stores a contract event buffer corresponding to a transaction and
// provides functions that build and add the events to the buffer.
// This can't be instantiated directly and is only accessible from the
// `TransactionMetaBuilder` corresponding to the transaction.
// This can be disabled, thus making all the calls that add events no-ops.
class TxEventManager
{
  public:
    // Add a new fee event for the transaction.
    // contract: native, topics: ["fee", feeSource: Address],
    // data: { amount:i128 }
    void newFeeEvent(AccountID const& feeSource, int64 amount,
                     TransactionEventStage stage);
    // Returns whether the event manager is enabled.
    bool isEnabled() const;
    // Moves the buffered events out from the event manager.
    xdr::xvector<TransactionEvent> finalize();

  private:
    friend class TransactionMetaBuilder;

    TxEventManager(bool metaEnabled, uint32_t protocolVersion,
                   Hash const& networkID, Config const& config);

    Hash const& mNetworkID;
    xdr::xvector<TransactionEvent> mTxEvents;
    bool mEnabled = false;
    bool mFinalized = false;
};

}
