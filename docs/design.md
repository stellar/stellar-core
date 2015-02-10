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
- Is there a way to have payments be atomic units and express everything in terms of a batch of a smaller payments?
	- Taking an offer would just be A paying B followed by B paying A
- we need some indicator of if a tx is successful in the ledger header. Otherwise people have to either replay the tx or trust someone to know if the payment worked
	- Not sure we need this


#v.2 code public
- Syncing to the network (graydon)
- storing history (graydon)
- bucket list (graydon)
- publish FBA (david)
- Replaying transactions to catch up (graydon)
  
#v.3 beta
- End to end tests, replaying a known set of tx 
- inflation tx (nicolas)

#v.4 switch to main network
- fuzzer
- stress test
- deal with bad acting peers (jed)
- store validations in the DB

#v.5 before scale
- tx set reconciliation. Do something smarter than send the whole list


Things we know we probably want to add later but design earlier than later
to avoid churn on the protocol front:
- some common place to look up tx memos, multisig txs, etc
- private transactions
- scripting


#Later things
- make a int128_t and use that for the math 
- can we drop the idea of transaction rate?




