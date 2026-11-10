// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderSCPDriver.h"
#include "HerderUtils.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/HerderImpl.h"
#include "herder/LeaderSchedule.h"
#include "herder/LedgerCloseData.h"
#include "herder/PendingEnvelopes.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include <Tracy.hpp>
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <xdrpp/marshal.h>

namespace stellar
{

uint32_t const TXSETVALID_CACHE_SIZE = 1000;

Hash
HerderSCPDriver::getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const
{
    SHA256 hasher;
    for (auto const& v : vals)
    {
        hasher.add(v);
    }
    return hasher.finish();
}

HerderSCPDriver::SCPMetrics::SCPMetrics(Application& app)
    : mEnvelopeSign(
          app.getMetrics().NewMeter({"scp", "envelope", "sign"}, "envelope"))
    , mValueValid(app.getMetrics().NewMeter({"scp", "value", "valid"}, "value"))
    , mValueInvalid(
          app.getMetrics().NewMeter({"scp", "value", "invalid"}, "value"))
    , mPrepareToExternalize(
          app.getMetrics().NewTimer({"scp", "timing", "externalized"}))
    , mTriggerToBallot(
          app.getMetrics().NewTimer({"scp", "timing", "trigger-to-ballot"}))
    , mFirstToSelfExternalizeLag(app.getMetrics().NewTimer(
          {"scp", "timing", "first-to-self-externalize-lag"}))
    , mSelfToOthersExternalizeLag(app.getMetrics().NewTimer(
          {"scp", "timing", "self-to-others-externalize-lag"}))
    , mBallotBlockedOnTxSet(app.getMetrics().NewTimer(
          {"scp", "timing", "ballot-blocked-on-txset"}))
    , mTxSetValidation(
          app.getMetrics().NewTimer({"herder", "txset", "validate"}))
    , mEmptyTxSetExternalized(
          app.getMetrics().NewCounter({"scp", "empty-tx-set", "externalized"}))
    , mEmptyTxSetValueReplaced(app.getMetrics().NewCounter(
          {"scp", "empty-tx-set", "value-replaced"}))
{
}

HerderSCPDriver::HerderSCPDriver(Application& app, HerderImpl& herder,
                                 Upgrades const& upgrades,
                                 PendingEnvelopes& pendingEnvelopes)
    : mApp{app}
    , mHerder{herder}
    , mLedgerManager{mApp.getLedgerManager()}
    , mUpgrades{upgrades}
    , mPendingEnvelopes{pendingEnvelopes}
    , mSCP{*this, mApp.getConfig().NODE_SEED.getPublicKey(),
           mApp.getConfig().NODE_IS_VALIDATOR, mApp.getConfig().QUORUM_SET}
    , mSCPMetrics{mApp}
    , mLeaderTimeout{mApp.getMetrics().NewHistogram(
          {"scp", "timeout", "leader"})}
    , mLeaderRound{mApp.getMetrics().NewHistogram({"scp", "leader", "round"})}
    , mPrepareTimeout{mApp.getMetrics().NewHistogram(
          {"scp", "timeout", "prepare"})}
    , mUniqueValues{mApp.getMetrics().NewHistogram(
          {"scp", "slot", "values-referenced"})}
    , mLedgerSeqProposing(0)
    , mTxSetValidCache(TXSETVALID_CACHE_SIZE)
{
}

HerderSCPDriver::~HerderSCPDriver()
{
}

void
HerderSCPDriver::stateChanged()
{
    mApp.syncOwnMetrics();
}

void
HerderSCPDriver::bootstrap()
{
    stateChanged();
    clearSCPExecutionEvents();
}

// envelope handling

class SCPHerderEnvelopeWrapper : public SCPEnvelopeWrapper
{
    HerderImpl& mHerder;

    SCPQuorumSetPtr mQSet;
    std::vector<TxSetXDRFrameConstPtr> mTxSets;

  public:
    // Wrap an SCP envelope `e`, using `herder` to fetch the quorum set. This
    // function inserts hashes corresponding to missing transaction sets into
    // the output parameter `missingTxSets`.
    explicit SCPHerderEnvelopeWrapper(SCPEnvelope const& e, HerderImpl& herder,
                                      std::set<Hash>& missingTxSets)
        : SCPEnvelopeWrapper(e), mHerder(herder)
    {
        releaseAssert(missingTxSets.empty());

        // attach everything we can to the wrapper
        auto qSetH = Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        mQSet = mHerder.getQSet(qSetH);
        if (!mQSet)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("SCPHerderEnvelopeWrapper: Wrapping an unknown "
                           "qset {} from envelope"),
                hexAbbrev(qSetH)));
        }
        auto txSets = getValidatedTxSetHashes(e);
        for (auto const& txSetH : txSets)
        {
            auto result = mHerder.getTxSet(txSetH);
            if (auto* txSet = std::get_if<TxSetXDRFrameConstPtr>(&result))
            {
                if (*txSet)
                {
                    mTxSets.emplace_back(*txSet);
                }
                else
                {
                    missingTxSets.insert(txSetH);
                }
            }
            // EmptyTxSet: not missing, nothing to store
        }
    }

    void
    addTxSet(TxSetXDRFrameConstPtr txSet) override
    {
        mTxSets.emplace_back(txSet);
    }
};

SCPEnvelopeWrapperPtr
HerderSCPDriver::wrapEnvelope(SCPEnvelope const& envelope)
{
    std::set<Hash> missingTxSets;
    auto r = std::make_shared<SCPHerderEnvelopeWrapper>(envelope, mHerder,
                                                        missingTxSets);

    // Register this wrapper for any tx sets that weren't available
    // so we can update it later when the tx set arrives
    for (auto const& h : missingTxSets)
    {
        mPendingTxSetEnvelopeWrappers[h].push_back(r);
    }

    return r;
}

void
HerderSCPDriver::signEnvelope(SCPEnvelope& envelope)
{
    ZoneScoped;
    mSCPMetrics.mEnvelopeSign.Mark();
    mHerder.signEnvelope(mApp.getConfig().NODE_SEED, envelope);
}

void
HerderSCPDriver::emitEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    mHerder.emitEnvelope(envelope);
}

bool
HerderSCPDriver::isEnvelopeReady(SCPEnvelope const& env) const
{
    if (!mPendingEnvelopes.isQsetFetched(env))
    {
        // QSet must be available
        return false;
    }

    if (mPendingEnvelopes.areTxSetsFetched(env))
    {
        // Have all tx sets and the qset. This envelope is ready to be processed
        return true;
    }

    if (!isParallelTxSetDownloadEnabled())
    {
        // Parallel downloading is disabled, so we need all tx sets
        return false;
    }

    // Beyond this point all checks relate to whether SCP can process `env`
    // in parallel with downloading the missing tx sets it references.

    auto const type = env.statement.pledges.type();
    if (type != SCP_ST_NOMINATE && type != SCP_ST_PREPARE)
    {
        // Parallel tx set downloading is only allowed for nomination and
        // prepare messages.
        return false;
    }

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (env.statement.slotIndex != lcl.header.ledgerSeq + 1)
    {
        // Parallel tx set downloading is only enabled for LCL+1
        return false;
    }

    // Parallel downloading is only enabled when tracking and in sync
    return mHerder.isTracking() &&
           mApp.getState() == Application::State::APP_SYNCED_STATE;
}

