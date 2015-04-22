#Running an instance

#Hardware requirements
    (need more stress testing)

#Operational considerations
* You will need a PEER_SEED to recognize your node on the network;
* you will also need a VALIDATION_SEED if you plan on participating in SCP

#Administrative commands
    Interaction with stellar-core is done via an administrative
    HTTP endpoint.
    The endpoint is typically accessed by a mid-tier application to
    submit transactions to the Stellar network, or by administrators.

#Notes
 It can take up to 5 or 6 minutes to sync to the network anytime you start up.

#Supported commands
## /catchup?ledger=NNN[&mode=MODE]
        triggers the instance to catch up to ledger NNN from history;
        mode is either 'minimal' (the default, if omitted) or 'complete'.
## /checkpoint
        triggers the instance to write an immediate history checkpoint.
## /connect?peer=NAME&port=NNN
        triggers the instance to connect to peer NAME at port NNN.
## /help
        give a list of currently supported commands
## /info
        returns information about the server in JSON format (sync
        state, connected peers, etc)
## /ll?level=L[&partition=P]
        adjust the log level for partition P (or all if no partition
        is specified).
        level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE
## /logrotate
        rotate log file
## /manualclose
        close the current ledger; must be used with MANUAL_CLOSE set to true
## /metrics
        returns a snapshot of the metrics registry (for monitoring and
        debugging purpose)
## /peers
        returns the list of known peers in JSON format
## /scp
        returns a JSON object with the internal state of the SCP engine
## /stop
        stops the instance
## /tx?blob=HEX
        submit a transaction to the network.
        blob is a hex encoded XDR serialized 'TransactionEnvelope'
        returns a JSON object
            wasReceived: boolean, true if transaction was queued properly
            result: hex encoded, XDR serialized 'TransactionResult'

