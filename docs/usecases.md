Ideally the stellar network will be flexible enough so people can do a lot of things we don't anticipate at first

tokens that aren't fungible that can be traded around
tokens that are unique and aren't fungible that can be traded around
the ability to issue currencies with a fixed supply
Someway to store things and look them up by hash. For:
	currency code descriptions
	token descriptions
	transaction memos


Things we might want to handle with payments:
- converting from one currency to another
- finding the best rate from several of your currencies. USD from A and USD from B. You don't care which you send you just want to send the least.
- or USD A part of it goes through one path and part goes through another path B gets the same amount you just pay less
- create two offers at the same time
- send a payment when you don't have enough of one source currency


Maybe there is a way to bundle some arbitrary set of txs into an atomic package.
This could allow you to do cool things like I send you X if you send me Y. They either both succeed or both fail
Issues:
	- more complex
	- more computation. 
	- will probably need to charge a higher fee

	


ability to do debits
invoices
recurring payments or subscriptions
passing around titles to things
oracles where contracts can look things up

bitcoin bridge
SEPA bridge (developer.fidor.de/API-Browser)
ACH bridge



