---
id: design
title: Design
category: Documents
---
#Open Questions
==============
- How are we going to handle Tx Memos?
	- we allow a 32byte memo field that can be attached to a tx
	- This can be interpreted as a hash and there is some way to look up the contents
	- Maybe accounts have a some value stored that tells people where to look for the memo contents

- How are we going to do reverse federation?


- Is it ok to drop the transfer fee?
	- Do we want to make a maxTransferRate for moving credit around?


- Are we charging enough for storage?
	Right now you can make a few million offers and greatly increase
		the ledger size and get it all back at somepoint
	Maybe it should be more than a bond. Like some part is destroyed permanently.

- Are we charging enough for tx?

- How do we boot a network when most or all the nodes have cashed?

- should we make payments that go through the same orderbook in the same ledger cross each other?
	- this will make things way more complicated but could potentially increase the liquidity a lot.
- freezing credit/compliance


If we id txs by hash of contents then there are multiple txs that could match
 maybe same contents were submitted too early and failed without consuming the seq num
 maybe two different sets of signers submitted the same contents
 but
 If we id by hash(contents + sig) we have similar problems
 I think we have to id by account:seqnum that is garaunteed to be a unique tx
 but we still need a way to refer to txs that didn't claim a seq num?


#Bigger Pieces
================
Things we know we probably want to add later but design earlier than later
to avoid churn on the protocol front:
- some common place to look up tx memos, multisig txs, etc
- private transactions
- scripting







