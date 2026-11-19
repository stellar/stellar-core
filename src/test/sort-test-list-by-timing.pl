#!/usr/bin/env perl

use strict;
use warnings;

my ($timing_file, $test_file) = @ARGV;
die "Usage: $0 TIMING_FILE TEST_LIST\n" unless defined $test_file;

# selftest-parallel passes only this build's selected shards; reorder that queue
# without changing shard membership, using the table produced by update-test-timings.
my @tests;
open my $test_names, "<", $test_file or die "Cannot read test list: $!";
while (my $test = <$test_names>) {
    chomp $test;
    $test =~ s/\r$//;
    push @tests, $test;
}

die "Test list is empty\n" unless @tests;

my %durations;
my $malformed_rows = 0;
my $timings;
if (!open $timings, "<", $timing_file) {
    warn "Timing file $timing_file is unavailable; keeping shuffled test order\n";
    print "$_\n" for @tests;
    exit 0;
}

while (my $line = <$timings>) {
    chomp $line;
    $line =~ s/\r$//;
    next if $line =~ /^\s*(?:#|$)/;
    if ($line =~ /^((?:\d+(?:\.\d*)?|\.\d+))\t(.+)$/) {
        my ($duration, $name) = ($1, $2);
        if ($duration > 1_000_000_000) {
            $malformed_rows++;
            next;
        }
        $durations{$name} += $duration;
    } else {
        $malformed_rows++;
    }
}

my %original_order;
@original_order{@tests} = (0 .. $#tests);
my $untimed_tests = grep { !exists $durations{$_} } @tests;
warn "Ignoring $malformed_rows malformed timing rows\n" if $malformed_rows;
warn "$untimed_tests selected tests have no timing; scheduling them first\n"
    if $untimed_tests;

for my $test (sort {
    (exists $durations{$a} <=> exists $durations{$b}) ||
    (exists $durations{$b} ? $durations{$b} <=> $durations{$a} : 0) ||
    ($original_order{$a} <=> $original_order{$b})
} @tests) {
    print "$test\n";
}
