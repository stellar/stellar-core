Running an instance

Hardware requirement
    (need more stress testing)

Operational considerations
You will need a PEER_SEED to recognize your node on the network;
you will also need a VALIDATION_SEED if you plan on contributing to SCP

Administrative commands
    Interaction with stellar-core is done via an administrative
    HTTP endpoint.
    The endpoint is typically accessed by a mid-tier application to
    submit transactions to the Stellar network, or by administrators.

    Supported commands:
*/help
        give a list of currently supported commands
* /stop
        stops the instance
* /peers
        returns the list of known peers in JSON format
* /info
        returns information about the server in JSON format (sync
        state, connected peers, etc)
* /metrics
        returns a snapshot of the metrics registry (for monitoring and
        debugging purpose)
* /logrotate
        rotate log files
* /connect?peer=NAME&port=NNN
        triggers the instance to connect to peer NAME at port NNN.
* /tx?blob=HEX
        submit a transaction to the network.
        blob is a hex encoded XDR serialized "TransactionEnvelope"
        returns a JSON object
            wasReceived: boolean, true if transaction was queued properly
            result: hex encoded, XDR serialized "TransactionResult"
* /ll?level=L[&partition=P]
        adjust the log level for partition P (or all if no partition
        is specified).
        level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE

