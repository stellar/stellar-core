#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include <utility>

namespace stellar
{

std::pair<TransactionEnvelope, LedgerKey>
getWasmRestoreTx(PublicKey const& publicKey, SequenceNumber seqNum,
                 int64_t addResourceFee);

std::pair<TransactionEnvelope, LedgerKey>
getUploadTx(PublicKey const& publicKey, SequenceNumber seqNum,
            int64_t addResourceFee);

std::tuple<TransactionEnvelope, LedgerKey, Hash>
getCreateTx(PublicKey const& publicKey, LedgerKey const& contractCodeLedgerKey,
            std::string const& networkPassphrase, SequenceNumber seqNum,
            int64_t addResourceFee);

std::pair<TransactionEnvelope, ConfigUpgradeSetKey>
getInvokeTx(PublicKey const& publicKey, LedgerKey const& contractCodeLedgerKey,
            LedgerKey const& contractSourceRefLedgerKey, Hash const& contractID,
            ConfigUpgradeSet const& upgradeSet, SequenceNumber seqNum,
            int64_t addResourceFee);

}
