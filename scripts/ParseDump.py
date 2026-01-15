import argparse
import re
import subprocess

# input: ./src/stellar-core(+0xd9f6bd) [0x55ab4c2456bd]
def extract_addr(line):
    if "stellar-core" not in line:
        return None

    # extracts 0x55ab4c2456bd by matching on [...]$
    # ./src/stellar-core(+0xd9f6bd) [0x55ab4c2456bd] -> 0x55ab4c2456bd
    match = re.search(r"\[(0x[0-9a-f]+)\]\s*$", line)
    if match is not None:
        return match.group(1)
    return None


def main():
    parser = argparse.ArgumentParser(
        description="Provide human readable stack trace for stellar-core traces."
    )

    parser.add_argument(
        "exe",
        type=argparse.FileType("r"),
        help="Filepath to stellar-core executable with debug symbols installed",
    )

    parser.add_argument(
        "stack_trace",
        type=argparse.FileType('r'),
        help="Stack trace reported by stellar-core (should start with something like: ./src/stellar-core(+0xd9f6bd) [0x55a1fcb7d6bd])",
    )

    args = parser.parse_args()
    command = [
        "addr2line",
        "-f",  # Display function names
        "-C",  # Demangle function names
        "-p",  # Pretty print to human readable form
        "-s",  # Only show file base names
        "-e",
        args.exe.name,
    ]
    addrs = False
    for line in args.stack_trace.readlines():
        addr = extract_addr(line)
        if addr is not None:
            addrs = True
            command.append(addr)
    if not addrs:
        print("No addresses found in stack trace")
        return
    subprocess.run(command)

if __name__ == "__main__":
    main()