bool
HerderSCPDriver::protocolAllowsEmptyTxSetValues() const
{
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    return protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                     EMPTY_TX_SET_PROTOCOL_VERSION);
}

bool
HerderSCPDriver::protocolUsesMsCloseTime() const
{
    return protocolHasMsCloseTime(
        mLedgerManager.getLastClosedLedgerHeader().header.ledgerVersion);
}

bool
HerderSCPDriver::isParallelTxSetDownloadEnabled() const
{
    return mApp.getConfig().EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD &&
           protocolAllowsEmptyTxSetValues();
}

// value validation

bool
HerderSCPDriver::checkCloseTime(uint64_t slotIndex, ConsensusTime lastCloseTime,
                                StellarValue const& b) const
{
    auto const closeTime = getConsensusTime(b);

    // Check closeTime (not too old)
    if (closeTime <= lastCloseTime)
    {
        CLOG_TRACE(Herder, "Close time too old for slot {}, got {} vs {}",
                   slotIndex, closeTime.toString(), lastCloseTime.toString());
        return false;
    }

    // Check closeTime (not too far in future)
    auto const protocolVersion =
        mLedgerManager.getLastClosedLedgerHeader().header.ledgerVersion;
    auto const maxCloseTime = ConsensusTime::fromSystemTime(
        mApp.getClock().system_now() + Herder::MAX_TIME_SLIP_SECONDS,
        protocolVersion);
    if (closeTime > maxCloseTime)
    {
        CLOG_TRACE(Herder,
                   "Close time too far in future for slot {}, got {} vs {}",
                   slotIndex, closeTime.toString(), maxCloseTime.toString());
        return false;
    }
    return true;
}

SCPDriver::ValidationLevel
HerderSCPDriver::validatePastOrFutureValue(
    uint64_t slotIndex, StellarValue const& b,
    LedgerHeaderHistoryEntry const& lcl) const
{
    ZoneScoped;
    releaseAssert(slotIndex != lcl.header.ledgerSeq + 1);
    auto const closeTime = getConsensusTime(b);
    auto const lclCloseTime = getConsensusTime(lcl.header.scpValue);
    if (slotIndex == lcl.header.ledgerSeq)
    {
        // previous ledger
        if (closeTime != lclCloseTime)
        {
            CLOG_TRACE(
                Herder, "Got a bad close time for ledger {}, got {} vs {}",
                slotIndex, closeTime.toString(), lclCloseTime.toString());
            return SCPDriver::kInvalidValue;
        }
        if (isEmptyTxSetStellarValue(b))
        {
            if (!protocolAllowsEmptyTxSetValues())
            {
                return SCPDriver::kInvalidValue;
            }

            // We can check previousLedgerHash because the LCL header
            // contains the hash of its parent. We cannot check
            // previousLedgerVersion because the LCL header only has
            // its own version, and a protocol upgrade on the LCL
            // could make it differ from its parent's version.
            if (getProposedPreviousLedgerHash(b) !=
                lcl.header.previousLedgerHash)
            {
                CLOG_TRACE(Herder,
                           "Got a bad previousLedgerHash for empty-tx-set "
                           "value in ledger {}",
                           slotIndex);
                return SCPDriver::kInvalidValue;
            }
        }
    }
    else if (slotIndex < lcl.header.ledgerSeq)
    {
        // basic sanity check on older value
        if (closeTime >= lclCloseTime)
        {
            CLOG_TRACE(
                Herder, "Got a bad close time for ledger {}, got {} vs {}",
                slotIndex, closeTime.toString(), lclCloseTime.toString());
            return SCPDriver::kInvalidValue;
        }
    }
    else if (!checkCloseTime(slotIndex, lclCloseTime, b))
    {
        // future messages must be valid compared to lastCloseTime
        return SCPDriver::kInvalidValue;
    }

    if (!mHerder.isTracking())
    {
        // if we're not tracking, there is not much more we can do to
        // validate
        CLOG_TRACE(Herder, "MaybeValidValue (not tracking) for slot {}",
                   slotIndex);
        return SCPDriver::kMaybeValidNotCurrentValue;
    }

    // Check slotIndex.
    if (mHerder.nextConsensusLedgerIndex() > slotIndex)
    {
        // we already moved on from this slot
        // still send it through for emitting the final messages
        CLOG_TRACE(Herder,
                   "MaybeValidValue (already moved on) for slot {}, at {}",
                   slotIndex, mHerder.nextConsensusLedgerIndex());
        return SCPDriver::kMaybeValidNotCurrentValue;
    }
    if (mHerder.nextConsensusLedgerIndex() < slotIndex)
    {
        // this is probably a bug as "tracking" means we're processing
        // messages only for smaller slots
        CLOG_ERROR(Herder,
                   "HerderSCPDriver::validateValue i: {} processing a future "
                   "message while tracking {} ",
                   slotIndex, mHerder.trackingConsensusLedgerIndex());
        return SCPDriver::kInvalidValue;
    }

    // when tracking, we use the tracked time for last close time
    auto lastCloseTime = mHerder.trackingConsensusCloseTime();
    if (!checkCloseTime(slotIndex, lastCloseTime, b))
    {
        return SCPDriver::kInvalidValue;
    }

    // this is as far as we can go if we don't have the state
    CLOG_TRACE(Herder, "Can't validate locally, value may be valid for slot {}",
               slotIndex);
    return SCPDriver::kMaybeValidNotCurrentValue;
}

