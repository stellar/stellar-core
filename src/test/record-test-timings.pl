#!/usr/bin/env perl

use strict;
use warnings;

# Convert Catch's duration report to the sorted table used by the shard scheduler.
# With --all-versions, repeated test-case totals are summed into one estimate.
# Keep available timings from failed or interrupted runs, warning about omissions.
my %seconds;
my $current_test;
my $current_test_has_duration = 0;
my $test_count = 0;
my $missing_duration_count = 0;

while (my $line = <STDIN>) {
    if ($line =~ /^"([^"]+)"\s+.*:\d+\s*$/) {
        $missing_duration_count++
            if defined $current_test && !$current_test_has_duration;
        $test_count++;
        $current_test = $1;
        $current_test_has_duration = 0;
        next;
    }

    if (defined $current_test &&
        $line =~ /^([0-9]+(?:\.[0-9]+)?) s: (.+)$/ &&
        $2 eq $current_test) {
        $seconds{$current_test} += $1;
        $current_test_has_duration = 1;
    }

    if ($line =~ /<done>/ && defined $current_test) {
        $missing_duration_count++ unless $current_test_has_duration;
        undef $current_test;
    }
}

$missing_duration_count++
    if defined $current_test && !$current_test_has_duration;
die "No test durations found in output\n" unless %seconds;
warn "Missing top-level durations for $missing_duration_count of $test_count test runs; saving available timings\n"
    if $missing_duration_count;

print "# seconds\ttest name (all-version total)\n";
for my $name (sort { $seconds{$b} <=> $seconds{$a} || $a cmp $b } keys %seconds) {
    printf "%.3f\t%s\n", $seconds{$name}, $name;
}
