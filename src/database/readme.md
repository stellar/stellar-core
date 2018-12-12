# Database

The [Database](Database.h) object manages connections to an SQL database, in
which the current ledger state and several auxiliary tables are stored. The
database object also caches prepared statements, and holds some timers for
tracking query performance.

Database connections are configured by the config variable DATABASE, see
[src/main/Config.h](../main/Config.h)

The connections and statements are of the types provided by the
[SOCI database access library](http://soci.sourceforge.net/), a copy of which
is contained in the [lib/soci](../lib/soci) subdirectory of the
`stellar-core` source tree and built along with it.
