# Ledger query examples

These are some examples of using the Core's `dump-ledger` command.

Executing the query requires the node to have been caught up at some 
point in time.

## Dumping ledger entries

The following examples demonstrate how to dump parts of the current 
ledger to JSON files for further analysis.

* Dump entries modified in the 1000 most recent ledgers:

  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg --output-file q.json --last-ledgers 1000`

* Dump 1000 recently modified ledger entries (not necessarily the *most* recently modified):
  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg --output-file q.json --limit 1000`

* Dump all the ledger entries with provided account ID or trustline ID:

  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
   --output-file q.json --filter-query 
   "data.account.accountID == 'GDNG6SVZAJHCFCH65R7SQDLGVR6FDAR67M7YDHEESXKRRZYBWVF4BEC5' 
   || data.trustLine.accountID == 'GDNG6SVZAJHCFCH65R7SQDLGVR6FDAR67M7YDHEESXKRRZYBWVF4BEC5'" `

* Dump 1000 account entries that have non-empty `inflationDest` field:

  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
  --output-file q.json --filter-query "data.account.inflationDest != NULL" --limit 1000`

* Dump all the offer entries that trade lumens for any asset with code `'AABBG'` and have
  been modified within the last 1000 ledgers:
  
  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
   --output-file q.json --filter-query 
   "data.offer.selling == 'NATIVE' && data.offer.buying.assetCode == 'AABBG'"
   --last-ledgers 1000`

* Dump 100 trustline entries that have buying liabilities lower than selling liabilities:

  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
   --output-file q.json --filter-query 
   "data.trustLine.ext.v1.liabilities.buying < data.trustLine.ext.v1.liabilities.selling"
   --limit 100`

* Dump 100 account entries that fulfill a more complex filter (this just demonstrates
  that filter supports logical expressions):
  
  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
   --output-file q7.json --filter-query 
   "(data.account.balance < 100000000 || data.account.balance >= 2000000000) 
    && data.account.numSubEntries > 2" --limit 100`

* Output 10 entries larger than 200 bytes:
  
  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
   --output-file q8.json --filter-query "entry_size() > 200" --limit 10`

## Aggregating ledger entries

The following examples demonstrate how to aggregate parts of the ledger into CSV tables.

* Find the count of every ledger entry type starting from the certain ledger seq:

  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
   --output-file q.csv --filter-query "lastModifiedLedgerSeq >= 37872608" 
   --group-by "data.type" --agg "count()"`

* Dump the order book stats for the offers that have been modified during the last 
  100000 ledgers:

  `./stellar-core dump-ledger --conf ../stellar-core_pubnet.cfg 
  --output-file q.csv --filter-query "data.type == 'OFFER'" 
  --group-by "data.offer.selling, data.offer.selling.assetCode, 
  data.offer.selling.issuer, data.offer.buying, data.offer.buying.assetCode, 
  data.offer.buying.issuer" --agg "sum(data.offer.amount), avg(data.offer.amount), count()" 
  --last-ledgers 100000`

* Find the entry size distribution: 

  `./stellar-core dump-ledger --output-file entry_stats.json --group-by data.type --agg sum(entry_size()),avg(entry_size())`