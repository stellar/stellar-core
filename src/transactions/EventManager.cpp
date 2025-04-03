#include "transactions/EventManager.h"
#include "crypto/KeyUtils.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"

namespace stellar
{

// If the event was emitted by an SAC, return the asset. Otherwise, return
// nullopt
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
                               OperationFrame const& op, Memo const& memo)
    : mParent(parentTxEventManager), mOp(op), mMemo(memo)
{
}

void
OpEventManager::eventsForClaimAtoms(
    MuxedAccount const& source,
    xdr::xvector<stellar::ClaimAtom> const& claimAtoms)
{
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    auto sourceSCAddress = accountToSCAddress(source);

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
                                            seller, amountToSeller);
            eventForTransferWithIssuerCheck(assetToSource, seller,
                                            sourceSCAddress, amountToSource);
            break;
        }
        case CLAIM_ATOM_TYPE_ORDER_BOOK:
        {
            auto seller = accountToSCAddress(atom.orderBook().sellerID);

            auto amountToSeller = atom.orderBook().amountBought;
            auto assetToSeller = atom.orderBook().assetBought;

            auto amountToSource = atom.orderBook().amountSold;
            auto assetToSource = atom.orderBook().assetSold;

            eventForTransferWithIssuerCheck(assetToSeller, sourceSCAddress,
                                            seller, amountToSeller);
            eventForTransferWithIssuerCheck(assetToSource, seller,
                                            sourceSCAddress, amountToSource);
            break;
        }
        case CLAIM_ATOM_TYPE_LIQUIDITY_POOL:
        {
            auto poolID = liquidityPoolIDToSCAddress(
                atom.liquidityPool().liquidityPoolID);

            auto amountToPool = atom.liquidityPool().amountBought;
            auto assetToPool = atom.liquidityPool().assetBought;

            auto amountFromPool = atom.liquidityPool().amountSold;
            auto assetFromPool = atom.liquidityPool().assetSold;

            eventForTransferWithIssuerCheck(assetToPool, sourceSCAddress,
                                            poolID, amountToPool);
            eventForTransferWithIssuerCheck(assetFromPool, poolID,
                                            sourceSCAddress, amountFromPool);
            break;
        }
        }
    }
}

void
OpEventManager::eventForTransferWithIssuerCheck(Asset const& asset,
                                                SCAddress const& from,
                                                SCAddress const& to,
                                                int64 amount)
{
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    auto fromIsIssuer = isIssuer(from, asset);
    auto toIsIssuer = isIssuer(to, asset);

    if (fromIsIssuer && toIsIssuer)
    {
        newTransferEvent(asset, from, to, amount);
    }
    else if (fromIsIssuer)
    {
        newMintEvent(asset, to, amount);
    }
    else if (toIsIssuer)
    {
        newBurnEvent(asset, from, amount);
    }
    else
    {
        newTransferEvent(asset, from, to, amount);
    }
}

void
OpEventManager::newTransferEvent(Asset const& asset, SCAddress const& from,
                                 SCAddress const& to, int64 amount)
{
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() =
        getAssetContractID(mParent.getNetworkID(), asset);

    SCVec topics = {makeSymbolSCVal("transfer"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(from)),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(to)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    // mux follows order of precedence
    // no mux no memo -- data is just i128
    // else data is an scmap

    bool is_from_mux = from.type() == SC_ADDRESS_TYPE_MUXED_ACCOUNT;
    bool is_to_mux = to.type() == SC_ADDRESS_TYPE_MUXED_ACCOUNT;
    bool has_memo = mMemo.type() != MemoType::MEMO_NONE;

    SCVal amountVal = makeI128SCVal(amount);

    bool is_to_muxed_with_memo =
        has_memo && to.type() == SC_ADDRESS_TYPE_ACCOUNT;
    if (!is_from_mux && !is_to_mux && !is_to_muxed_with_memo)
    {
        ev.body.v0().data = amountVal;
    }
    else
    {
        // data is ScMap
        SCVal data(SCV_MAP);
        SCMap& dataMap = data.map().activate();

        SCMapEntry amountEntry;
        amountEntry.key = makeSymbolSCVal("amount");
        amountEntry.val = amountVal;
        dataMap.push_back(amountEntry);

        if (is_from_mux)
        {
            SCMapEntry fromEntry;
            fromEntry.key = makeSymbolSCVal("from_muxed_id");
            fromEntry.val = makeMuxIDSCVal(from.muxedAccount());
            dataMap.push_back(fromEntry);
        }

        if (is_to_mux)
        {
            SCMapEntry toEntry;
            toEntry.key = makeSymbolSCVal("to_muxed_id");
            toEntry.val = makeMuxIDSCVal(to.muxedAccount());
            dataMap.push_back(toEntry);
        }
        else if (is_to_muxed_with_memo)
        {
            SCMapEntry toEntry;
            toEntry.key = makeSymbolSCVal("to_muxed_id");
            toEntry.val = makeClassicMemoSCVal(mMemo);
            dataMap.push_back(toEntry);
        }

        ev.body.v0().data = data;
    }
    mContractEvents.emplace_back(std::move(ev));
}

void
OpEventManager::newMintEvent(Asset const& asset, SCAddress const& to,
                             int64 amount)
{
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() =
        getAssetContractID(mParent.getNetworkID(), asset);

    SCVec topics = {makeSymbolSCVal("mint"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(to)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    ev.body.v0().data = makeI128SCVal(amount);

    mContractEvents.emplace_back(std::move(ev));
}

void
OpEventManager::newBurnEvent(Asset const& asset, SCAddress const& from,
                             int64 amount)
{
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() =
        getAssetContractID(mParent.getNetworkID(), asset);

    SCVec topics = {makeSymbolSCVal("burn"),
                    makeAddressSCVal(getAddressWithDroppedMuxedInfo(from)),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    ev.body.v0().data = makeI128SCVal(amount);

    mContractEvents.emplace_back(std::move(ev));
}

void
OpEventManager::newClawbackEvent(Asset const& asset, SCAddress const& from,
                                 int64 amount)
{
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() =
        getAssetContractID(mParent.getNetworkID(), asset);

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
    if (!mParent.shouldEmitClassicEvents())
    {
        return;
    }

    ContractEvent ev;
    ev.type = ContractEventType::CONTRACT;
    ev.contractID.activate() =
        getAssetContractID(mParent.getNetworkID(), asset);

    SCVec topics = {makeSymbolSCVal("set_authorized"), makeAccountIDSCVal(id),
                    makeSep0011AssetStringSCVal(asset)};
    ev.body.v0().topics = topics;

    SCVal val(SCV_BOOL);
    val.b() = authorize;

    ev.body.v0().data = val;

    mContractEvents.emplace_back(std::move(ev));
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

xdr::xvector<ContractEvent> const&
OpEventManager::getContractEvents()
{
    return mContractEvents;
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
TxEventManager::createNewOpEventManager(OperationFrame const& op,
                                        Memo const& memo)
{
    return OpEventManager(*this, op, memo);
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

Hash const&
TxEventManager::getNetworkID() const
{
    return mNetworkID;
}

uint32_t
TxEventManager::getProtocolVersion() const
{
    return mProtocolVersion;
}

Config const&
TxEventManager::getConfig() const
{
    return mConfig;
}

bool
TxEventManager::shouldEmitClassicEvents() const
{
    if (!mConfig.EMIT_CLASSIC_EVENTS)
    {
        return false;
    }

    return protocolVersionStartsFrom(mProtocolVersion, ProtocolVersion::V_23) ||
           mConfig.BACKFILL_STELLAR_ASSET_EVENTS;
}

} // namespace stellar