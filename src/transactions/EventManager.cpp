#include "transactions/EventManager.h"
#include "crypto/KeyUtils.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"
#include "xdrpp/printer.h"

namespace stellar
{
namespace
{
bool
classicEventsEnabled(uint32_t protocolVersion, Config const& config)
{
    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23))
    {
        return config.EMIT_CLASSIC_EVENTS;
    }
    return config.BACKFILL_STELLAR_ASSET_EVENTS;
}
} // namespace

std::optional<Asset>
getAssetFromEvent(ContractEvent const& event, Hash const& networkID)
{
    if (!event.contractID)
    {
        return std::nullopt;
    }
    auto const& topics = event.body.v0().topics;
    if (topics.empty())
    {
        return std::nullopt;
    }

    // The last topic will be the SEP-0011 asset string for the SAC
    auto assetVal = topics.at(topics.size() - 1);
    if (assetVal.type() != SCV_STRING)
    {
        return std::nullopt;
    }

    Asset asset;
    auto const& assetStr = assetVal.str();
    if (assetStr == "native")
    {
        asset.type(ASSET_TYPE_NATIVE);
    }
    else
    {
        auto delimPos = assetStr.find(':');
        if (delimPos == std::string::npos || delimPos == assetStr.size() - 1)
        {
            return std::nullopt;
        }
        auto issuerStr = assetStr.substr(delimPos + 1, assetStr.size());

        PublicKey issuer;

        try
        {
            issuer = KeyUtils::fromStrKey<PublicKey>(issuerStr);
        }
        catch (std::invalid_argument)
        {
            return std::nullopt;
        }

        auto assetName = assetStr.substr(0, delimPos);
        if (assetName.size() <= 4)
        {
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
            strToAssetCode(asset.alphaNum4().assetCode, assetName);
            asset.alphaNum4().issuer = issuer;
        }
        else if (assetName.size() <= 12)
        {
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
            strToAssetCode(asset.alphaNum12().assetCode, assetName);
            asset.alphaNum12().issuer = issuer;
        }
        else
        {
            return std::nullopt;
        }

        // The protocol version check is only used for poolShareAssets,
        // and we aren't passing one in here, so the value does not
        // matter
        if (!isAssetValid<Asset>(asset, 0))
        {
            return std::nullopt;
        }
    }
    auto hash = getAssetContractID(networkID, asset);
    if (hash != *event.contractID)
    {
        return std::nullopt;
    }

    return asset;
}

SCVal
getPossibleMuxedData(SCAddress const& to, int64 amount, Memo const& memo,
                     bool allowMuxedIdOrMemo)
{
    bool is_to_mux = to.type() == SC_ADDRESS_TYPE_MUXED_ACCOUNT;

    SCVal amountVal = makeI128SCVal(amount);

    bool is_to_account_with_memo = to.type() == SC_ADDRESS_TYPE_ACCOUNT &&
                                   memo.type() != MemoType::MEMO_NONE;
    if (!allowMuxedIdOrMemo || (!is_to_mux && !is_to_account_with_memo))
    {
        return amountVal;
    }
    else
    {
        SCVal data(SCV_MAP);
        SCMap& dataMap = data.map().activate();

        SCMapEntry amountEntry;
        amountEntry.key = makeSymbolSCVal("amount");
        amountEntry.val = amountVal;
        dataMap.push_back(amountEntry);

        SCMapEntry toEntry;
        toEntry.key = makeSymbolSCVal("to_muxed_id");
        toEntry.val = is_to_mux ? makeMuxIDSCVal(to.muxedAccount())
                                : makeClassicMemoSCVal(memo);
        dataMap.push_back(toEntry);

        return data;
    }
}

DiagnosticEventManager
DiagnosticEventManager::createForApply(bool metaEnabled,
                                       TransactionFrameBase const& tx,
                                       Config const& config)
{
    bool enabled = metaEnabled && tx.isSoroban() &&
                   config.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS;
    return DiagnosticEventManager(enabled);
}

