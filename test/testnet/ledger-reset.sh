#!/bin/sh
# Reset the stellar ledger
# Probably work only on my computer.
STELLAR_CORE_SRC=$HOME/stellar-core
HORIZON_SRC=$HOME/horizon

echo "Killing stellar-core"
killall stellar-core

echo "Killing horizon"
killall horizon

echo "drop and create the stellar db"
sudo -u postgres sh -c 'psql -c "drop database stellar;"'
sudo -u postgres sh -c 'psql -c "create database stellar;"'

echo "drop the horizon db"
sudo -u postgres sh -c 'psql -c "drop database horizon;"'

echo "init the stellar db"
cd $STELLAR_CORE_SRC
./src/stellar-core --newdb

echo "init the horizon db"
cd $HORIZON_SRC
rake db:create && rake db:migrate