SCPDriver::ValidationLevel
HerderSCPDriver::validateValueAgainstLocalState(uint64_t slotIndex,
                                                StellarValue const& b,
                                                bool proposal) const
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

    // We can only fully validate values for LCL+1
    // For past and future slots, perform partial validity checks, specifically
    // validate close time and network tracking ledger sequence.
    bool isCurrentLedger = slotIndex == lcl.header.ledgerSeq + 1;

    SCPDriver::ValidationLevel res;
    if (isCurrentLedger)
    {
        // The value is for LCL+1, perform all possible checks
        if (!checkCloseTime(slotIndex, getConsensusTime(lcl.header.scpValue),
                            b))
        {
            return SCPDriver::kInvalidValue;
        }

        // For empty-tx-set values, validate that the previous ledger context
        // matches our LCL. Empty-tx-set values don't have a real tx set to
        // validate.
        if (isEmptyTxSetStellarValue(b))
        {
            if (!protocolAllowsEmptyTxSetValues())
            {
                return SCPDriver::kInvalidValue;
            }

            if (proposal)
            {
                // Empty-tx-set values only come out of balloting (a node
                // gives up on a download): nobody proposes one.
                CLOG_DEBUG(Herder,
                           "HerderSCPDriver::validateValue i: {} rejecting "
                           "empty-tx-set value as a proposal",
                           slotIndex);
                return SCPDriver::kInvalidValue;
            }
            if (getProposedPreviousLedgerHash(b) != lcl.hash ||
                getProposedPreviousLedgerVersion(b) != lcl.header.ledgerVersion)
            {
                CLOG_DEBUG(Herder,
                           "HerderSCPDriver::validateValue i: {} empty-tx-set "
                           "value has mismatched previous ledger context",
                           slotIndex);
                return SCPDriver::kInvalidValue;
            }
            return SCPDriver::kFullyValidatedValue;
        }

        Hash const& txSetHash = b.txSetHash;
        // Empty-tx-set values return early above, so this only runs for
        // non-empty-tx-set hashes. Extract the TxSetXDRFrameConstPtr.
        TxSetXDRFrameConstPtr txSet = std::get<TxSetXDRFrameConstPtr>(
            mPendingEnvelopes.getTxSet(txSetHash));

        auto closeTimeOffset =
            getApplyTime(b) - getApplyTime(lcl.header.scpValue);

        if (!txSet)
        {
            // Parallel tx set downloading must be enabled to get here. This
            // check has a carve-out for slots restored from the database
            // because the setting may have previously been enabled on those
            // slots.
            releaseAssert(isParallelTxSetDownloadEnabled() ||
                          mRestoredSlotIndices.count(slotIndex));
            if (protocolAllowsEmptyTxSetValues())
            {
                res = SCPDriver::kStructurallyValidValue;
            }
            else
            {
                CLOG_ERROR(Herder, "validateValue i:{} unknown txSet {}",
                           slotIndex, hexAbbrev(txSetHash));

                res = SCPDriver::kInvalidValue;
            }
        }
        else if (!checkAndCacheTxSetValid(*txSet, lcl, closeTimeOffset))
        {
            CLOG_DEBUG(Herder,
                       "HerderSCPDriver::validateValue i: {} invalid txSet {}",
                       slotIndex, hexAbbrev(txSetHash));
            res = protocolAllowsEmptyTxSetValues()
                      ? SCPDriver::kStructurallyValidValue
                      : SCPDriver::kInvalidValue;
        }
        else
        {
            CLOG_DEBUG(Herder,
                       "HerderSCPDriver::validateValue i: {} valid txSet {}",
                       slotIndex, hexAbbrev(txSetHash));
            res = SCPDriver::kFullyValidatedValue;
        }

        // kMaybeValidNotCurrentValue should never be returned for LCL+1 values,
        // as these values should always be fully valid/invalid, or awaiting
        // download
        releaseAssert(res != SCPDriver::kMaybeValidNotCurrentValue);
    }
    else
    {
        res = validatePastOrFutureValue(slotIndex, b, lcl);

        // Non-LCL+1 values cannot be fully validated and are not eligible for
        // parallel downloading.
        releaseAssert(res != SCPDriver::kStructurallyValidValue &&
                      res != SCPDriver::kFullyValidatedValue);
    }
    return res;
}

bool
HerderSCPDriver::deserializeAndValidateStellarValue(uint64_t slotIndex,
                                                    Value const& value,
                                                    StellarValue& sv) const
{
    ZoneScoped;
    try
    {
        ZoneNamedN(xdrZone, "XDR deserialize", true);
        xdr::xdr_from_opaque(value, sv);
    }
    catch (...)
    {
        return false;
    }

    // An ms value's closeTime must agree with its closeTimeMs
    if (!hasValidCloseTime(sv))
    {
        return false;
    }

    // Whether a slot uses whole-second or ms close times is decided by the
    // protocol of the ledger before it (CAP-0088). We only know that protocol
    // for certain for the next slot, LCL+1; for any other slot the network may
    // have upgraded somewhere between our LCL and that slot, so we accept
    // every format we cannot rule out:
    //
    //                  LCL before ms upgrade       LCL at/after ms upgrade
    //   slot <= LCL    whole-second only           either (may predate it)
    //   slot == LCL+1  whole-second only           ms only
    //   slot >  LCL+1  either (may have upgraded)  ms only
    //
    // Prior to the upgrade, if we are behind, it's possible our peers our
    // sending us valid future slots after an upgrade we have not yet applied.
    // Similarly for past slots, it's possible these predate the upgrade that we
    // have already applied.
    auto const& lclHeader = mLedgerManager.getLastClosedLedgerHeader().header;
    bool const lclProtocolHasMsCloseTime =
        protocolHasMsCloseTime(lclHeader.ledgerVersion);
    bool const slotAlreadyClosed = slotIndex <= lclHeader.ledgerSeq;
    bool const slotBeyondNext = slotIndex > lclHeader.ledgerSeq + 1;
    bool const valueHasMsCloseTime = isMsCloseTimeStellarValue(sv);

    bool const msCloseTimeAllowed = lclProtocolHasMsCloseTime || slotBeyondNext;
    bool const wholeSecondCloseTimeAllowed =
        !lclProtocolHasMsCloseTime || slotAlreadyClosed;
    bool const valueFormatAllowed =
        valueHasMsCloseTime ? msCloseTimeAllowed : wholeSecondCloseTimeAllowed;
    if (!valueFormatAllowed)
    {
        return false;
    }

    // Values must be signed or empty-tx-set values
    bool const isSigned = isSignedStellarValue(sv);
    bool const isEmpty = isEmptyTxSetStellarValue(sv);
    if (!isSigned && !isEmpty)
    {
        return false;
    }
    // Empty-tx-set values are only valid once the protocol allows them
    if (isEmpty && !protocolAllowsEmptyTxSetValues())
    {
        return false;
    }

    // Empty-tx-set values must have the empty-tx-set hash, and
    // non-explicitly-empty-tx-set values must not have the empty-tx-set hash.
    if ((sv.txSetHash == Herder::EMPTY_TX_SET_HASH) !=
        isEmptyTxSetStellarValue(sv))
    {
        return false;
    }

    {
        ZoneNamedN(sigZone, "signature check", true);
        if (!mHerder.verifyStellarValueSignature(sv))
        {
            return false;
        }
    }

    return true;
}

void
HerderSCPDriver::extractValidUpgrades(StellarValue& sv, bool proposal) const
{
    LedgerUpgradeType lastUpgradeType = LEDGER_UPGRADE_VERSION;
    LedgerUpgradeType thisUpgradeType;
    bool first = true;
    for (auto it = sv.upgrades.begin(); it != sv.upgrades.end();)
    {
        if (!mUpgrades.isValid(*it, thisUpgradeType, proposal, mApp))
        {
            it = sv.upgrades.erase(it);
        }
        else if (!first && lastUpgradeType >= thisUpgradeType)
        {
            it = sv.upgrades.erase(it);
        }
        else
        {
            lastUpgradeType = thisUpgradeType;
            first = false;
            it++;
        }
    }
}

SCPDriver::ValidationLevel
HerderSCPDriver::validateValue(uint64_t slotIndex, Value const& value,
                               bool proposal) const
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    StellarValue b;
    if (!deserializeAndValidateStellarValue(slotIndex, value, b))
    {
        mSCPMetrics.mValueInvalid.Mark();
        return SCPDriver::kInvalidValue;
    }

    SCPDriver::ValidationLevel res =
        validateValueAgainstLocalState(slotIndex, b, proposal);
    if (res != SCPDriver::kInvalidValue)
    {
        auto origSize = b.upgrades.size();
        extractValidUpgrades(b, proposal);
        if (b.upgrades.size() != origSize)
        {
            CLOG_TRACE(Herder,
                       "HerderSCPDriver::validateValue i: {} rejected due to "
                       "invalid or misordered upgrade steps",
                       slotIndex);
            res = SCPDriver::kInvalidValue;
        }
    }

    if (res)
    {
        mSCPMetrics.mValueValid.Mark();
    }
    else
    {
        mSCPMetrics.mValueInvalid.Mark();
    }
    return res;
}