DiagnosticEventManager
DiagnosticEventManager::createForValidation(Config const& config)
{
    return DiagnosticEventManager(config.ENABLE_DIAGNOSTICS_FOR_TX_SUBMISSION);
}

DiagnosticEventManager
DiagnosticEventManager::createDisabled()
{
    return DiagnosticEventManager(false);
}

void
DiagnosticEventManager::pushEvent(DiagnosticEvent&& event)
{
    if (mEnabled)
    {
        releaseAssert(!mFinalized);
        mBuffer.emplace_back(std::move(event));
    }
}

DiagnosticEventManager::DiagnosticEventManager(bool enabled) : mEnabled(enabled)
{
}

void
DiagnosticEventManager::pushError(SCErrorType ty, SCErrorCode code,
                                  std::string&& message,
                                  xdr::xvector<SCVal>&& args)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
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

bool
DiagnosticEventManager::isEnabled() const
{
    return mEnabled;
}

xdr::xvector<DiagnosticEvent>
DiagnosticEventManager::finalize()
{
    releaseAssert(!mFinalized);
    mFinalized = true;
    return std::move(mBuffer);
}

void
DiagnosticEventManager::debugLogEvents() const
{
    for (auto const& event : mBuffer)
    {
        CLOG_DEBUG(Tx, "Soroban diagnostic event: {}",
                   xdr::xdr_to_string(event));
    }
}

OpEventManager::OpEventManager(bool metaEnabled, bool isSoroban,
                               uint32_t protocolVersion, Hash const& networkID,
                               Memo const& memo, Config const& config)
    : mNetworkID(networkID), mMemo(memo)
{
    mEnabled = metaEnabled &&
               (isSoroban || classicEventsEnabled(protocolVersion, config));
    mUpdateSACEventsToProtocol23Format =
        config.BACKFILL_STELLAR_ASSET_EVENTS &&
        protocolVersionIsBefore(protocolVersion, ProtocolVersion::V_23);
}

void
OpEventManager::eventsForClaimAtoms(
    MuxedAccount const& source,
    xdr::xvector<stellar::ClaimAtom> const& claimAtoms)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    auto sourceSCAddress = makeMuxedAccountAddress(source);

    for (auto const& atom : claimAtoms)
    {
        switch (atom.type())
        {
        case CLAIM_ATOM_TYPE_V0:
        {
            SCAddress seller(SC_ADDRESS_TYPE_ACCOUNT);
            seller.accountId().ed25519() = atom.v0().sellerEd25519;

            auto amountToSeller = atom.v0().amountBought;
            auto assetToSeller = atom.v0().assetBought;

            auto amountToSource = atom.v0().amountSold;
            auto assetToSource = atom.v0().assetSold;

            eventForTransferWithIssuerCheck(assetToSeller, sourceSCAddress,
                                            seller, amountToSeller, false);
            eventForTransferWithIssuerCheck(
                assetToSource, seller, sourceSCAddress, amountToSource, false);
            break;
        }
        case CLAIM_ATOM_TYPE_ORDER_BOOK:
        {
            auto seller = makeAccountAddress(atom.orderBook().sellerID);

            auto amountToSeller = atom.orderBook().amountBought;
            auto assetToSeller = atom.orderBook().assetBought;

            auto amountToSource = atom.orderBook().amountSold;
            auto assetToSource = atom.orderBook().assetSold;

            eventForTransferWithIssuerCheck(assetToSeller, sourceSCAddress,
                                            seller, amountToSeller, false);
            eventForTransferWithIssuerCheck(
                assetToSource, seller, sourceSCAddress, amountToSource, false);
            break;
        }
        case CLAIM_ATOM_TYPE_LIQUIDITY_POOL:
        {
            auto poolID =
                makeLiquidityPoolAddress(atom.liquidityPool().liquidityPoolID);

            auto amountToPool = atom.liquidityPool().amountBought;
            auto assetToPool = atom.liquidityPool().assetBought;

            auto amountFromPool = atom.liquidityPool().amountSold;
            auto assetFromPool = atom.liquidityPool().assetSold;

            eventForTransferWithIssuerCheck(assetToPool, sourceSCAddress,
                                            poolID, amountToPool, false);
            eventForTransferWithIssuerCheck(
                assetFromPool, poolID, sourceSCAddress, amountFromPool, false);
            break;
        }
        }
    }
}

