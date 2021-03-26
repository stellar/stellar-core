# LEIGHNOTES

My notes of useful commands and other things while hacking on stellar-core.

## Standalone cfg

The stellar-core.cfg file contains a standalone network config.

NOTE: The account minimum reserve is 20XLM.

## Useful commands

### First compile

./autogen.sh
./configure --enable-next-protocol-version-unsafe-for-production
time (make -j24)

### Run all the tests in parallel

rm -fr /tmp/pgtmp.* ; time (make check -j24 ALL_VERSIONS=1 NUM_PARTITIONS=24 BATCHSIZE=15)

### Run all the tests in parallel

rm -fr /tmp/pgtmp.* ; time (make check -j24 ALL_VERSIONS=1 NUM_PARTITIONS=24 BATCHSIZE=15 TEST_SPEC='txenvelope')

### Run a single test case or tests in serial

rm -fr /tmp/pgtmp.* ; time (make -j24) && time (src/stellar-core test 'txenvelope')

### Setup DB

src/stellar-core new-db

### Run stellar-core

src/stellar-core run

### Upgrade to protocol 16

curl 'http://localhost:11626/upgrades?mode=set&upgradetime=1970-01-01T00:00:00Z&protocolversion=16'

### Submit tx

curl -G localhost:11626/tx --data-urlencode "blob=$(stc -c <stc-tx-file>)"

### Test account details

#### Ledger Root
GBZXN7PIRZGNMHGA7MUUUF4GWPY5AYPV6LY4UV2GL6VJGIQRXFDNMADI
SC5O7VZUXDJ6JBDSZ74DSERXL7W3Y5LTOAMRF7RQRL3TAGAPS7LUVG3L

#### Initiator
GCOBXOFPZXUEBZK4QS5TKSXN6X2YBM3YHBZW6PQW5KJ73K3GFXL7KJHA
SAGANSXQOLF434C3HZ75IL3QW3WVXL6P7NGHDCYFWCRDRZ6O72YJXAHD

#### Responder
GD7ZSZ7G2P3N3JOH5C2CIIHNJWJOD6OMT7R542AC4MW2CY5OM47UWUZG
SCSDLYC5NRXLZAH2ELSDKJFY4YSNDKCOBYUAZY753GHTESLVO5JK3JS2

#### Channel
GBK34BGFU2HBRUF53HGPV5ICFYJMDMOAUHNUIPZT5C7FTISQGQRHCRYU
SDMPC36TG7ADLOPDHA43R3DRR4AEGTPWHXCA6UAAPBB5KMQH4ULFXCQT
