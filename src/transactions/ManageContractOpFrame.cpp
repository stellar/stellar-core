// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageContractOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <Tracy.hpp>

namespace stellar
{

using namespace std;

ManageContractOpFrame::ManageContractOpFrame(Operation const& op,
                                             OperationResult& res,
                                             TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mManageContract(mOperation.body.manageContractOp())
{
}

bool
ManageContractOpFrame::doApply(AbstractLedgerTxn& ltx)
{

    ZoneNamedN(applyZone, "ManageContractOp apply", true);
    auto header = ltx.loadHeader();

    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_20))
    {
        throw std::runtime_error(
            "MANAGE_CONTRACT not supported before ledger version 20");
    }

    auto codeLTE = stellar::loadContractCode(ltx, getSourceID(),
                                             mManageContract.contractID);
    if (mManageContract.body)
    {
        // For now we only support WASM.
        releaseAssert(mManageContract.body->type() == CONTRACT_CODE_WASM);

        if (!codeLTE)
        { // create a new contract code entry
            LedgerEntry newCode;
            newCode.data.type(CONTRACT_CODE);
            auto& codeEntry = newCode.data.contractCode();
            codeEntry.owner = getSourceID();
            codeEntry.contractID = mManageContract.contractID;

            codeEntry.body.type(CONTRACT_CODE_WASM);
            codeEntry.body.wasm() = mManageContract.body->wasm();

            auto sourceAccount = loadSourceAccount(ltx, header);
            switch (createEntryWithPossibleSponsorship(ltx, header, newCode,
                                                       sourceAccount))
            {
            case SponsorshipResult::SUCCESS:
                break;
            case SponsorshipResult::LOW_RESERVE:
                innerResult().code(MANAGE_CONTRACT_LOW_RESERVE);
                return false;
            case SponsorshipResult::TOO_MANY_SUBENTRIES:
                mResult.code(opTOO_MANY_SUBENTRIES);
                return false;
            case SponsorshipResult::TOO_MANY_SPONSORING:
                mResult.code(opTOO_MANY_SPONSORING);
                return false;
            case SponsorshipResult::TOO_MANY_SPONSORED:
                // This is impossible right now because there is a limit on sub
                // entries, fall through and throw
            default:
                throw std::runtime_error("Unexpected result from "
                                         "createEntryWithPossibleSponsorship");
            }
            ltx.create(newCode);
        }
        else
        { // modify an existing entry
            releaseAssert(codeLTE.current().data.contractCode().body.type() ==
                          CONTRACT_CODE_WASM);
            codeLTE.current().data.contractCode().body.wasm() =
                mManageContract.body->wasm();
        }
    }
    else
    { // delete an existing contract code entry
        if (!codeLTE)
        {
            innerResult().code(MANAGE_CONTRACT_NOT_FOUND);
            return false;
        }

        auto sourceAccount = loadSourceAccount(ltx, header);
        removeEntryWithPossibleSponsorship(ltx, header, codeLTE.current(),
                                           sourceAccount);
        codeLTE.erase();
    }

    innerResult().code(MANAGE_CONTRACT_SUCCESS);
    return true;
}

bool
ManageContractOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_20))
    {
        innerResult().code(MANAGE_CONTRACT_NOT_SUPPORTED_YET);
        return false;
    }

    return true;
}

void
ManageContractOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(contractCodeKey(getSourceID(), mManageContract.contractID));
}
}