void
OpEventManager::eventForTransferWithIssuerCheck(Asset const& asset,
                                                SCAddress const& from,
                                                SCAddress const& to,
                                                int64 amount,
                                                bool allowMuxedIdOrMemo)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    auto fromIsIssuer = isIssuer(from, asset);
    auto toIsIssuer = isIssuer(to, asset);

    if (fromIsIssuer && toIsIssuer)
    {
        newTransferEvent(asset, from, to, amount, allowMuxedIdOrMemo);
    }
    else if (fromIsIssuer)
    {
        newMintEvent(asset, to, amount, allowMuxedIdOrMemo);
    }
    else if (toIsIssuer)
    {
        newBurnEvent(asset, from, amount);
    }
    else
    {
        newTransferEvent(asset, from, to, amount, allowMuxedIdOrMemo);
    }
}

void
OpEventManager::newTransferEvent(Asset const& asset, SCAddress const& from,
                                 SCAddress const& to, int64 amount,
                                 bool allowMuxedIdOrMemo)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() = getAssetContractID(mNetworkID, asset);

    SCVec topics = {makeSymbolSCVal("transfer"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(from)),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(to)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    ev.body.v0().data =
        getPossibleMuxedData(to, amount, mMemo, allowMuxedIdOrMemo);
    mContractEvents.emplace_back(std::move(ev));
}