// value marshaling

std::string
HerderSCPDriver::toShortString(NodeID const& pk) const
{
    return mApp.getConfig().toShortString(pk);
}

std::string
HerderSCPDriver::getValueString(Value const& v) const
{
    StellarValue b;
    if (v.empty())
    {
        return "[:empty:]";
    }

    try
    {
        xdr::xdr_from_opaque(v, b);

        return stellarValueToString(mApp.getConfig(), b);
    }
    catch (...)
    {
        return "[:invalid:]";
    }
}

Value
HerderSCPDriver::makeEmptyTxSetValueFromValue(Value const& v) const
{
    ZoneScoped;
    StellarValue proposedValue = toStellarValueOrThrow(v);
    releaseAssert(isSignedStellarValue(proposedValue));
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

    StellarValue sv;
    sv.txSetHash = Herder::EMPTY_TX_SET_HASH;
    sv.closeTime = proposedValue.closeTime;
    sv.upgrades = proposedValue.upgrades;
    if (proposedValue.ext.v() == STELLAR_VALUE_SIGNED_MS)
    {
        sv.ext.v(STELLAR_VALUE_EMPTY_TX_SET_MS);
        auto& ov = sv.ext.proposedMsValue();
        ov.closeTimeMs = proposedValue.ext.signedMsValue().closeTimeMs;
        ov.txSetHash = proposedValue.txSetHash;
        ov.previousLedgerHash = lcl.hash;
        ov.previousLedgerVersion = lcl.header.ledgerVersion;
        ov.lcValueSignature =
            proposedValue.ext.signedMsValue().lcValueSignature;
        return xdr::xdr_to_opaque(sv);
    }
    sv.ext.v(STELLAR_VALUE_EMPTY_TX_SET);
    sv.ext.proposedValue().txSetHash = proposedValue.txSetHash;
    sv.ext.proposedValue().previousLedgerHash = lcl.hash;
    sv.ext.proposedValue().previousLedgerVersion = lcl.header.ledgerVersion;
    sv.ext.proposedValue().lcValueSignature =
        proposedValue.ext.lcValueSignature();
    return xdr::xdr_to_opaque(sv);
}

bool
HerderSCPDriver::isEmptyTxSetValue(Value const& v) const
{
    ZoneScoped;
    StellarValue sv;
    bool success = toStellarValue(v, sv);
    if (!success)
    {
        return false;
    }

    return isEmptyTxSetStellarValue(sv);
}

// timer handling
void
HerderSCPDriver::timerCallbackWrapper(uint64_t slotIndex, int timerID,
                                      std::function<void()> cb)
{
    // reschedule timers for future slots when tracking
    if (mHerder.isTracking() && mHerder.nextConsensusLedgerIndex() != slotIndex)
    {
        CLOG_WARNING(
            Herder, "Herder rescheduled timer {} for slot {} with next slot {}",
            timerID, slotIndex, mHerder.nextConsensusLedgerIndex());
        setupTimer(slotIndex, timerID, std::chrono::seconds(1),
                   std::bind(&HerderSCPDriver::timerCallbackWrapper, this,
                             slotIndex, timerID, cb));
    }
    else
    {
        auto SCPTimingIt = mSCPExecutionTimes.find(slotIndex);
        if (SCPTimingIt != mSCPExecutionTimes.end())
        {
            auto& SCPTiming = SCPTimingIt->second;
            if (timerID == Slot::BALLOT_PROTOCOL_TIMER)
            {
                // Timeout happened in between first prepare and externalize
                ++SCPTiming.mPrepareTimeoutCount;
            }
            else
            {
                // Timed out waiting for the round leader's proposal
                ++SCPTiming.mLeaderTimeoutCount;
            }
        }

        cb();
    }
}

void
HerderSCPDriver::setupTimer(uint64_t slotIndex, int timerID,
                            std::chrono::milliseconds timeout,
                            std::function<void()> cb)
{
    // don't setup timers for old slots
    if (slotIndex <= mApp.getHerder().trackingConsensusLedgerIndex())
    {
        mSCPTimers.erase(slotIndex);
        return;
    }

    auto& slotTimers = mSCPTimers[slotIndex];

    auto it = slotTimers.find(timerID);
    if (it == slotTimers.end())
    {
        it = slotTimers.emplace(timerID, std::make_unique<VirtualTimer>(mApp))
                 .first;
    }
    auto& timer = *it->second;
    timer.cancel();
    if (cb)
    {
        timer.expires_from_now(timeout);
        timer.async_wait(std::bind(&HerderSCPDriver::timerCallbackWrapper, this,
                                   slotIndex, timerID, cb),
                         &VirtualTimer::onFailureNoop);
    }
}

void
HerderSCPDriver::stopTimer(uint64 slotIndex, int timerID)
{

    auto timersIt = mSCPTimers.find(slotIndex);
    if (timersIt == mSCPTimers.end())
    {
        return;
    }

    auto& slotTimers = timersIt->second;
    auto it = slotTimers.find(timerID);
    if (it != slotTimers.end())
    {
        auto& timer = *it->second;
        timer.cancel();
    }
}

static uint32_t const MAX_TIMEOUT_MS = (30 * 60) * 1000;

std::chrono::milliseconds
HerderSCPDriver::computeTimeout(uint32 roundNumber)
{
    releaseAssertOrThrow(roundNumber > 0);

    // Before p23, straight linear timeout
    // starting at 1 second and capping at MAX_TIMEOUT_MS
    uint32_t initialTimeoutMS = 1000;
    uint32_t incrementMS = 1000;

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  ProtocolVersion::V_23))
    {
        auto const& networkConfig =
            mLedgerManager.getLastClosedSorobanNetworkConfig();
        initialTimeoutMS = networkConfig.ballotTimeoutInitialMilliseconds();
        incrementMS = networkConfig.ballotTimeoutIncrementMilliseconds();
    }

    auto timeoutMS = initialTimeoutMS + (roundNumber - 1) * incrementMS;
    if (timeoutMS > MAX_TIMEOUT_MS)
    {
        timeoutMS = MAX_TIMEOUT_MS;
    }
    return std::chrono::milliseconds(timeoutMS);
}

std::optional<std::chrono::milliseconds>
HerderSCPDriver::getTxSetDownloadWaitTime(Value const& v) const
{
    StellarValue sv = toStellarValueOrThrow(v);
    return mPendingEnvelopes.getTxSetWaitingTime(sv.txSetHash);
}

std::chrono::milliseconds
HerderSCPDriver::getTxSetDownloadTimeout() const
{
    return mApp.getConfig().TX_SET_DOWNLOAD_TIMEOUT;
}

