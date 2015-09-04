
stellar-core maintains the current state of the ledger in a SQL DB. Currently
it can be configured to use either sqlite or postgres. 

Here are the tables used by stellar-core:


## ledgerheaders
Field | Type | Description
------|------|---------------
ledgerhash | CHARACTER(64) PRIMARY KEY | 
prevhash | CHARACTER(64) NOT NULL | 
bucketlisthash | CHARACTER(64) NOT NULL | 
ledgerseq | INT UNIQUE CHECK (ledgerseq >= 0) | 
closetime | BIGINT NOT NULL CHECK (closetime >= 0) |
data | TEXT NOT NULL | 


## accounts
Field | Type | Description
------|------|---------------
accountid | VARCHAR(56)  PRIMARY KEY | 
balance | BIGINT NOT NULL CHECK (balance >= 0) |
seqnum | BIGINT NOT NULL |
numsubentries | INT NOT NULL CHECK (numsubentries >= 0) |
inflationdest | VARCHAR(56) | 
homedomain | VARCHAR(32) | 
thresholds | TEXT | 
flags | INT NOT NULL | 
lastmodified | INT NOT NULL | 

## offers
Field | Type | Description
------|------|---------------
sellerid | VARCHAR(56) NOT NULL |
offerid | BIGINT NOT NULL CHECK (offerid >= 0) |
sellingassettype | INT |
sellingassetcode | VARCHAR(12) |
sellingissuer | VARCHAR(56) |
buyingassettype | INT |
buyingassetcode | VARCHAR(12) |
buyingissuer | VARCHAR(56) |
amount | BIGINT NOT NULL CHECK (amount >= 0) |
pricen | INT NOT NULL |
priced | INT NOT NULL |
price | BIGINT NOT NULL |
flags | INT NOT NULL |
lastmodified | INT NOT NULL |
-- | PRIMARY KEY (offerid) | 


## trustlines
Field | Type | Description
------|------|---------------
accountid | VARCHAR(56) NOT NULL |
assettype | INT NOT NULL |
issuer | VARCHAR(56) NOT NULL |
assetcode | VARCHAR(12) NOT NULL |
tlimit | BIGINT NOT NULL DEFAULT 0 CHECK (tlimit >= 0) |
balance | BIGINT NOT NULL DEFAULT 0 CHECK (balance >= 0) |
flags | INT NOT NULL |
lastmodified | INT NOT NULL |
-- | PRIMARY KEY  (accountid, issuer, assetcode) |


## txhistory
Field | Type | Description
------|------|---------------
txid | CHARACTER(64) NOT NULL |
ledgerseq | INT NOT NULL CHECK (ledgerseq >= 0) |
txindex | INT NOT NULL |
txbody | TEXT NOT NULL |
txresult | TEXT NOT NULL |
txmeta | TEXT NOT NULL |
-- | PRIMARY KEY (txid, ledgerseq) |


## storestate
Field | Type | Description
------|------|---------------
statename | CHARACTER(32) PRIMARY KEY | Key
state | TEXT | Value


## peers
Field | Type | Description
------|------|---------------
ip | VARCHAR(15) NOT NULL | 
port | INT DEFAULT 0 CHECK (port > 0 AND port <= 65535) NOT NULL | 
nextattempt | TIMESTAMP NOT NULL | 
numfailures | INT DEFAULT 0 CHECK (numfailures >= 0) NOT NULL | 
rank | INT DEFAULT 0 CHECK (rank >= 0) NOT NULL | 
-- | PRIMARY KEY (ip, port) | 

