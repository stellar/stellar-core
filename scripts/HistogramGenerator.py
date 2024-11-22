#!/usr/bin/env python3

# Copyright 2024 Stellar Development Foundation and contributors. Licensed
# under the Apache License, Version 2.0. See the COPYING file at the root
# of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

from base64 import b64decode
import csv
import json
from multiprocessing.pool import Pool
from typing import Any, Optional, Tuple
import subprocess
import sys

import numpy as np
import numpy.typing as npt

# Sample query to gather history_transactions data:
# SELECT soroban_resources_instructions, soroban_resources_write_bytes, tx_envelope FROM `crypto-stellar.crypto_stellar.history_transactions` WHERE batch_run_date BETWEEN DATETIME("2024-06-24") AND DATETIME("2024-09-24") AND soroban_resources_instructions > 0

# Sample query to gather history_contract_events data:
# SELECT topics_decoded, data_decoded FROM `crypto-stellar.crypto_stellar.history_contract_events` WHERE type = 2 AND TIMESTAMP_TRUNC(closed_at, MONTH) between TIMESTAMP("2024-06-27") AND TIMESTAMP("2024-09-27") AND contains_substr(topics_decoded, "write_entry")
# NOTE: this query filters out anything that isn't a write_entry. This is
# required for the script to work correctly!

# Threads to use for parallel processing
WORKERS=9

# Maximum number of histogram bins to generate
MAX_BINS=100

# Maximum number of histogram bins to output. This is much lower than MAX_BINS,
# because most bins will be empty (and therefore pruned from the output). If
# there are too many bins with nonzero values, the script will reduce the number
# of bins until there are at most MAX_OUTPUT_BINS bins with nonzero values.
MAX_OUTPUT_BINS=10

def decode_xdr(xdr: str) -> dict[str, Any]:
    """ Decode a TransactionEnvelope using the stellar-xdr tool. """
    decoded = subprocess.check_output(
            ["stellar-xdr", "decode",
            "--type", "TransactionEnvelope",
            "--input", "single-base64",
            "--output", "json"
            ],
            input=xdr.encode("utf-8"))
    return json.loads(decoded)

def process_history_row(row: dict[str, str]) -> Tuple[Optional[Tuple[int, int, int]], Optional[int]]:
    """
    Process a row from the history_transactions table. Returns:
    * (None, None) if the row is not a transaction
    * (None, wasm_size) if the row is a wasm upload
    * ((instructions, write_bytes, tx_size), None) if the row is an invoke
      transaction
    """
    envelope_xdr = row["tx_envelope"]
    assert isinstance(envelope_xdr, str)
    envelope = decode_xdr(envelope_xdr)
    if "tx" not in envelope:
        # Skip anything that isn't a transaction (such as a fee bump)
        return (None, None)
    operations = envelope["tx"]["tx"]["operations"]
    assert len(operations) == 1
    body = operations[0]["body"]
    if "invoke_host_function" in body:
        ihf = body["invoke_host_function"]
        if "upload_contract_wasm" in ihf["host_function"]:
            # Count wasm bytes
            wasm = ihf["host_function"]["upload_contract_wasm"]
            return (None, len(bytes.fromhex(wasm)))
        else:
            # Treat as a "normal" invoke
            instructions = row["soroban_resources_instructions"]
            write_bytes = row["soroban_resources_write_bytes"]
            return ( ( int(instructions),
                       int(write_bytes),
                       len(b64decode(envelope_xdr, validate=True))),
                     None)
    return (None, None)

def process_event_row(row: dict[str, str]) -> int:
    """
    Process a row from the history_events table. Must already be filtered to
    contain only write entries. Returns an int with the number of write entries.
    """
    return int(json.loads(row["data_decoded"])["value"])

def to_normalized_histogram(data: npt.ArrayLike) -> None:
    """
    Given a set of data, print a normalized histogram with at most
    MAX_OUTPUT_BINS bins formatted for easy pasting into supercluster.
    """
    for i in range(MAX_BINS, MAX_OUTPUT_BINS-1, -1):
        hist, bins = np.histogram(data, bins=i)

        # Add up counts in each bin
        total = np.sum(hist)

        # Normalize each count to parts per ten-thousand
        normalized = (hist / total) * 1000

        # Convert to ints
        normalized = normalized.round().astype(int)

        if len([x for x in normalized if x != 0]) <= MAX_OUTPUT_BINS:
            break
        # We have too many non-zero output bins. Reduce the number of total bins
        # and try again.

    # Find midpoint of each bin
    midpoints = np.empty(len(bins) - 1)
    for i in range(len(bins) - 1):
        midpoints[i] = (bins[i] + bins[i + 1]) / 2

    # Convert to ints
    midpoints = midpoints.round().astype(int)

    # Format for easy pasting into supercluster
    print("[ ", end="")
    for count, point in zip(normalized, midpoints):
        if count == 0:
            # Drop empty bins
            continue
        print(f"({point}, {count}); ", end="")
    print("]")

def process_soroban_history(history_transactions_csv: str) -> None:
    """ Generate histograms from data in the history_transactions table. """
    with open(history_transactions_csv) as f:
        reader = csv.DictReader(f)

        # Decode XDR in parallel
        with Pool(WORKERS) as p:
            processed_rows = p.imap_unordered(process_history_row, reader)

            # Filter to just valid rows
            valid = [row for row in processed_rows if row != (None, None)]

            # Parse out invokes
            invokes = [i for (i, u) in valid if i is not None]
            wasms = [u for (i, u) in valid if u is not None]

            # Decompose into instructions, write bytes, and tx size
            instructions, write_bytes, tx_size = zip(*invokes)

            print("Instructions:")
            to_normalized_histogram(instructions)

            # Convert write_bytes to kilobytes
            write_kilobytes = (np.array(write_bytes) / 1024).round().astype(int)
            print("\nI/O Kilobytes:")
            to_normalized_histogram(write_kilobytes)

            print("\nTransaction Size Bytes:")
            to_normalized_histogram(tx_size)

            print("\nWasm Size Bytes:")
            to_normalized_histogram(wasms)

def process_soroban_events(history_contract_events_csv) -> None:
    """
    Generate a histogram for data entries from data in the
    history_contract_events table.
    """
    with open(history_contract_events_csv) as f:
        reader = csv.DictReader(f)

        # Process CSV in parallel
        with Pool(WORKERS) as p:
            processed_rows = list(p.imap_unordered(process_event_row, reader))

            print("Data Entries:")
            to_normalized_histogram(processed_rows)

def help_and_exit() -> None:
    print(f"Usage: {sys.argv[0]} <history_transactions data> "
          "<history_contract_events data>")
    print("See the comments at the top of this file for sample Hubble queries "
          "to generate the appropriate data.")
    sys.exit(1)

def main() -> None:
    if len(sys.argv) != 3:
        help_and_exit()

    print("Processing data. This might take a few minutes...")

    process_soroban_history(sys.argv[1])
    print("")
    process_soroban_events(sys.argv[2])

if __name__ == "__main__":
    main()