# LedgerDelta

LedgerDelta represents all side effects that occured while applying
transactions. It keeps tracks of creation, modification and deletion of Ledger
Entries as well as changes to the ledgerHeader.

LedgerDelta is also a view for changed ledger entries. Each time a change to
a ledger entry is to be made, it should be loaded from LedgerDelta first. This
makes updating global entry cache (LedgerEntries class) easy as it only needs
to be done at the end of ledger close state (database is updated constantly as
some queries - inflation and offer - require database to be up-to-date and
database changes are easily rollbacked, unlikely cache changes).

LedgerDelta is in fact a stack of LedgerDeltaLayer instances, each contains
changes on different level: for whole transaction set, for one transaction, for
one operation. This allows for deciding which subset of changes to include or
not in the final set of changes that will be commited to the ledger. It also
allows for reporting set of changes in results of transaction (for example,
each operation has its own set of changes that must be reported back).

In future LedgerDeltaLayer should be tightly coupled with soci transaction, so
when one gets rollback, second one should too.

LedgerDelta always contains at least one LedgerDeltaLayer instance. When it
contains exactly one, its value of bool isCollapsed() is true. Only collapsed
deltas can be applied to an ledger as non-collapsed values can only be result
of serious bugs in code.

LedgerDeltaScope is a RAII class used to manage LedgerDeltaLayer for
LedgerDelta. It has friend access to LedgerDelta members like newDelta(),
applyTop() and rollbackTop(). When a LedgerDeltaScope is introduced into a
scope, a new LedgerDeltaLayer is created on top of LedgerDelta stack. It has
empty set of changes. By default when going out of scope LedgerDeltaScope
performs rollback of its changes. To commit changes to next LedgerDeltaLayer on
stack a commit() method must be called.

This design makes several kinds of bugs that could appear in previous
implementation impossible:
* creating two child LedgerDelta from one parent at the same time
* changing values in non-leaf (non-top) LedgerDelta
* leaving non-commited and non-rolled-back LedgerDelta somewhere in memory

It also simplifies handling of cache:
* no updates to global cache is done until LedgerDelta is applied to ledger
  so no flushing of cache is required