void
HerderSCPDriver::valueExternalized(uint64_t slotIndex, Value const& value)
{
    ZoneScoped;
    auto it = mSCPTimers.begin(); // cancel all timers below this slot
    while (it != mSCPTimers.end() && it->first <= slotIndex)
    {
        it = mSCPTimers.erase(it);
    }

    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        // This may not be possible as all messages are validated and should
        // therefore contain a valid StellarValue.
        CLOG_ERROR(Herder, "HerderSCPDriver::valueExternalized "
                           "Externalized StellarValue malformed");
        CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
        // no point in continuing as 'b' contains garbage at this point
        abort();
    }

    // externalize may trigger on older slots:
    //  * when the current instance starts up
    //  * when getting back in sync (a gap potentially opened)
    // in both cases do limited processing on older slots; more importantly,
    // deliver externalize events to LedgerManager
    bool isLatestSlot =
        slotIndex > mApp.getHerder().trackingConsensusLedgerIndex();

    if (isEmptyTxSetStellarValue(b))
    {
        mSCPMetrics.mEmptyTxSetExternalized.inc();
    }

    // Only update tracking state when newer slot comes in
    if (isLatestSlot)
    {
        // log information from older ledger to increase the chances that
        // all messages made it
        if (slotIndex > 2)
        {
            logQuorumInformationAndUpdateMetrics(slotIndex - 2);
        }

        // this may or may not be the ledger the local node started: either
        // way its own proposal is obsolete (the ledger is closing, or catchup
        // is about to trigger)
        mCurrentValue.reset();
        mLedgerSeqProposing = 0;

        if (!mHerder.isTracking())
        {
            stateChanged();
        }

        mHerder.setTrackingSCPState(slotIndex, b, /* isTrackingNetwork */ true);

        // record lag
        recordSCPExternalizeEvent(slotIndex, mSCP.getLocalNodeID(), false);

        recordSCPExecutionMetrics(slotIndex);

        mHerder.valueExternalized(slotIndex, b, isLatestSlot);

        // update externalize time so that we don't include the time spent in
        // `mHerder.valueExternalized`
        recordSCPExternalizeEvent(slotIndex, mSCP.getLocalNodeID(), true);
    }
    else
    {
        mHerder.valueExternalized(slotIndex, b, isLatestSlot);
    }
}

void
HerderSCPDriver::noteEmptyTxSetValueReplaced(uint64_t)
{
    ZoneScoped;
    mSCPMetrics.mEmptyTxSetValueReplaced.inc();
}

void
HerderSCPDriver::logQuorumInformationAndUpdateMetrics(uint64_t index)
{
    std::string res;
    auto v = mApp.getHerder().getJsonQuorumInfo(mSCP.getLocalNodeID(), true,
                                                false, index);
    auto qset = v.get("qset", "");
    if (!qset.empty())
    {
        Json::FastWriter fw;
        CLOG_INFO(Herder, "Quorum information for {} : {}", index,
                  fw.write(qset));
    }

    std::unordered_set<Hash> referencedValues;
    auto collectReferencedHashes = [&](SCPEnvelope const& envelope) {
        for (auto const& hash : getValidatedTxSetHashes(envelope))
        {
            referencedValues.insert(hash);
        }
        return true;
    };

    getSCP().processCurrentState(index, collectReferencedHashes,
                                 /* forceSelf */ true);
    if (!referencedValues.empty())
    {
        mUniqueValues.Update(referencedValues.size());
    }

    // Set mMissingNodes to the intersection of itself and the set of any
    // nodes missing in the latest slots.
    std::set<NodeID> missing =
        getSCP().getMissingNodes(getSCP().getLocalNodeID(), index);
    std::set<NodeID> prevMissing = std::move(mMissingNodes);
    mMissingNodes.clear();
    std::set_intersection(missing.begin(), missing.end(), prevMissing.begin(),
                          prevMissing.end(),
                          std::inserter(mMissingNodes, mMissingNodes.begin()));
}

void
HerderSCPDriver::startSlot(uint64_t slotIndex, ProposalValueSupplier makeValue,
                           bool constructValueNow)
{
    ZoneScoped;
    if (mLedgerSeqProposing != slotIndex)
    {
        mCurrentValue.reset();
    }
    mLedgerSeqProposing = static_cast<uint32_t>(slotIndex);
    if (constructValueNow && !mCurrentValue)
    {
        auto value = makeValue();
        // Publishing the tx set may externalize this slot. Do not restart it.
        if (mLedgerSeqProposing != slotIndex)
        {
            return;
        }
        // A null value is retried by the supplier below if this node becomes
        // a leader.
        mCurrentValue = std::move(value);
    }
    mSCP.startSlot(slotIndex,
                   [this, slotIndex,
                    makeValue = std::move(makeValue)]() -> ValueWrapperPtr {
                       if (mLedgerSeqProposing != slotIndex)
                       {
                           return nullptr;
                       }
                       if (!mCurrentValue)
                       {
                           auto value = makeValue();
                           // Publishing the tx set may externalize a pending
                           // value.
                           if (mLedgerSeqProposing != slotIndex)
                           {
                               return nullptr;
                           }
                           mCurrentValue = std::move(value);
                       }
                       return mCurrentValue;
                   });
}

std::vector<NodeID> const&
HerderSCPDriver::getLeaderSchedule(uint64_t slotIndex)
{
    static std::vector<NodeID> const empty;
    releaseAssert(threadIsMain());
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (slotIndex != static_cast<uint64_t>(lcl.header.ledgerSeq) + 1)
    {
        // The seed is the hash of the previous ledger's header: unknown for
        // any slot but the next one.
        return empty;
    }
    // Force a pending rebuild of the transitive quorum, if any
    mPendingEnvelopes.isNodeDefinitelyInQuorum(mSCP.getLocalNodeID());
    auto const& quorum = mPendingEnvelopes.getCurrentlyTrackedQuorum();
    auto& cache = mLeaderSchedule;
    if (cache.mSlotIndex != slotIndex || cache.mSeed != lcl.hash ||
        cache.mQuorumSize != quorum.size() ||
        cache.mQuorumRebuildCount != mPendingEnvelopes.getQuorumRebuildCount())
    {
        std::vector<NodeID> nodes;
        nodes.reserve(quorum.size());
        for (auto const& n : quorum)
        {
            nodes.emplace_back(n.first);
        }
        cache.mSchedule = computeLeaderSchedule(lcl.hash, std::move(nodes));
        cache.mSlotIndex = slotIndex;
        cache.mSeed = lcl.hash;
        cache.mQuorumSize = quorum.size();
        cache.mQuorumRebuildCount = mPendingEnvelopes.getQuorumRebuildCount();
    }
    return cache.mSchedule;
}

std::optional<NodeID>
HerderSCPDriver::getRoundLeader(uint64 slotIndex, uint32 round)
{
    auto const& schedule = getLeaderSchedule(slotIndex);
    if (schedule.empty() || round == 0)
    {
        return std::nullopt;
    }
    return schedule[(round - 1) % schedule.size()];
}

bool
HerderSCPDriver::isProposalPending(uint64 slotIndex, NodeID const& leader) const
{
    return mPendingEnvelopes.hasPendingEnvelopeFrom(slotIndex, leader);
}

bool
HerderSCPDriver::isLocalNodeRoundLeader(uint64_t slotIndex, uint32_t round)
{
    auto leader = getRoundLeader(slotIndex, round);
    return leader && *leader == mSCP.getLocalNodeID();
}