ContractEvent
OpEventManager::makeMintEvent(Asset const& asset, SCAddress const& to,
                              int64 amount, bool allowMuxedIdOrMemo)
{
    releaseAssert(!mFinalized);
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() = getAssetContractID(mNetworkID, asset);

    SCVec topics = {makeSymbolSCVal("mint"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(to)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    ev.body.v0().data =
        getPossibleMuxedData(to, amount, mMemo, allowMuxedIdOrMemo);

    return ev;
}

ContractEvent
OpEventManager::makeBurnEvent(Asset const& asset, SCAddress const& from,
                              int64 amount)
{
    releaseAssert(!mFinalized);
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() = getAssetContractID(mNetworkID, asset);

    SCVec topics = {makeSymbolSCVal("burn"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(from)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    ev.body.v0().data = makeI128SCVal(amount);

    return ev;
}

void
OpEventManager::newMintEvent(Asset const& asset, SCAddress const& to,
                             int64 amount, bool allowMuxedIdOrMemo,
                             bool insertAtBeginning)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);

    auto ev = makeMintEvent(asset, to, amount, allowMuxedIdOrMemo);

    if (insertAtBeginning)
    {
        // This will only be called pre protocol 8 in rare scenarios where XLM
        // was minted, so the performance hit isn't an issue. We could also use
        // a std::deque instead.
        mContractEvents.emplace(mContractEvents.begin(), std::move(ev));
    }
    else
    {
        mContractEvents.emplace_back(std::move(ev));
    }
}

void
OpEventManager::newBurnEvent(Asset const& asset, SCAddress const& from,
                             int64 amount)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    auto ev = makeBurnEvent(asset, from, amount);

    mContractEvents.emplace_back(std::move(ev));
}

void
OpEventManager::newClawbackEvent(Asset const& asset, SCAddress const& from,
                                 int64 amount)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() = getAssetContractID(mNetworkID, asset);

    SCVec topics = {makeSymbolSCVal("clawback"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(from)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    ev.body.v0().data = makeI128SCVal(amount);

    mContractEvents.emplace_back(std::move(ev));
}

void
OpEventManager::newSetAuthorizedEvent(Asset const& asset, AccountID const& id,
                                      bool authorize)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() = getAssetContractID(mNetworkID, asset);

    SCVec topics = {makeSymbolSCVal("set_authorized"), makeAccountIDSCVal(id),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    SCVal val(SCV_BOOL);
    val.b() = authorize;

    ev.body.v0().data = val;

    mContractEvents.emplace_back(std::move(ev));
}

void
OpEventManager::setEvents(xdr::xvector<ContractEvent>&& events)
{
    if (!mEnabled)
    {
        return;
    }
    releaseAssert(!mFinalized);
    releaseAssert(mContractEvents.empty());
    mContractEvents = std::move(events);

    if (!mUpdateSACEventsToProtocol23Format)
    {
        return;
    }

    // Modify backfilled SAC events to match V23 format
    for (auto& event : mContractEvents)
    {
        auto res = getAssetFromEvent(event, mNetworkID);
        if (!res)
        {
            continue;
        }
        auto const& asset = *res;

        auto& topics = event.body.v0().topics;

        auto eventNameVal = topics.at(0);
        releaseAssertOrThrow(eventNameVal.type() == SCV_SYMBOL);

        if (eventNameVal.sym() == "transfer")
        {
            releaseAssertOrThrow(topics.size() == 4);

            auto const& fromVal = topics.at(1);
            auto const& toVal = topics.at(2);
            bool fromIsIssuer = isIssuer(fromVal.address(), asset);
            bool toIsIssuer = isIssuer(toVal.address(), asset);

            if ((fromIsIssuer && toIsIssuer) || (!fromIsIssuer && !toIsIssuer))
            {
                continue;
            }

            // Exactly one of from or two is the issuer

            // Make sure to check data!
            // 1. Change the event name topic to mint or burn
            // 2. Remove the issuer topic
            if (fromIsIssuer)
            {
                topics.at(0).sym() = "mint";
                topics.erase(topics.begin() + 1);
            }
            else
            {
                topics.at(0).sym() = "burn";
                topics.erase(topics.begin() + 2);
            }
        }
        else if (eventNameVal.sym() == "mint" ||
                 eventNameVal.sym() == "clawback" ||
                 eventNameVal.sym() == "set_authorized")
        {
            releaseAssertOrThrow(topics.size() == 4);

            // The admin should be the second topic
            topics.erase(topics.begin() + 1);
        }
    }
}

xdr::xvector<ContractEvent> const&
OpEventManager::getEvents()
{
    return mContractEvents;
}

bool
OpEventManager::isEnabled() const
{
    return mEnabled;
}

xdr::xvector<ContractEvent>
OpEventManager::finalize()
{
    releaseAssert(!mFinalized);
    mFinalized = true;
    return std::move(mContractEvents);
}

TxEventManager::TxEventManager(bool metaEnabled, uint32_t protocolVersion,
                               Hash const& networkID, Config const& config)
    : mNetworkID(networkID)
{
    mEnabled = metaEnabled && classicEventsEnabled(protocolVersion, config);
}

void
TxEventManager::newFeeEvent(AccountID const& feeSource, int64_t amount,
                            TransactionEventStage stage)
{
    // We don't emit 0 fee events. This is relevant for Soroban transactions
    // that end up with no refunds.
    if (!mEnabled || amount == 0)
    {
        return;
    }
    releaseAssert(!mFinalized);

    Asset native(ASSET_TYPE_NATIVE);
    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() =
        getAssetContractInfo(native, mNetworkID).mAssetContractID;

    SCVec topics = {makeSymbolSCVal("fee"), makeAccountIDSCVal(feeSource)};
    ev.body.v0().topics = topics;
    ev.body.v0().data = makeI128SCVal(amount);

    mTxEvents.emplace_back(stage, std::move(ev));
}

bool
TxEventManager::isEnabled() const
{
    return mEnabled;
}

xdr::xvector<TransactionEvent>
TxEventManager::finalize()
{
    releaseAssert(!mFinalized);
    mFinalized = true;
    return std::move(mTxEvents);
}

} // namespace stellar
