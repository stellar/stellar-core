#Left to design:
- Bucket list
- Is this really the way we want to do paths/offers?
- we need some indicator of if a tx is successful in the ledger header. Otherwise people have to either replay the tx or trust someone to know if the payment worked
 - Not sure we need this


#Left to do:
- items blocking function
 - Tests for transactions (nicolas)
 - Syncing to the network (graydon)
 - Replaying transactions to catch up (graydon)
 - storing history (graydon)
 - bucket list (graydon)
 - end to end test
- non blocking items
 - saving peers in the DB (jed)
 - sending round lists of peers (jed)
 - deal with bad acting peers (jed)
 - stress test
 - End to end tests, replaying a known set of tx  
 - inflation tx (nicolas)
 - store validations in the DB
 - tx set reconciliation
 - fuzzer



Things we know we probably want to add later but design earlier than later
to avoid churn on the protocol front:
- some common place to look up tx memos, multisig txs, etc
- private transactions
- scripting


#Later things
- make a int128_t and use that for the math 





