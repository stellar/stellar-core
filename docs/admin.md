---
id: admin
title: Admin
category: Guides
---

stellar-core is responsible for communicating directly with and/or maintaining the Stellar peer to peer network. You will want to run a stellar-core if you want to:
* Participate in validating the Stellar network
* Obtain the most up to date and reliable data from the Stellar network
* Submit transactions without depending on a 3rd party

## Building
See [readme](/README.md) for build instructions.

## Running an instance

## Configuring
All configuration for stellar-core is done with a TOML file. By default `./stellar-core.cfg`, you can specify the file stellar-core loads on the commandline: `> stellar-core --conf betterfile.cfg` The [example config](/docs/stellar-core_example.cfg) describes all the possible configuration options.  

## Hardware requirements
The hardware requirements scale with the amount of activity in the network. Currently stellar-core requires very modest hardware. It would be fine to run on an AWS micro instance for example.

## Validating
Nodes are considered **validating** if they take part in SCP and sign messages pledging that the network agreed to a particular transaction set. It isn't necessary to be a validator. Only set your node to validate if other nodes care about your validation. 
If you want to validate you must generate a public/private key for your node. Nodes shouldn't share keys. You should carefully secure your private key. If it is compromised someone can send false messages to the network and they will look like they came from you. 
Generate a key pair like this:
`> stellar-core --genseed`
Place the seed in your config:
`VALIDATION_SEED="SBI3CZU7XZEWVXU7OZLW5MMUQAP334JFOPXSLTPOH43IRTEQ2QYXU5RG"`
The public key you should advertise so people can add it to their `QUOROM_SET` in their config.
If you don't include a `VALIDATION_SEED` you will still watch SCP and see all the data in the network but you won't send validation messages. 

## Bucket list

## Database
stellar-core stores the state of the ledger in a SQL DB. It can be configured to use; in memory SQLite, SQLite on disk, or Postgres. See [example config](/docs/stellar-core_example.cfg) for how to specify the DB.
When running stellar-core for the first time you must initialize the DB like this:
`> stellar-core --newDB`
This will initialize the DB and then exit. 
You can also use this command if your DB gets corrupted and you want to restart it from scratch. 

## History Store


## Administrative commands
Interaction with stellar-core is done via an administrative HTTP endpoint.
The endpoint is typically accessed by a mid-tier application to submit 
transactions to the Stellar network, or by administrators. See [commands](./commands.md) for a description of the available commands.

## Docker

## Notes
It can take up to 5 or 6 minutes to sync to the network when you start up.



## Additional Documentation

This directory contains the following additional documentation:

* [testnet.md](/docs/testnet.md) is short tutorial demonstrating how to
  configure and run a short-lived, isolated test network.

* [architecture.md](/docs/architecture.md) describes how `stellar-core` is
  structured internally, how it is intended to be deployed and the collection of
  servers and services needed to get the full functionality and performance.

