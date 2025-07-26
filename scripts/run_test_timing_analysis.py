#!/usr/bin/env python3
"""
Run stellar-core test suite with wall-clock timing for each test.

Usage:
    ./run_test_timing_analysis.py                    # Run all tests
    ./run_test_timing_analysis.py -p history         # Run only history tests
    ./run_test_timing_analysis.py -n 50              # Show top 50 slowest tests
"""

import subprocess
import sys
import argparse
import os
import time
from typing import List, Tuple, Dict


def get_test_list(partition=None, test_filter=None):
    """Get list of tests to run."""
    cmd = ["stellar-core", "test", "--list-test-names-only"]
    
    if partition:
        cmd.append(f"[{partition}]")
    
    if test_filter:
        cmd.extend(["-a", test_filter])
    
    # Exclude benchmark and hidden tests
    cmd.append("~[bench]")
    cmd.append("~[!hide]")
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        # Parse test names from output, filtering out log lines
        tests = []
        for line in result.stdout.split('\n'):
            line = line.strip()
            if line and not line.startswith('Warning:') and not line.startswith('2025-'):
                tests.append(line)
        return tests
    except subprocess.CalledProcessError:
        return []


def run_single_test(test_name: str, src_dir: str) -> Tuple[float, int]:
    """Run a single test and return its wall-clock time and exit code."""
    cmd = ["stellar-core", "test", "-a", test_name]
    
    start_time = time.time()
    try:
        result = subprocess.run(
            cmd,
            cwd=src_dir,
            capture_output=True,
            text=True,
            check=False
        )
        elapsed = time.time() - start_time
        return elapsed, result.returncode
    except Exception as e:
        elapsed = time.time() - start_time
        print(f"Error running test '{test_name}': {e}")
        return elapsed, -1


def print_report(test_timings: Dict[str, float], top_n: int = 20):
    """Print timing report."""
    print("\n" + "=" * 80)
    print("TEST TIMING ANALYSIS REPORT (Wall-Clock Time)")
    print("=" * 80)
    
    # Sort tests by time
    sorted_tests = sorted(test_timings.items(), key=lambda x: x[1], reverse=True)
    
    # Total statistics
    total_tests = len(sorted_tests)
    total_time = sum(t[1] for t in sorted_tests)
    avg_time = total_time / total_tests if total_tests > 0 else 0
    
    print(f"\nTotal tests: {total_tests}")
    print(f"Total time: {total_time:.2f}s")
    print(f"Average time per test: {avg_time:.3f}s")
    
    # Slowest tests
    print(f"\nTop {min(top_n, total_tests)} Slowest Tests:")
    print("-" * 80)
    
    cumulative_time = 0
    for i, (test_name, time_seconds) in enumerate(sorted_tests[:top_n], 1):
        cumulative_time += time_seconds
        percentage = (time_seconds / total_time * 100) if total_time > 0 else 0
        print(f"{i:3d}. {time_seconds:8.3f}s ({percentage:5.1f}%) {test_name}")
    
    if total_time > 0:
        print(f"\nTop {min(top_n, total_tests)} tests account for {cumulative_time:.2f}s "
              f"({cumulative_time/total_time*100:.1f}% of total time)")


def main():
    parser = argparse.ArgumentParser(
        description="Run stellar-core tests with wall-clock timing"
    )
    parser.add_argument(
        "-p", "--partition",
        help="Test partition/tag to run (e.g., 'history', 'herder')"
    )
    parser.add_argument(
        "-t", "--test",
        help="Specific test name filter"
    )
    parser.add_argument(
        "-n", "--top",
        type=int,
        default=20,
        help="Number of slowest tests to show (default: 20)"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Show verbose output"
    )
    
    args = parser.parse_args()
    
    # Get list of tests
    tests = get_test_list(args.partition, args.test)
    
    if not tests:
        print("No tests found matching criteria")
        return 1
    
    print(f"Found {len(tests)} tests to run")
    print("Running each test individually to measure wall-clock time...")
    print("This may take a while depending on the number of tests...")
    print("-" * 80)
    
    # Check src directory
    src_dir = os.path.join(os.getcwd(), 'src')
    if not os.path.exists(src_dir):
        print(f"Error: src directory not found at {src_dir}")
        print("Please run this script from the stellar-core root directory")
        return 1
    
    # Run each test and collect timings
    test_timings = {}
    failed_tests = []
    
    for i, test_name in enumerate(tests, 1):
        if args.verbose:
            print(f"[{i}/{len(tests)}] Running: {test_name}")
        else:
            print(f"[{i}/{len(tests)}] Running: {test_name[:60]}..." if len(test_name) > 60 else f"[{i}/{len(tests)}] Running: {test_name}")
        
        elapsed, exit_code = run_single_test(test_name, src_dir)
        test_timings[test_name] = elapsed
        
        if exit_code != 0:
            failed_tests.append(test_name)
            if args.verbose:
                print(f"  -> FAILED (exit code: {exit_code}) in {elapsed:.3f}s")
        elif args.verbose:
            print(f"  -> Completed in {elapsed:.3f}s")
    
    # Print report
    print_report(test_timings, args.top)
    
    # Report failures
    if failed_tests:
        print(f"\nWarning: {len(failed_tests)} tests failed")
        if args.verbose:
            print("Failed tests:")
            for test in failed_tests[:10]:
                print(f"  - {test}")
            if len(failed_tests) > 10:
                print(f"  ... and {len(failed_tests) - 10} more")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())