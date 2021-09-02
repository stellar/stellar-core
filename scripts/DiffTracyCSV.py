#!/usr/bin/env python

import sys
if sys.version_info < (3, 4):
    raise "must use python 3.4 or greater"

import csv
import statistics
import argparse
from collections import namedtuple

Measure = namedtuple("Measure", ["old", "new", "diff", "pct", "flag"])
Measures = namedtuple("Measures", ["median", "p90", "sum", "events"])
Changes = namedtuple("Changes", ["zone", "measures"])


def read_file(filename):
    data = dict()
    with open(filename, newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        next(reader, None)  # skip header
        rows = 0
        for (name, src_file, src_line, _, exec_time_ns) in reader:
            rows += 1
            key = (name, src_file, src_line)
            if key in data:
                data[key].append(float(exec_time_ns))
            else:
                data[key] = [float(exec_time_ns)]
    print("  - read {} rows about {} zones from {}".format(rows, len(data),
                                                           filename))
    return data


def chk_diff(old, new, pct_lim, abs_lim):
    diff = int(new - old)
    pct = (diff / (1.0 + old)) * 100.0
    flag = (new >= abs_lim and pct >= pct_lim)
    return Measure(old=int(old), new=int(new), flag=flag,
                   diff=diff, pct=int(pct))


def filter_zone_changes(old, new, pct_lim, zone_lim, sum_lim, evt_lim):
    print(("  - showing all zones with pct_lim={:,d}, zone_lim={:s}, " +
           "sum_lim={:s} and evt_lim={:,d}\n")
          .format(pct_lim, fmt_time(zone_lim), fmt_time(sum_lim), evt_lim))
    out = []
    for (zone, new_data) in new.items():
        if zone in old.keys():
            old_data = old[zone]
            old_evt = len(old_data)
            new_evt = len(new_data)
            if old_evt > 2 and new_evt > 2:
                if old_evt < evt_lim and new_evt < evt_lim:
                    continue
                old_sum = sum(old_data)
                new_sum = sum(new_data)
                if old_sum < sum_lim and new_sum < sum_lim:
                    continue
                # Quantiles returns an n-1 cutpoint list qq, so median is
                # at qq[4] and p90 is at qq[8].
                old_qq = statistics.quantiles(old_data, n=10)
                new_qq = statistics.quantiles(new_data, n=10)
                m1 = chk_diff(old_qq[4], new_qq[4], pct_lim, zone_lim)
                m2 = chk_diff(old_qq[8], new_qq[8], pct_lim, zone_lim)
                m3 = chk_diff(old_sum, new_sum, pct_lim, sum_lim)
                m4 = chk_diff(old_evt, new_evt, pct_lim, evt_lim)
                if m1.flag or m2.flag or m3.flag or m4.flag:
                    ms = Measures(median=m1, p90=m2, sum=m3, events=m4)
                    z = "{} @ {}:{}".format(*zone)
                    out.append(Changes(zone=z, measures=ms))
    return out


def fmt_time(ns):
    thousand = 1000
    million = thousand * thousand
    billion = million * thousand
    ans = int(abs(ns))
    if ans >= billion:
        return "{:n} sec".format(int(ns / billion))
    elif ans >= million:
        return "{:n} msec".format(int(ns / million))
    elif ans >= thousand:
        return "{:n} usec".format(int(ns / thousand))
    else:
        return "{:n} nsec".format(int(ns))


def fmt_flag(flag):
    if flag:
        return "🛑"
    else:
        return "✅"


def main():

    # construct the argument parse and parse the arguments
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument("--old", required=True,
                                 help="old CSV file")
    argument_parser.add_argument("--new", required=True,
                                 help="new CSV file")
    argument_parser.add_argument("--pct-lim", default=10, type=int,
                                 help="limit to deltas >= given percent")
    argument_parser.add_argument("--zone-lim", default=1000, type=int,
                                 help="limit to zone delta >= given nsecs")
    argument_parser.add_argument("--sum-lim", default=100000000, type=int,
                                 help="limit to sum delta >= given nsecs")
    argument_parser.add_argument("--evt-lim", default=1000, type=int,
                                 help="limit to event delta >= given count")

    args = argument_parser.parse_args()

    print("\n### Tracy zone-timing comparison\n")

    old = read_file(args.old)
    new = read_file(args.new)
    out = filter_zone_changes(old, new, args.pct_lim, args.zone_lim,
                              args.sum_lim, args.evt_lim)

    if len(out) == 0:
        print("**No zone changes exceed limits**")
    else:
        out.sort(key=lambda v: v.measures.sum.pct)
        for c in out:
            print("\n### {}".format(c.zone))
            print(("| {:>8s} | {:>15s} | {:>15s} " +
                   "| {:>15s} | {:>15s} | {:<5s} ").format(
                       "measure", "old", "new",
                       "diff", "diff %", "flag"))
            print("|---------:|----------------:|----------------:" +
                  "|----------------:|----------------:|:------|")
            for k, m in c.measures._asdict().items():
                if k == "events":
                    print(("| {:>8.8s} | {:15n} | {:15n} " +
                           "| {:15n} | {:14n}% | {:<5s} |").format(
                               k, m.old, m.new, m.diff,
                               m.pct, fmt_flag(m.flag)))
                else:
                    print(("| {:>8.8s} | {:>15s} | {:>15s} " +
                           "| {:>15s} | {:14n}% | {:<5s} |").format(
                                k, fmt_time(m.old), fmt_time(m.new),
                                fmt_time(m.diff), m.pct, fmt_flag(m.flag)))


if __name__ == "__main__":
    main()
