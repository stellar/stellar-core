// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <json/json.h>
#include <medida/metrics_registry.h>
#include <xdrpp/types.h>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "rust/RustVecXdrMarshal.h"
#endif
// clang-format on

#include "ledger/LedgerTxnImpl.h"
#include "rust/CppShims.h"
#include "xdr/Stellar-transaction.h"
#include <stdexcept>
#include <xdrpp/xdrpp/printer.h>

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "rust/RustBridge.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include <crypto/SHA.h>

namespace stellar
{

CxxLedgerInfo
getLedgerInfo(AbstractLedgerTxn& ltx, Config const& cfg)
{
    CxxLedgerInfo info;
    auto const& hdr = ltx.loadHeader().current();
    info.base_reserve = hdr.baseReserve;
    info.protocol_version = hdr.ledgerVersion;
    info.sequence_number = hdr.ledgerSeq;
    info.timestamp = hdr.scpValue.closeTime;
    // TODO: move network id to config to not recompute hash
    auto networkID = sha256(cfg.NETWORK_PASSPHRASE);
    for (auto c : networkID)
    {
        info.network_id.push_back(static_cast<unsigned char>(c));
    }
    return info;
}

uint32_t
getHostLogicVersion(AbstractLedgerTxn& ltx)
{
    LedgerKey hostLogicVersionLedgerKey;
    hostLogicVersionLedgerKey.type(CONFIG_SETTING);
    hostLogicVersionLedgerKey.configSetting().configSettingID =
        CONFIG_SETTING_CONTRACT_HOST_LOGIC_VERSION;
    auto hostLogicVersionConfigEntry =
        ltx.loadWithoutRecord(hostLogicVersionLedgerKey);
    if (hostLogicVersionConfigEntry)
    {
        return hostLogicVersionConfigEntry.current()
            .data.configSetting()
            .contractHostLogicVersion();
    }
    // Default host logic version is the 'hi' host version
    // wired-in to the current binary
    return static_cast<uint32_t>(
        rust_bridge::get_soroban_host_logic_versions().hi);
}

template <typename T>
std::vector<uint8_t>
toVec(T const& t)
{
    return std::vector<uint8_t>(xdr::xdr_to_opaque(t));
}

template <typename T>
CxxBuf
toCxxBuf(T const& t)
{
    return CxxBuf{std::make_unique<std::vector<uint8_t>>(toVec(t))};
}

InvokeHostFunctionOpFrame::InvokeHostFunctionOpFrame(Operation const& op,
                                                     OperationResult& res,
                                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mInvokeHostFunction(mOperation.body.invokeHostFunctionOp())
{
}

ThresholdLevel
InvokeHostFunctionOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
InvokeHostFunctionOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error("InvokeHostFunctionOpFrame::doApply needs Config");
}

void
InvokeHostFunctionOpFrame::maybePopulateDiagnosticEvents(
    Config const& cfg, InvokeHostFunctionOutput const& output)
{
    if (cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        OperationDiagnosticEvents diagnosticEvents;
        for (auto const& e : output.diagnostic_events)
        {
            DiagnosticEvent evt;
            xdr::xdr_from_opaque(e.data, evt);
            diagnosticEvents.events.emplace_back(evt);
        }
        mParentTx.pushDiagnosticEvents(diagnosticEvents);
    }
}

struct HostFunctionMetrics
{
    medida::MetricsRegistry& mMetrics;

    char const* mFnType{nullptr};

    size_t mReadEntry{0};
    size_t mWriteEntry{0};

    size_t mReadKeyByte{0};
    size_t mWriteKeyByte{0};

    size_t mReadDataByte{0};
    size_t mWriteDataByte{0};

    size_t mReadCodeByte{0};
    size_t mWriteCodeByte{0};

    size_t mEmitEvent{0};
    size_t mEmitEventByte{0};

    size_t mCpuInsn{0};
    size_t mMemByte{0};

    bool mSuccess{false};

    HostFunctionMetrics(medida::MetricsRegistry& metrics, HostFunctionType hf)
        : mMetrics(metrics)
    {
        switch (hf)
        {
        case HOST_FUNCTION_TYPE_INVOKE_CONTRACT:
            mFnType = "invoke-contract";
            break;
        case HOST_FUNCTION_TYPE_CREATE_CONTRACT:
            mFnType = "create-contract";
            break;
        case HOST_FUNCTION_TYPE_INSTALL_CONTRACT_CODE:
            mFnType = "install-contract-code";
            break;
        default:
            throw std::runtime_error("unknown host function type");
        }
    }

    bool
    isCodeKey(LedgerKey const& lk)
    {
        return lk.type() == CONTRACT_DATA &&
               lk.contractData().key.type() ==
                   SCValType::SCV_LEDGER_KEY_CONTRACT_EXECUTABLE;
    }

    void
    noteReadEntry(LedgerKey const& lk, size_t n)
    {
        mReadEntry++;
        mReadKeyByte += xdr::xdr_size(lk);
        if (isCodeKey(lk))
        {
            mReadCodeByte += n;
        }
        else
        {
            mReadDataByte += n;
        }
    }

    void
    noteWriteEntry(LedgerKey const& lk, size_t n)
    {
        mWriteEntry++;
        mWriteKeyByte += xdr::xdr_size(lk);
        if (isCodeKey(lk))
        {
            mReadCodeByte += n;
        }
        else
        {
            mReadDataByte += n;
        }
    }