SCPQuorumSetPtr
HerderSCPDriver::getQSet(Hash const& qSetHash)
{
    return mPendingEnvelopes.getQSet(qSetHash);
}

void
HerderSCPDriver::ballotDidHearFromQuorum(uint64_t, SCPBallot const&)
{
}

void
HerderSCPDriver::recordBallotBlockedOnTxSet(uint64_t slotIndex,
                                            Value const& value)
{
    auto& timing = mSCPExecutionTimes[slotIndex];
    if (timing.mBallotBlockedOnTxSetStart.find(value) ==
        timing.mBallotBlockedOnTxSetStart.end())
    {
        timing.mBallotBlockedOnTxSetStart[value] = mApp.getClock().now();
    }
}

void
HerderSCPDriver::measureAndRecordBallotBlockedOnTxSet(uint64_t slotIndex,
                                                      Value const& value)
{
    auto it = mSCPExecutionTimes.find(slotIndex);
    if (it != mSCPExecutionTimes.end())
    {
        auto& timing = it->second;
        auto valueIt = timing.mBallotBlockedOnTxSetStart.find(value);
        if (valueIt != timing.mBallotBlockedOnTxSetStart.end())
        {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    mApp.getClock().now() - valueIt->second);
            mSCPMetrics.mBallotBlockedOnTxSet.Update(elapsed);
            return;
        }
    }

    // No blocking - record zero duration
    mSCPMetrics.mBallotBlockedOnTxSet.Update(std::chrono::milliseconds(0));
}

void
HerderSCPDriver::proposedValue(uint64_t slotIndex, uint32 round,
                               Value const& value)
{
    CLOG_DEBUG(Herder, "proposedValue i:{} round:{} v: {}", slotIndex, round,
               getValueString(value));
}

void
HerderSCPDriver::enteredRound(uint64_t slotIndex, uint32 round, bool timedOut)
{
    auto it = mSCPExecutionTimes.find(slotIndex);
    if (it != mSCPExecutionTimes.end())
    {
        it->second.mMaxRound = std::max(it->second.mMaxRound, round);
    }
    if (round > 1)
    {
        // Only read the cached schedule here: this is called from inside SCP
        // state changes (including state restore), where a quorum rebuild
        // (database reads) does not belong.
        auto const& cache = mLeaderSchedule;
        std::string leader = "unknown";
        if (cache.mSlotIndex == slotIndex && !cache.mSchedule.empty())
        {
            leader = toShortString(
                cache.mSchedule[(round - 2) % cache.mSchedule.size()]);
        }
        CLOG_INFO(Herder, "Slot {} moved to round {}{} (round {} leader: {})",
                  slotIndex, round, timedOut ? " after a timeout" : "",
                  round - 1, leader);
    }
}

void
HerderSCPDriver::startedBallotProtocol(uint64_t slotIndex,
                                       SCPBallot const& ballot)
{
    recordBallotStart(slotIndex);
    // Prewarm the next ledger's signatures, after SCP has finished processing
    // (and emitting) this step: the mempool pull waits for the overlay.
    StellarValue sv;
    if (toStellarValue(ballot.value, sv))
    {
        mApp.postOnMainThread(
            [this, slotIndex, txSetHash = sv.txSetHash]() {
                mHerder.prewarmNextLedgerSignatures(slotIndex, txSetHash);
            },
            "Herder: prewarm next ledger signatures");
    }
}
void
HerderSCPDriver::acceptedBallotPrepared(uint64_t slotIndex,
                                        SCPBallot const& ballot)
{
}

void
HerderSCPDriver::confirmedBallotPrepared(uint64_t slotIndex,
                                         SCPBallot const& ballot)
{
}

void
HerderSCPDriver::acceptedCommit(uint64_t slotIndex, SCPBallot const& ballot)
{
}

std::optional<VirtualClock::time_point>
HerderSCPDriver::getPrepareStart(uint64_t slotIndex)
{
    std::optional<VirtualClock::time_point> res;
    auto it = mSCPExecutionTimes.find(slotIndex);
    if (it != mSCPExecutionTimes.end())
    {
        res = it->second.mPrepareStart;
    }
    return res;
}

Json::Value
HerderSCPDriver::getQsetLagInfo(bool summary, bool fullKeys)
{
    Json::Value ret;
    double totalLag = 0;
    int numNodes = 0;

    auto qSet = getSCP().getLocalQuorumSet();
    LocalNode::forAllNodes(qSet, [&](NodeID const& n) {
        auto lag = getExternalizeLag(n);
        if (lag > 0)
        {
            if (!summary)
            {
                ret[toStrKey(n, fullKeys)] = static_cast<Json::UInt64>(lag);
            }
            else
            {
                totalLag += lag;
                numNodes++;
            }
        }
        return true;
    });

    if (summary && numNodes > 0)
    {
        double avgLag = totalLag / numNodes;
        ret = static_cast<Json::UInt64>(avgLag);
    }

    return ret;
}

Json::Value
HerderSCPDriver::getMaybeDeadNodes(bool fullKeys)
{
    Json::Value maybeDeadNodes(Json::arrayValue);
    for (auto const& node : mDeadNodes)
    {
        maybeDeadNodes.append(mApp.getConfig().toStrKey(node, fullKeys));
    }
    return maybeDeadNodes;
}

void
HerderSCPDriver::startCheckForDeadNodesInterval()
{
    mDeadNodes = std::move(mMissingNodes);
    mMissingNodes.clear();
    LocalNode::forAllNodes(getSCP().getLocalNode()->getQuorumSet(),
                           [this](NodeID const& nodeId) {
                               mMissingNodes.insert(nodeId);
                               return true;
                           });
}

double
HerderSCPDriver::getExternalizeLag(NodeID const& id) const
{
    auto n = mQSetLag.find(id);

    if (n == mQSetLag.end())
    {
        return 0.0;
    }

    return n->second.GetSnapshot().get75thPercentile();
}

void
HerderSCPDriver::recordTrigger(uint64_t slotIndex)
{
    auto& timing = mSCPExecutionTimes[slotIndex];
    if (!timing.mTriggerStart)
    {
        timing.mTriggerStart = mApp.getClock().now();
        if (timing.mPrepareStart)
        {
            // The node already voted for the round leader's proposal before
            // its own trigger fired.
            mSCPMetrics.mTriggerToBallot.Update(std::chrono::nanoseconds(0));
        }
    }
}

void
HerderSCPDriver::recordBallotStart(uint64_t slotIndex)
{
    auto& timing = mSCPExecutionTimes[slotIndex];
    VirtualClock::time_point start = mApp.getClock().now();

    // The first entry into ballot ends the trigger-to-ballot interval
    if (!timing.mPrepareStart && timing.mTriggerStart &&
        start >= *timing.mTriggerStart)
    {
        mSCPMetrics.mTriggerToBallot.Update(start - *timing.mTriggerStart);
    }
    timing.mPrepareStart = std::make_optional<VirtualClock::time_point>(start);
}

