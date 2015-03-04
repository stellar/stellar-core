OPEN QUESTIONS
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


If we id txs by hash of contents then there are multiple txs that could match
 maybe same contents were submitted too early and failed without consuming the seq num
 maybe two different sets of signers submitted the same contents 
 but
 If we id by hash(contents + sig) we have similar problems
 I think we have to id by account:seqnum that is garaunteed to be a unique tx
 but we still need a way to refer to txs that didn't claim a seq num? 



ANSWERED
=======
- What do we do about txs with extra signatures?
	- We don't really want to enforce or check if there are extra signatures. Seems like an easy mistake to make.
	- But we don't want to allow a crap load of useless signatures.
Do we keep full entries in the BucketList?
What is the file format for the history?
What do we do about tx that make it into the applied txset but have too low a max fee? 
	Do we consume the seq num?
	Do we charge the max fee and not apply them? 
	Do we just ignore them?
	Do we let them get by for free?

How do we want to represent the transaction results?
	I think we need to calculate this and make it available to be stored for clients
	The data is pretty unstructured so maybe we stick it in json?
	Entries and new values

We need to deal with someone submitting a ton of tx for one ledger when they can only pay fee for one tx.
	Take fees before any other txs

Should we make entry indexes just be a sequential number rather than do this hashing to calculate it?
	Entry indexes could be 64bit
	This will have a bunch of space everywhere. 
		Issuers would only be 64 bit
		Offers are smaller
		tx would only need 64bit for destination and source
	Downsides:
		Creating an account would have to be a special kind of tx
		Easier to accidentally send to the wrong person?
		Makes it more complicated to go from secret key to account
		We of course need to be absolutely sure people securely learn the Ledger IDs corresponding to created accounts.

AccountEntry.ownerCount  do we want this in the ledgerEntry? 
	It is calculateable

Do we allow alternate paths?
	I don't think it is worth the protocol complexity. Clients can just retry if it fails.

Do we check seq numbers when we check validity?
	Yes probably better otherwise if someone signs a tx with a future seq num it can be resubimtted and million times and the fee can drain the accounts balance. 

Do we want a separate base fee for the reserve? or should it be tied to the amount we charge for tx fee?
	Maybe changing the base reserve is the same process for approving protocol upgrades?



How do we want to handle returning payments?
	If a user sends a payment from a gateway and someone needs to return it 
		they can't just send it back since the gateway 
		needs to know who to credit the payment to. 
	source memo that is echod if there is a return payment?
	include the tx id in the return payment?

How do we represnt currencies?
	A client should be able to see the code and know if it was 
		something simple like "USD" or "BTC" without doing some lookup
	We should be able to represent more complex things that are interpreted 
		by clients if we want to do demerge or something.
	Do we want to allow longer human readable currency codes?
	options:
		bit packed code ala ripple
		union of string or hash we look up