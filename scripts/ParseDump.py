import argparse
import re
import subprocess


# input: ./src/stellar-core(+0xd9f6bd) [0x55ab4c2456bd]
def extract_relative_offset(line):
    if "stellar-core" not in line:
        return ""

    # extracts (+0xd9f6bd) by matching on (+ ... )
    # ./src/stellar-core(+0xd9f6bd) [0x55ab4c2456bd] -> (+0xd9f6bd)
    formated_offset = re.findall(r"\(\+.*?\)", line)

    # Remove first two characters and final character to extract raw offset in 0x.. form
    # (+0xd9f6bd) -> 0xd9f6bd
    return formated_offset[0][2:-1]


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
        help="Stack trace reported by stellar-core (should start with something like: ./src/stellar-core(+0xd9f6bd) [0x55a1fcb7d6bd])",
    )

    args = parser.parse_args()
    stack_traces = args.stack_trace.split("\n")

    addr2line_base_args = [
        "addr2line",
        "-f",  # Display function names
        "-C",  # Demangle function names
        "-p",  # Pretty print to human readable form
        "-s",  # Only show file base names
        "-e",
        args.exe.name,
    ]

    for line in stack_traces:
        relative_offset = extract_relative_offset(line)
        if not relative_offset:
            print("??")
        else:
            command = addr2line_base_args.copy()
            command.append(relative_offset)
            subprocess.run(command)


if __name__ == "__main__":
    main()