void
HerderSCPDriver::recordSCPExternalizeEvent(uint64_t slotIndex, NodeID const& id,
                                           bool forceUpdateSelf)
{
    auto& timing = mSCPExecutionTimes[slotIndex];
    auto now = mApp.getClock().now();

    if (!timing.mFirstExternalize)
    {
        timing.mFirstExternalize =
            std::make_optional<VirtualClock::time_point>(now);
    }

    if (id == mSCP.getLocalNodeID())
    {
        if (!timing.mSelfExternalize)
        {
            recordLogTiming(*timing.mFirstExternalize, now,
                            mSCPMetrics.mFirstToSelfExternalizeLag,
                            "first to self externalize lag",
                            std::chrono::nanoseconds::zero(), slotIndex);
        }
        if (!timing.mSelfExternalize || forceUpdateSelf)
        {
            timing.mSelfExternalize =
                std::make_optional<VirtualClock::time_point>(now);
        }
    }
    else
    {
        // Record externalize delay
        if (timing.mSelfExternalize)
        {
            recordLogTiming(
                *timing.mSelfExternalize, now,
                mSCPMetrics.mSelfToOthersExternalizeLag,
                fmt::format(FMT_STRING("self to {} externalize lag"),
                            toShortString(id)),
                std::chrono::nanoseconds::zero(), slotIndex);
        }

        // Record lag for other nodes
        auto& lag = mQSetLag[id];
        recordLogTiming(*timing.mFirstExternalize, now, lag,
                        fmt::format(FMT_STRING("first to {} externalize lag"),
                                    toShortString(id)),
                        std::chrono::nanoseconds::zero(), slotIndex);
    }
}

void
HerderSCPDriver::recordLogTiming(VirtualClock::time_point start,
                                 VirtualClock::time_point end,
                                 medida::Timer& timer,
                                 std::string const& logStr,
                                 std::chrono::nanoseconds threshold,
                                 uint64_t slotIndex)
{
    auto delta =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    CLOG_DEBUG(
        Herder, "{} delta for slot {} is {} ms", logStr, slotIndex,
        std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
    if (delta >= threshold)
    {
        timer.Update(delta);
    }
};

void
HerderSCPDriver::recordSCPExecutionMetrics(uint64_t slotIndex)
{
    auto externalizeStart = mApp.getClock().now();

    // Use threshold of 0 in case of a single node
    auto& qset = mApp.getConfig().QUORUM_SET;
    auto isSingleNode = qset.innerSets.size() == 0 &&
                        qset.validators.size() == 1 &&
                        qset.validators[0] == getSCP().getLocalNodeID();
    auto threshold = isSingleNode ? std::chrono::nanoseconds::zero()
                                  : Herder::TIMERS_THRESHOLD_NANOSEC;

    auto SCPTimingIt = mSCPExecutionTimes.find(slotIndex);
    if (SCPTimingIt == mSCPExecutionTimes.end())
    {
        return;
    }

    auto& SCPTiming = SCPTimingIt->second;

    mLeaderTimeout.Update(SCPTiming.mLeaderTimeoutCount);
    mPrepareTimeout.Update(SCPTiming.mPrepareTimeoutCount);
    if (SCPTiming.mMaxRound > 0)
    {
        mLeaderRound.Update(SCPTiming.mMaxRound);
    }

    if (SCPTiming.mLeaderTimeoutCount > 0)
    {
        CLOG_INFO(Herder,
                  "Slot {} timed out {} time(s) waiting for a round leader "
                  "(externalized in round {})",
                  slotIndex, SCPTiming.mLeaderTimeoutCount,
                  SCPTiming.mMaxRound);
    }

    // Compute prepare time
    // The 'threshold' here acts as a filter to coarsely exclude from
    // metric-recording events that occur "too close together". This
    // happens when the current node is not actually keeping up with
    // consensus (i.e. not participating meaningfully): it receives bursts
    // of SCP messages that traverse all SCP states "instantly". If we
    // record those events it gives the misleading impression of the node
    // going "super fast", which is not really accurate: the node is
    // actually going so slow nobody's even listening to it anymore, it's
    // just being dragged along with its quorum.
    //
    // Unfortunately by excluding these "too fast" events we produce a
    // different distortion in that case: we record so few events that the
    // node looks like it's "going fast" from mere _sparsity of data_, the
    // summary metric only recording a handful of samples. What you want
    // to look at -- any time you're examining SCP phase-timing data -- is
    // the combination of this timer _and_ the lag timers that say whether
    // the node is so lagged that nobody's listening to it.
    if (SCPTiming.mPrepareStart)
    {
        recordLogTiming(*SCPTiming.mPrepareStart, externalizeStart,
                        mSCPMetrics.mPrepareToExternalize, "Prepare", threshold,
                        slotIndex);
    }
}

namespace
{
// Remove expired weak_ptrs from each vector in the map, and erase map entries
// whose vectors become empty.
template <typename T>
void
purgeExpiredWeakPtrs(std::map<Hash, std::vector<std::weak_ptr<T>>>& map)
{
    for (auto mapIt = map.begin(); mapIt != map.end();)
    {
        auto& vec = mapIt->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [](auto& wp) { return wp.expired(); }),
                  vec.end());
        if (vec.empty())
        {
            mapIt = map.erase(mapIt);
        }
        else
        {
            ++mapIt;
        }
    }
}
}

void
HerderSCPDriver::purgeSlotsOutsideRange(std::optional<uint64_t> minSlotIndex,
                                        std::optional<uint64_t> maxSlotIndex,
                                        uint64 slotToKeep)
{
    // Erase `it` and advance it, unless `it` is `slotToKeep`, in which case
    // just advance it.
    auto const maybePurge = [&](auto& it) {
        if (it->first == slotToKeep)
        {
            ++it;
        }
        else
        {
            it = mSCPExecutionTimes.erase(it);
        }
    };

    // Clean up timings map — below
    if (minSlotIndex)
    {
        auto it = mSCPExecutionTimes.begin();
        while (it != mSCPExecutionTimes.end() && it->first < *minSlotIndex)
        {
            maybePurge(it);
        }
    }

    // Clean up timings map — above
    if (maxSlotIndex)
    {
        auto it = mSCPExecutionTimes.upper_bound(*maxSlotIndex);
        while (it != mSCPExecutionTimes.end())
        {
            maybePurge(it);
        }
    }

    getSCP().purgeSlotsOutsideRange(minSlotIndex, maxSlotIndex, slotToKeep);

    // Clean up expired weak_ptrs from the pending tx set registries.
    purgeExpiredWeakPtrs(mPendingTxSetWrappers);
    purgeExpiredWeakPtrs(mPendingTxSetEnvelopeWrappers);
}

void
HerderSCPDriver::onTxSetReceived(Hash const& txSetHash,
                                 TxSetXDRFrameConstPtr txSet)
{
    // Update any ValueWrappers waiting for this tx set
    auto it = mPendingTxSetWrappers.find(txSetHash);
    if (it != mPendingTxSetWrappers.end())
    {
        for (auto& wp : it->second)
        {
            if (auto sp = wp.lock())
            {
                sp->setTxSet(txSet);
            }
        }
        mPendingTxSetWrappers.erase(it);
    }

    // Update any EnvelopeWrappers waiting for this tx set
    auto envIt = mPendingTxSetEnvelopeWrappers.find(txSetHash);
    if (envIt != mPendingTxSetEnvelopeWrappers.end())
    {
        for (auto& wp : envIt->second)
        {
            if (auto sp = wp.lock())
            {
                sp->addTxSet(txSet);
            }
        }
        mPendingTxSetEnvelopeWrappers.erase(envIt);
    }
}

