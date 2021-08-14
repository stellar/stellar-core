#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;
class AccountCommutativityRequirements;

class CreateSpeedexIOCOfferOpFrame : public OperationFrame
{
    CreateSpeedexIOCOfferResult&
    innerResult()
    {
        return mResult.tr().createSpeedexIOCOfferResult();
    }
    CreateSpeedexIOCOfferOp const& mCreateSpeedexIOCOffer;

  public:
    CreateSpeedexIOCOfferOpFrame(Operation const& op, OperationResult& res,
                   TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    bool doAddCommutativityRequirements(AbstractLedgerTxn& ltx, AccountCommutativityRequirements& reqs) const override;
    
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static CreateSpeedexIOCOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createSpeedexIOCOfferResult().code();
    }
};
}
