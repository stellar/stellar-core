#!/bin/sh
# Usage: `bash settings-helper.sh <ACCOUNT_SECRET_KEY> <pubnet|testnet> <path_to_settings.json>
set -e

SECRET_KEY="$1"
PUBLIC_KEY=$(./stellar-core convert-id "$SECRET_KEY" | sed -n '4p' | awk '{print $NF}')
echo "PUBLIC_KEY is $PUBLIC_KEY"

PUBNET_HORIZON="https://horizon.stellar.org/accounts"
PUBNET_PASSPHRASE="Public Global Stellar Network ; September 2015"

TESTNET_HORIZON="https://horizon-testnet.stellar.org/accounts"
TESTNET_PASSPHRASE="Test SDF Network ; September 2015"

HORIZON=""
PASSPHRASE=""

#choose which horizon to hit for the SEQ_NUM. If Horizon is down, remove this code and manually set SEQ_NUM below
if [ "$2" == "pubnet" ]
then
HORIZON=$PUBNET_HORIZON
PASSPHRASE=$PUBNET_PASSPHRASE
elif [ "$2" == "testnet" ]
then
HORIZON=$TESTNET_HORIZON
PASSPHRASE=$TESTNET_PASSPHRASE
else
echo "invalid passphrase"
fi

# get seq num
SEQ_NUM="$(curl -s "$HORIZON/$PUBLIC_KEY" | grep "\"sequence\":" | sed 's/[^0-9]//g')"
re='^[0-9]+$'
if ! [[ $SEQ_NUM =~ $re ]] ; then
   echo "Error: SEQ_NUM not retrieved. Your account might not be funded, or Horizon might be down. Hardcode the SEQ_NUM below and remove the horizon code." >&2; exit 1
fi

OUTPUT="$(echo $SECRET_KEY | ./stellar-core get-settings-upgrade-txs "$PUBLIC_KEY" "$SEQ_NUM" "$PASSPHRASE" --xdr $(stellar-xdr encode --type ConfigUpgradeSet $3) --signtxs)"

echo "----- TX #1 -----"
echo "curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=$(echo "$OUTPUT" | sed -n '1p')'"

echo "----- TX #2 -----"
echo "curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=$(echo "$OUTPUT" | sed -n '3p')'"

echo "----- TX #3 -----"
echo "curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=$(echo "$OUTPUT" | sed -n '5p')'"
echo "-----"

echo "----- TX #4 -----"
echo "curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=$(echo "$OUTPUT" | sed -n '7p')'"
echo "-----"

echo "curl -G 'http://localhost:11626/dumpproposedsettings' --data-urlencode 'blob=$(echo "$OUTPUT" | sed -n '9p')'"
echo "-----"

echo "distribute the following command with the upgradetime set to an agreed upon point in the future"
echo "curl -G 'http://localhost:11626/upgrades?mode=set&upgradetime=YYYY-MM-DDT01:25:00Z' --data-urlencode 'configupgradesetkey=$(echo "$OUTPUT" | sed -n '9p')'"
