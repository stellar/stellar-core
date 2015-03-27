# Herder

The Herder is a concrete implementation of the SCP protocol, operating in terms
of the "transaction sets" and "ledger numbers" that constitute the stellar
vocabulary. It is implemented as a subclass of the [SCP class](src/scp/SCP.h),
and so is most easily understood after reading that class and understanding
where and how a subclass would make the abstract SCP protocol concrete.

Specifically, the Herder considers a ledger number to be a "slot" in the SCP
protocol, and transaction set hashes (along with close-time and base-fee) to be
the sort of "value" that it is attempting to agree on for each "slot".
