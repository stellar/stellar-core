#Left to design:
- Bucket list
- memo in txs
- seq numbers 
	- top byte is the slot so we have 256 slots 
- what to do about inner ledger arbitrage?
	- maybe solved by validators delaying what look to be arbitrage transactions
- should we make payments that go through the same orderbook in the same ledger cross each other?
	- this will make things way more complicated but could potentially increase the liquidity a lot.
- Is this really the way we want to do paths/offers?
	- what if we allow you to specify many paths in a given tx and the path includes the destination so it essentially makes an arbitray set of txs atomic
	- def seems like there is some way we can break this stuff up a bit differently  
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




