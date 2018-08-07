---
title: DB Schema
---

stellar-core maintains the current state of the ledger in a SQL DB. Currently
it can be configured to use either sqlite or postgres.

This database is the main way a dependent service such as Horizon can gather information on the current ledger state or transaction history.

Most objects are the straight representation of the equivalent XDR object.
See [`src/ledger/readme.md`](/src/ledger/readme.md) for a detailed description of those.

Types used in the tables:

Type Name | Description
--------- | -----------
HEX | Hex encoded binary blob
BASE64 | Base 64 encoded binary blob
XDR | Base 64 encoded object serialized in XDR form
STRKEY | Custom encoding for public/private keys. See [`src/crypto/readme.md`](/src/crypto/readme.md)

## ledgerheaders

Defined in [`src/ledger/LedgerHeaderFrame.cpp`](/src/ledger/LedgerHeaderFrame.cpp)

Equivalent to _LedgerHeader_

Field | Type | Description
------|------|---------------
ledgerhash | CHARACTER(64) PRIMARY KEY | Hash of the ledger header (HEX)
prevhash | CHARACTER(64) NOT NULL | previousLedgerHash (HEX)
bucketlisthash | CHARACTER(64) NOT NULL | (HEX)
ledgerseq | INT UNIQUE CHECK (ledgerseq >= 0) |
closetime | BIGINT NOT NULL CHECK (closetime >= 0) | scpValue.closeTime
data | TEXT NOT NULL | Entire LedgerHeader (XDR)


## accounts

Defined in [`src/ledger/AccountFrame.cpp`](/src/ledger/AccountFrame.cpp)

Equivalent to _AccountEntry_

Field | Type | Description
------|------|---------------
accountid | VARCHAR(56)  PRIMARY KEY | (STRKEY)
balance | BIGINT NOT NULL CHECK (balance >= 0) |
seqnum | BIGINT NOT NULL |
numsubentries | INT NOT NULL CHECK (numsubentries >= 0) |
inflationdest | VARCHAR(56) | (STRKEY)
homedomain | VARCHAR(32) |
thresholds | TEXT | (BASE64)
flags | INT NOT NULL |
lastmodified | INT NOT NULL | lastModifiedLedgerSeq
buyingliabilities | BIGINT CHECK (buyingliabilities >= 0)
sellingliabilities | BIGINT CHECK (sellingliabilities >= 0)

## offers

Defined in [`src/ledger/OfferFrame.cpp`](/src/ledger/OfferFrame.cpp)

Equivalent to _OfferEntry_

Field | Type | Description
------|------|---------------
sellerid | VARCHAR(56) NOT NULL | (STRKEY)
offerid | BIGINT NOT NULL CHECK (offerid >= 0) |
sellingassettype | INT | selling.type
sellingassetcode | VARCHAR(12) | selling.*.assetCode
sellingissuer | VARCHAR(56) | selling.*.issuer
buyingassettype | INT | buying.type
buyingassetcode | VARCHAR(12) | buying.*.assetCode
buyingissuer | VARCHAR(56) | buying.*.issuer
amount | BIGINT NOT NULL CHECK (amount >= 0) |
pricen | INT NOT NULL | Price.n
priced | INT NOT NULL | Price.d
price | DOUBLE PRECISION NOT NULL | computed price n/d, used for ordering offers
flags | INT NOT NULL |
lastmodified | INT NOT NULL | lastModifiedLedgerSeq


## trustlines

Defined in [`src/ledger/TrustFrame.cpp`](/src/ledger/TrustFrame.cpp)

Equivalent to _TrustLineEntry_

Field | Type | Description
------|------|---------------
accountid | VARCHAR(56) NOT NULL | (STRKEY)
assettype | INT NOT NULL | asset.type
issuer | VARCHAR(56) NOT NULL | asset.*.issuer
assetcode | VARCHAR(12) NOT NULL | asset.*.assetCode
tlimit | BIGINT NOT NULL DEFAULT 0 CHECK (tlimit >= 0) | limit
balance | BIGINT NOT NULL DEFAULT 0 CHECK (balance >= 0) |
flags | INT NOT NULL |
lastmodified | INT NOT NULL | lastModifiedLedgerSeq
buyingliabilities | BIGINT CHECK (buyingliabilities >= 0)
sellingliabilities | BIGINT CHECK (sellingliabilities >= 0)


## txhistory

Defined in [`src/transactions/TransactionFrame.cpp`](/src/transactions/TransactionFrame.cpp)

Field | Type | Description
------|------|---------------
txid | CHARACTER(64) NOT NULL | Hash of the transaction (excluding signatures) (HEX)
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this transaction got applied
txindex | INT NOT NULL | Apply order (per ledger, 1)
txbody | TEXT NOT NULL | TransactionEnvelope (XDR)
txresult | TEXT NOT NULL | TransactionResultPair (XDR)
txmeta | TEXT NOT NULL | TransactionMeta (XDR)

## txfeehistory

Defined in [`src/transactions/TransactionFrame.cpp`](/src/transactions/TransactionFrame.cpp)

Field | Type | Description
------|------|---------------
txid | CHARACTER(64) NOT NULL | Hash of the transaction (excluding signatures) (HEX)
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this transaction got applied
txindex | INT NOT NULL | Apply order (per ledger, 1)
txchanges | TEXT NOT NULL | LedgerEntryChanges (XDR)

## scphistory
Field | Type | Description
------|------|---------------
nodeid | CHARACTER(56) NOT NULL | (STRKEY)
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this transaction got applied
envelope | TEXT NOT NULL | (XDR)

## scpquorums
Field | Type | Description
------|------|---------------
qsethash | CHARACTER(64) NOT NULL | hash of quorum set (HEX)
lastledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this quorum set was last seen
qset | TEXT NOT NULL | (XDR)


## storestate

Defined in [`src/main/PersistantState.cpp`](/src/main/PersistantState.cpp)

Field | Type | Description
------|------|---------------
statename | CHARACTER(32) PRIMARY KEY | Key
state | TEXT | Value


## peers

Defined in [`src/overlay/PeerRecord.cpp`](/src/overlay/PeerRecord.cpp)

Field | Type | Description
------|------|---------------
ip | VARCHAR(15) NOT NULL |
port | INT DEFAULT 0 CHECK (port > 0 AND port <= 65535) NOT NULL |
nextattempt | TIMESTAMP NOT NULL |
numfailures | INT DEFAULT 0 CHECK (numfailures >= 0) NOT NULL |


## upgradehistory

Defined in [`src/herder/Upgrades.cpp`](/src/herder/Upgrades.cpp)

Field | Type | Description
------|------|---------------
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this upgrade got applied
upgradeindex | INT NOT NULL | Apply order (per ledger, 1)
upgrade | TEXT NOT NULL | The upgrade (XDR)
changes | TEXT NOT NULL | LedgerEntryChanges (XDR)

