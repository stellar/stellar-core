#!/bin/bash

# This is a basic 3-node test that runs locally and uses 'cp' to write to
# archives and connects the peers to one another through TCP/localhost. It
# starts two nodes in sync with --forcescp then a third after a 30 second delay
# without --forcescp; the test (which is not automatically checked, but you can
# watch) is that the third node eventually syncs and joins consensus after the
# history publication bounces off the archive.
#
# It requires multitail and realpath installed

set -x

echo "killing all nodes"
killall -9 stellar-core

ST=$(realpath ../../bin/stellar-core)

VSEC0=sfSXhEWL4YLthoeJL7GUYz7MNu7KM5TkQUgETmg5SQPWy94i5XZ
VPUB0=gqViD3GvzLHY5sjtieSqzLtuJMXaNrBHMpqiD6DLAFCxSLxFiB

VSEC1=s3JyNSXUrqfwmfWaKXNR8ZrtC23nDeP9NCcVBJZKt1QresBubYe
VPUB1=gsh2GRrnEy5bNpmfBDPaVbuygkwqPXWdtmxMMz6oPv2GvxRpsWH

VSEC2=sfXi3coELM5Lfaod4UZqppxXfxnxgQpsqsr5dgU5JPPFDVFqDx6
VPUB2=gsHW1obJFtWZj45Vqf8mPGLyyU4DrrWW5JNi24NsGcBzR3CJx9Y

NSEC0=s3HM7aRMr6wEgYCQJo7V3iN8Ck7nP1ZxVECnTrCHSHCM2o5zzW2
NPUB0=g4Fd2Ai6jP395fLMZQ3gzwQLRGF3SBdy8Jgp8hTRmCA7X1yp7R

NSEC1=s3hUenb8TWaVpRpJek5Go64n1UE8p3iNf1idjHi6Adid4AU9TVM
NPUB1=gs2GfogJPA8cksDftdLzUChXdL2bjxyppMinfnterAPkRAzWtgq

NSEC2=s36idnC29ty9xMArD9ryVv4jQMh724K3nMhnavGtwD24mL9KhRG
NPUB2=gsqfnS41bgWfS2nZnU2XD4YjugycAyGh3JwmWVrWmJiUxemcvUG

for i in archive node{0,1,2}
do
    rm -Rf $i
    mkdir $i
done

ARCH=$(realpath archive)

for i in 0 1 2
do
    a=$(( ($i + 1) % 3 ))
    b=$(( ($i + 2) % 3 ))
    baseport=30000

    NSEC=NSEC${i}
    VSEC=VSEC${i}
    VPUB=VPUB${i}
    VPUBA=VPUB${a}
    VPUBB=VPUB${b}

    PORT=$(( $baseport + $i ))
    HTTP_PORT=$(($PORT + 8080))
    PORTA=$(( $baseport + $a ))
    PORTB=$(( $baseport + $b ))

    cat >node$i/node.cfg <<EOF
LOG_FILE_PATH="stellar.log"
RUN_STANDALONE=false
PEER_PORT=${PORT}
HTTP_PORT=${HTTP_PORT}
PEER_SEED="${!NSEC}"
VALIDATION_SEED="${!VSEC}"
PREFERRED_PEERS=["127.0.0.1:${PORTA}","127.0.0.1:${PORTB}"]
KNOWN_PEERS=["127.0.0.1:${PORTA}","127.0.0.1:${PORTB}"]
QUORUM_THRESHOLD=2
QUORUM_SET=["${!VPUB}", "${!VPUBA}", "${!VPUBB}"]
DATABASE="sqlite3://stellar.db"
COMMANDS=["ll?level=info"]
[HISTORY.archive]
get="cp ${ARCH}/{0} {1}"
put="cp {0} ${ARCH}/{1}"
mkdir="mkdir -p ${ARCH}/{0}"
EOF
done

echo "nodes configured, initializing databases"

(cd node0 && $ST --conf node.cfg --newdb --forcescp)
(cd node1 && $ST --conf node.cfg --newdb --forcescp)
(cd node2 && $ST --conf node.cfg --newdb)

echo "nodes initialized, initializing history archive"
(cd node0 && $ST --conf node.cfg --newhist archive)

rm node*/*.log
for i in 0 1 2; do touch node$i/stellar.log; done

echo "nodes initialized, running quietly + multitail"
(cd node0 && $ST --conf node.cfg --ll info >/dev/null 2>&1) &
(cd node1 && $ST --conf node.cfg --ll info >/dev/null 2>&1) &
(cd node2 && sleep 30 && $ST --conf node.cfg --ll info >/dev/null 2>&1) &
multitail --config multitail.conf -CS stellar node{0,1,2}/stellar.log

echo "killing all nodes"
killall -9 stellar-core