void
HerderSCPDriver::clearSCPExecutionEvents()
{
    mSCPExecutionTimes.clear();
}

// Value handling
class SCPHerderValueWrapper : public ValueWrapper
{
    HerderImpl& mHerder;

    TxSetXDRFrameConstPtr mTxSet;
    Hash const mTxSetHash;

  public:
    explicit SCPHerderValueWrapper(StellarValue const& sv, Value const& value,
                                   HerderImpl& herder)
        : ValueWrapper(value), mHerder(herder), mTxSetHash(sv.txSetHash)
    {
        auto const result = mHerder.getTxSet(sv.txSetHash);
        if (auto const* ptr = std::get_if<TxSetXDRFrameConstPtr>(&result))
        {
            mTxSet = *ptr;
        }
        // else: EmptyTxSet -> mTxSet stays null
        // mTxSet may also be null if tx set hasn't been received yet
        // (parallel downloading). It will be set later via setTxSet()
        // when the tx set arrives.
    }

    bool
    hasTxSet() const
    {
        return mTxSet != nullptr || mTxSetHash == Herder::EMPTY_TX_SET_HASH;
    }

    Hash const&
    getTxSetHash() const
    {
        return mTxSetHash;
    }

    void
    setTxSet(TxSetXDRFrameConstPtr txSet) override
    {
        releaseAssert(txSet->getContentsHash() == mTxSetHash);
        mTxSet = txSet;
    }
};

ValueWrapperPtr
HerderSCPDriver::wrapValue(Value const& val)
{
    StellarValue sv = toStellarValueOrThrow(val);
    auto res = std::make_shared<SCPHerderValueWrapper>(sv, val, mHerder);

    // If tx set wasn't available, register this wrapper to be updated later
    // when the tx set arrives via onTxSetReceived()
    if (!res->hasTxSet())
    {
        mPendingTxSetWrappers[res->getTxSetHash()].push_back(res);
    }

    return res;
}

ValueWrapperPtr
HerderSCPDriver::wrapStellarValue(StellarValue const& sv)
{
    auto val = xdr::xdr_to_opaque(sv);
    auto res = std::make_shared<SCPHerderValueWrapper>(sv, val, mHerder);

    // If tx set wasn't available, register this wrapper to be updated later
    // when the tx set arrives via onTxSetReceived()
    if (!res->hasTxSet())
    {
        mPendingTxSetWrappers[res->getTxSetHash()].push_back(res);
    }

    return res;
}

void
HerderSCPDriver::cacheValidTxSet(ApplicableTxSetFrame const& txSet,
                                 LedgerHeaderHistoryEntry const& lcl,
                                 ApplyTimeOffset closeTimeOffset) const
{
    auto key =
        TxSetValidityKey{lcl.hash, txSet.getContentsHash(),
                         closeTimeOffset.seconds(), closeTimeOffset.seconds()};
    bool* pRes = mTxSetValidCache.maybeGet(key);
    if (pRes == nullptr)
    {
#ifdef SCP_DEBUGGING
        releaseAssert(txSet.checkValid(mApp, closeTimeOffset.seconds(),
                                       closeTimeOffset.seconds()));
#endif
        mTxSetValidCache.put(key, true);
    }
    else
    {
        if (!*pRes)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("Inconsistent txSet validity for tx set {}"),
                hexAbbrev(txSet.getContentsHash())));
        }
    }
}

bool
HerderSCPDriver::checkAndCacheTxSetValid(TxSetXDRFrame const& txSet,
                                         LedgerHeaderHistoryEntry const& lcl,
                                         ApplyTimeOffset closeTimeOffset) const
{
    ZoneScoped;

    auto key =
        TxSetValidityKey{lcl.hash, txSet.getContentsHash(),
                         closeTimeOffset.seconds(), closeTimeOffset.seconds()};

    bool* pRes = mTxSetValidCache.maybeGet(key);
    if (pRes == nullptr)
    {
        ZoneNamedN(txSetValidityMissZone, "txset validity cache miss", true);
        // Validation verifies the remaining signatures itself, on all cores;
        // a concurrent signature prewarm would only compete with it.
        mHerder.stopSignaturePrewarm();
        uint64_t sigCacheHits = 0;
        uint64_t sigCacheMisses = 0;
        PubKeyUtils::getVerifySigCacheCounts(sigCacheHits, sigCacheMisses);
        auto validationTime = mSCPMetrics.mTxSetValidation.TimeScope();

        // The invariant here is that we only validate tx sets nominated
        // to be applied to the current ledger state. However, in case
        // if we receive a bad SCP value for the current state, we still
        // might end up with malformed tx set that doesn't refer to the
        // LCL.
        ApplicableTxSetFrameConstPtr applicableTxSet;
        if (txSet.previousLedgerHash() == lcl.hash)
        {
            applicableTxSet = txSet.prepareForApply(mApp, lcl.header);
        }

        bool res = true;
        if (applicableTxSet == nullptr)
        {
            CLOG_ERROR(
                Herder, "validateValue i:{} can't prepare txSet {} for apply",
                (lcl.header.ledgerSeq + 1), hexAbbrev(txSet.getContentsHash()));
            res = false;
        }
        else
        {
            res = applicableTxSet->checkValid(mApp, closeTimeOffset.seconds(),
                                              closeTimeOffset.seconds());
        }
        markSignatureCacheLookups(mApp, "validate", sigCacheHits,
                                  sigCacheMisses);

        mTxSetValidCache.put(key, res);
        return res;
    }
    else
    {
        ZoneNamedN(txSetValidityHitZone, "txset validity cache hit", true);
        return *pRes;
    }
}
size_t
HerderSCPDriver::TxSetValidityKeyHash::operator()(
    TxSetValidityKey const& key) const
{

    size_t res = std::hash<Hash>()(std::get<0>(key));
    hashMix(res, std::hash<Hash>()(std::get<1>(key)));
    hashMix(res, std::get<2>(key));
    hashMix(res, std::get<3>(key));
    return res;
}

std::optional<int64_t>
HerderSCPDriver::getLeaderTimeouts(uint64_t slotIndex) const
{
    auto it = mSCPExecutionTimes.find(slotIndex);
    if (it != mSCPExecutionTimes.end())
    {
        return it->second.mLeaderTimeoutCount;
    }
    return std::nullopt;
}

std::chrono::milliseconds
HerderSCPDriver::getTriggerToBallotDuration(uint64_t slotIndex) const
{
    auto it = mSCPExecutionTimes.find(slotIndex);
    if (it != mSCPExecutionTimes.end())
    {
        auto const& timing = it->second;
        if (timing.mTriggerStart && timing.mPrepareStart &&
            *timing.mPrepareStart > *timing.mTriggerStart)
        {
            // Stop at ballot even if a local proposal is still being built.
            // Ballot and apply are already covered by the fallback anchor.
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                *timing.mPrepareStart - *timing.mTriggerStart);
        }
    }
    return std::chrono::milliseconds::zero();
}

void
HerderSCPDriver::markSlotAsRestored(uint64_t slotIndex)
{
    mRestoredSlotIndices.insert(slotIndex);
}

}
