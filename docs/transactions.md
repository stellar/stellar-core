
Anything that changes the ledger is a Transaction:

All transactions have:
	Source Account
	Max Fee
	Sequence


Supported Transactions:

Payment
	Destination
	Amount
	Path
	Meta Info

Create Offer
Cancel Offer
AdjustAccount
AccountMerge
SetTrust
Inflation


Later:
StoreData
RemoveData


#Rules for what txs are not flooded
- malformed
- sum signature weight not above the necessary threshold
- source account doesn't have enough balance to pay the fee for all the txs in this set
- seq num is too low
- *Note: we still flood seq numbers that are the same as another in this tx set since we don't want to have a situation where you flood or don't flood depending on the order you recieved a tx.*