    ~HostFunctionMetrics()
    {
        mMetrics.NewMeter({"host-fn", mFnType, "read-entry"}, "entry")
            .Mark(mReadEntry);
        mMetrics.NewMeter({"host-fn", mFnType, "write-entry"}, "entry")
            .Mark(mWriteEntry);

        mMetrics.NewMeter({"host-fn", mFnType, "read-key-byte"}, "byte")
            .Mark(mReadKeyByte);
        mMetrics.NewMeter({"host-fn", mFnType, "write-key-byte"}, "byte")
            .Mark(mWriteKeyByte);

        mMetrics.NewMeter({"host-fn", mFnType, "read-data-byte"}, "byte")
            .Mark(mReadDataByte);
        mMetrics.NewMeter({"host-fn", mFnType, "read-code-byte"}, "byte")
            .Mark(mReadCodeByte);

        mMetrics.NewMeter({"host-fn", mFnType, "write-data-byte"}, "byte")
            .Mark(mWriteDataByte);
        mMetrics.NewMeter({"host-fn", mFnType, "write-code-byte"}, "byte")
            .Mark(mWriteCodeByte);

        mMetrics.NewMeter({"host-fn", mFnType, "emit-event"}, "event")
            .Mark(mEmitEvent);
        mMetrics.NewMeter({"host-fn", mFnType, "emit-event-byte"}, "byte")
            .Mark(mEmitEventByte);

        mMetrics.NewMeter({"host-fn", mFnType, "cpu-insn"}, "insn")
            .Mark(mCpuInsn);
        mMetrics.NewMeter({"host-fn", mFnType, "mem-byte"}, "byte")
            .Mark(mMemByte);

        if (mSuccess)
        {
            mMetrics.NewMeter({"host-fn", mFnType, "success"}, "call").Mark();
        }
        else
        {
            mMetrics.NewMeter({"host-fn", mFnType, "failure"}, "call").Mark();
        }
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.NewTimer({"host-fn", mFnType, "exec"}).TimeScope();
    }
};

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx, Config const& cfg,
                                   medida::MetricsRegistry& metricsReg)
{
    HostFunctionMetrics metrics(metricsReg,
                                mInvokeHostFunction.function.type());

    uint32_t hostLogicVersion = getHostLogicVersion(ltx);

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;
    ledgerEntryCxxBufs.reserve(mInvokeHostFunction.footprint.readOnly.size() +
                               mInvokeHostFunction.footprint.readWrite.size());
    for (auto const& lk : mInvokeHostFunction.footprint.readOnly)
    {
        // Load without record for readOnly to avoid writing them later
        auto ltxe = ltx.loadWithoutRecord(lk);
        size_t nByte{0};
        if (ltxe)
        {
            auto buf = toCxxBuf(ltxe.current());
            nByte = buf.data->size();
            ledgerEntryCxxBufs.emplace_back(std::move(buf));
        }
        metrics.noteReadEntry(lk, nByte);
    }
    for (auto const& lk : mInvokeHostFunction.footprint.readWrite)
    {
        auto ltxe = ltx.load(lk);
        size_t nByte{0};
        if (ltxe)
        {
            auto buf = toCxxBuf(ltxe.current());
            nByte = buf.data->size();
            ledgerEntryCxxBufs.emplace_back(std::move(buf));
        }
        metrics.noteWriteEntry(lk, nByte);
    }

    rust::Vec<CxxBuf> contractAuthEntryCxxBufs;
    if (mInvokeHostFunction.function.type() ==
        HOST_FUNCTION_TYPE_INVOKE_CONTRACT)
    {
        contractAuthEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
        for (auto const& authEntry : mInvokeHostFunction.auth)
        {
            contractAuthEntryCxxBufs.push_back(toCxxBuf(authEntry));
        }
    }

    InvokeHostFunctionOutput out;
    try
    {
        auto timeScope = metrics.getExecTimer();
        out = rust_bridge::invoke_host_function(
            hostLogicVersion, cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS,
            toCxxBuf(mInvokeHostFunction.function),
            toCxxBuf(mInvokeHostFunction.footprint), toCxxBuf(getSourceID()),
            contractAuthEntryCxxBufs, getLedgerInfo(ltx, cfg),
            ledgerEntryCxxBufs);
        metrics.mSuccess = true;

        if (!out.success)
        {
            maybePopulateDiagnosticEvents(cfg, out);

            innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
            return false;
        }
    }
    catch (std::exception&)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
        return false;
    }

    metrics.mCpuInsn = out.cpu_insns;
    metrics.mMemByte = out.mem_bytes;

    // Create or update every entry returned
    std::unordered_set<LedgerKey> keys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.data, le);
        auto lk = LedgerEntryKey(le);

        auto ltxe = ltx.load(lk);
        if (ltxe)
        {
            ltxe.current() = le;
        }
        else
        {
            ltx.create(le);
        }

        keys.emplace(std::move(lk));
    }

    // Erase every entry not returned
    for (auto const& lk : mInvokeHostFunction.footprint.readWrite)
    {
        if (keys.find(lk) == keys.end())
        {
            auto ltxe = ltx.load(lk);
            if (ltxe)
            {
                ltx.erase(lk);
            }
        }
    }

    // Append events to the enclosing TransactionFrame, where
    // they'll be picked up and transferred to the TxMeta.
    OperationEvents events;
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        metrics.mEmitEventByte += buf.data.size();
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        events.events.push_back(evt);
    }
    mParentTx.pushContractEvents(events);

    maybePopulateDiagnosticEvents(cfg, out);

    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);
    xdr::xdr_from_opaque(out.result_value.data, innerResult().success());
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
}

void
InvokeHostFunctionOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
InvokeHostFunctionOpFrame::isSmartOperation() const
{
    return true;
}

}
#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
