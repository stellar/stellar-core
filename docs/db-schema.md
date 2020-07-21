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

Defined in [`src/ledger/LedgerHeaderUtils.cpp`](/src/ledger/LedgerHeaderUtils.cpp)

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

Defined in [`src/ledger/LedgerTxnAccountSQL.cpp`](/src/ledger/LedgerTxnAccountSQL.cpp)

Equivalent to _AccountEntry_

Field | Type | Description
------|------|---------------
accountid | VARCHAR(56)  PRIMARY KEY | (STRKEY)
balance | BIGINT NOT NULL CHECK (balance >= 0) |
seqnum | BIGINT NOT NULL |
numsubentries | INT NOT NULL CHECK (numsubentries >= 0) |
inflationdest | VARCHAR(56) | (STRKEY)
homedomain | VARCHAR(44) | (BASE64)
thresholds | TEXT | (BASE64)
flags | INT NOT NULL |
lastmodified | INT NOT NULL | lastModifiedLedgerSeq
extension | TEXT | Extension specific to AccountEntry (XDR)
ledgerext | TEXT | Extension common to all LedgerEntry types (XDR)
signers | TEXT | (XDR)

## offers

Defined in [`src/ledger/LedgerTxnOfferSQL.cpp`](/src/ledger/LedgerTxnOfferSQL.cpp)

Equivalent to _OfferEntry_

Field | Type | Description
------|------|---------------
sellerid | VARCHAR(56) NOT NULL | (STRKEY)
offerid | BIGINT NOT NULL CHECK (offerid >= 0) |
sellingasset | TEXT NOT NULL | selling (XDR)
buyingasset | TEXT NOT NULL | buying (XDR)
amount | BIGINT NOT NULL CHECK (amount >= 0) |
pricen | INT NOT NULL | Price.n
priced | INT NOT NULL | Price.d
price | DOUBLE PRECISION NOT NULL | computed price n/d, used for ordering offers
flags | INT NOT NULL |
lastmodified | INT NOT NULL | lastModifiedLedgerSeq
extension | TEXT | Extension specific to OfferEntry (XDR)
ledgerext | TEXT | Extension common to all LedgerEntry types (XDR)
(offerid) | PRIMARY KEY |

## trustlines

Defined in [`src/ledger/LedgerTxnTrustLineSQL.cpp`](/src/ledger/LedgerTxnTrustLineSQL.cpp)

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
extension | TEXT | Extension specific to TrustLineEntry (XDR)
ledgerext | TEXT | Extension common to all LedgerEntry types (XDR)
(accountid, issuer, assetcode) | PRIMARY KEY |

## accountdata

Defined in [`src/ledger/LedgerTxnDataSQL.cpp`](/src/ledger/LedgerTxnDataSQL.cpp)

Equivalent to _DataEntry_

Field | Type | Description
------|------|---------------
accountid | VARCHAR(56) NOT NULL | (STRKEY)
dataname | VARCHAR(88) NOT NULL | (BASE64)
datavalue | VARCHAR(112) NOT NULL | (BASE64)
lastmodified | INT NOT NULL | lastModifiedLedgerSeq
extension | TEXT | Extension specific to DataEntry (XDR)
ledgerext | TEXT | Extension common to all LedgerEntry types (XDR)
(accountid, dataname) | PRIMARY KEY |

## claimablebalance

Defined in [`src/ledger/LedgerTxnClaimableBalanceSQL.cpp`](/src/ledger/LedgerTxnClaimableBalanceSQL.cpp)

Equivalent to _ClaimableBalanceEntry_

Field | Type | Description
------|------|---------------
balanceid | VARCHAR(48) PRIMARY KEY | This is a ClaimableBalanceID (XDR)
ledgerentry | TEXT NOT NULL | LedgerEntry that contains a ClaimableBalanceEntry (XDR)
lastmodified | INT NOT NULL | lastModifiedLedgerSeq

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
(ledgerseq, txindex) | PRIMARY KEY |

## txfeehistory

Defined in [`src/transactions/TransactionFrame.cpp`](/src/transactions/TransactionFrame.cpp)

Field | Type | Description
------|------|---------------
txid | CHARACTER(64) NOT NULL | Hash of the transaction (excluding signatures) (HEX)
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this transaction got applied
txindex | INT NOT NULL | Apply order (per ledger, 1)
txchanges | TEXT NOT NULL | LedgerEntryChanges (XDR)
(ledgerseq, txindex) | PRIMARY KEY |

## scphistory

Defined in [`src/herder/HerderPersistenceImpl.cpp`](/src/herder/HerderPersistenceImpl.cpp)

Field | Type | Description
------|------|---------------
nodeid | CHARACTER(56) NOT NULL | (STRKEY)
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this transaction got applied
envelope | TEXT NOT NULL | (XDR)

## scpquorums

Defined in [`src/herder/HerderPersistenceImpl.cpp`](/src/herder/HerderPersistenceImpl.cpp)

Field | Type | Description
------|------|---------------
qsethash | CHARACTER(64) NOT NULL | hash of quorum set (HEX)
lastledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this quorum set was last seen
qset | TEXT NOT NULL | (XDR)
(qsethash) | PRIMARY KEY |

## quoruminfo

Defined in [`src/herder/HerderPersistenceImpl.cpp`](/src/herder/HerderPersistenceImpl.cpp)

Field | Type | Description
------|------|---------------
nodeid | CHARACTER(56) NOT NULL | (STRKEY)
qsethash | CHARACTER(64) NOT NULL | hash of quorum set (HEX)
(nodeid) | PRIMARY KEY |

## storestate

Defined in [`src/main/PersistentState.cpp`](/src/main/PersistentState.cpp)

Field | Type | Description
------|------|---------------
statename | CHARACTER(32) PRIMARY KEY | Key
state | TEXT | Value

## peers

Defined in [`src/overlay/PeerManager.cpp`](/src/overlay/PeerManager.cpp)

Field | Type | Description
------|------|---------------
ip | VARCHAR(15) NOT NULL |
port | INT DEFAULT 0 CHECK (port > 0 AND port <= 65535) NOT NULL |
nextattempt | TIMESTAMP NOT NULL |
numfailures | INT DEFAULT 0 CHECK (numfailures >= 0) NOT NULL |
type | INT NOT NULL |
(ip, port) | PRIMARY KEY |

## upgradehistory

Defined in [`src/herder/Upgrades.cpp`](/src/herder/Upgrades.cpp)

Field | Type | Description
------|------|---------------
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) | Ledger this upgrade got applied
upgradeindex | INT NOT NULL | Apply order (per ledger, 1)
upgrade | TEXT NOT NULL | The upgrade (XDR)
changes | TEXT NOT NULL | LedgerEntryChanges (XDR)
(ledgerseq, upgradeindex) | PRIMARY KEY |

## ban

Defined in [`src/overlay/BanManagerImpl.cpp`](/src/overlay/BanManagerImpl.cpp)

Field | Type | Description
------|------|---------------
nodeid | CHARACTER(56) NOT NULL PRIMARY KEY |

## publishqueue

Defined in [`src/history/HistoryManagerImpl.cpp`](/src/history/HistoryManagerImpl.cpp)

Field | Type | Description
------|------|---------------
ledger | INTEGER PRIMARY KEY |
state | TEXT |

## pubsub

Defined in [`src/main/ExternalQueue.cpp`](/src/main/ExternalQueue.cpp)

Field | Type | Description
------|------|---------------
resid | CHARACTER(32) PRIMARY KEY |
lastread | INTEGER |
