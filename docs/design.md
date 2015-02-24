#Left to design:
- Bucket list
- memo in txs
- seq numbers 
	- How do we prevent replaying the same tx multiple times
	- ideally we would be able to create a tx that can be submitted to the network much later
	- We don't want to have to worry about the order of some txs
	- top byte is the slot so we have 256 slots 
		- this breaks since then they all have to be tracked by the account
	- variable number of slots that are just tracked in the account
	- keep a record of all tx around for each account
	- create another ledgerEntry that basically holds a tx's place for some amount of time till it can be submitted
	- another ledgerentry that gives you extra slots for an account
	- maybe an account has a certain number of tokens that a tx can consume. these are just a bit field. The tokens get returned when the consuming tx can no longer be replayed (its maxledger has passed)
	- maybe makes seq# optional and if not there the tx should be created so that it kills the account it is from.
- what to do about inner ledger arbitrage?
	- maybe solved by validators delaying what look to be arbitrage transactions
- should we make payments that go through the same orderbook in the same ledger cross each other?
	- this will make things way more complicated but could potentially increase the liquidity a lot.
- Is this really the way we want to do paths/offers?
	- what if we allow you to specify many paths in a given tx and the path includes the destination so it essentially makes an arbitray set of txs atomic
	- def seems like there is some way we can break this stuff up a bit differently  
	- transaction has a set of operations. The transaction can 
- Is there a way to have payments be atomic units and express everything in terms of a batch of a smaller payments?
	- Taking an offer would just be A paying B followed by B paying A
- we need some indicator of if a tx is successful in the ledger header. Otherwise people have to either replay the tx or trust someone to know if the payment worked
	- Not sure we need this
- how do we want to treat duplicate entries in the quorum set?



Things we know we probably want to add later but design earlier than later
to avoid churn on the protocol front:
- some common place to look up tx memos, multisig txs, etc
- private transactions
- scripting


#Later things
- make a int128_t and use that for the math 
- can we drop the idea of transaction rate?




