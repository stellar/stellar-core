#!/usr/bin/env python3
"""Convert frozen ledger keys from base64 to hex in place.

This script updates JSON files that contain `updated_entry` objects with a
`frozen_ledger_keys_delta` section. It rewrites `keys_to_freeze` and
`keys_to_unfreeze` entries from base64 to lowercase hex, while skipping keys
that already start with `0000` because those are guaranteed to already be hex.

Usage:
    python frozen_keys_base64_to_hex.py path/to/frozen_keys.json
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import sys
from pathlib import Path
from typing import Any


TARGET_FIELDS = ("keys_to_freeze", "keys_to_unfreeze")
HEX_PREFIX = "0000"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert frozen_ledger_keys_delta keys from base64 to hex in place."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "json_file",
        type=Path,
        help="Path to the JSON file to update in place.",
    )
    return parser.parse_args()


def decode_base64_to_hex(value: str) -> str:
    normalized = value.strip()
    padding = "=" * (-len(normalized) % 4)
    decoded = base64.b64decode(normalized + padding, validate=True)
    return decoded.hex()


def warn(location: str, value: Any) -> None:
    print(
        f"Warning: could not parse base64 key at {location}: {value!r}",
        file=sys.stderr,
    )


def convert_key_list(keys: list[Any], *, location_prefix: str) -> tuple[list[Any], int, int]:
    converted: list[Any] = []
    converted_count = 0
    warning_count = 0

    for index, key in enumerate(keys):
        location = f"{location_prefix}[{index}]"
        if not isinstance(key, str):
            warn(location, key)
            converted.append(key)
            warning_count += 1
            continue

        if key.startswith(HEX_PREFIX):
            converted.append(key)
            continue

        try:
            hex_value = decode_base64_to_hex(key)
        except (binascii.Error, ValueError):
            warn(location, key)
            converted.append(key)
            warning_count += 1
            continue

        converted.append(hex_value)
        if hex_value != key:
            converted_count += 1

    return converted, converted_count, warning_count


def convert_document(document: dict[str, Any]) -> tuple[bool, int, int]:
    updated_entry = document.get("updated_entry")
    if not isinstance(updated_entry, list):
        return False, 0, 0

    changed = False
    converted_total = 0
    warning_total = 0

    for entry_index, entry in enumerate(updated_entry):
        if not isinstance(entry, dict):
            continue

        delta = entry.get("frozen_ledger_keys_delta")
        if not isinstance(delta, dict):
            continue

        for field_name in TARGET_FIELDS:
            keys = delta.get(field_name)
            if keys is None:
                continue
            if not isinstance(keys, list):
                warn(f"updated_entry[{entry_index}].frozen_ledger_keys_delta.{field_name}", keys)
                warning_total += 1
                continue

            converted_keys, converted_count, warning_count = convert_key_list(
                keys,
                location_prefix=(
                    f"updated_entry[{entry_index}].frozen_ledger_keys_delta.{field_name}"
                ),
            )
            if converted_keys != keys:
                delta[field_name] = converted_keys
                changed = True
            converted_total += converted_count
            warning_total += warning_count

    return changed, converted_total, warning_total


def main() -> int:
    args = parse_args()

    try:
        content = args.json_file.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"Failed to read {args.json_file}: {exc}", file=sys.stderr)
        return 1

    try:
        document = json.loads(content)
    except json.JSONDecodeError as exc:
        print(f"Failed to parse JSON from {args.json_file}: {exc}", file=sys.stderr)
        return 1

    if not isinstance(document, dict):
        print(
            f"Expected top-level JSON object in {args.json_file}, found {type(document).__name__}.",
            file=sys.stderr,
        )
        return 1

    changed, converted_total, warning_total = convert_document(document)

    try:
        args.json_file.write_text(
            json.dumps(document, indent=2) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        print(f"Failed to write {args.json_file}: {exc}", file=sys.stderr)
        return 1

    print(
        f"Converted {converted_total} key(s) in {args.json_file}. Warnings: {warning_total}. Changed: {'yes' if changed else 'no'}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())