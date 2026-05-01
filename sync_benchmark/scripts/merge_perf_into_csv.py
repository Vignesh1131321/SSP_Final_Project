#!/usr/bin/env python3
import csv
import re
import argparse
from collections import defaultdict

EVENT_KEYS = {
    'cache-misses': 'perf_cache_misses',
    'instructions': 'perf_instructions',
    'cycles': 'perf_cycles',
    'branch-misses': 'perf_branch_misses',
    'context-switches': 'perf_context_switches',
    'cpu-migrations': 'perf_cpu_migrations',
}

perf_line_re = re.compile(r"^\s*([0-9,]+)\s+([a-zA-Z0-9\-]+)\b")

def parse_perf_stat(path):
    vals = {}
    with open(path, 'r') as f:
        for line in f:
            m = perf_line_re.search(line)
            if not m:
                continue
            num_s, name = m.groups()
            num = int(num_s.replace(',', ''))
            if name in EVENT_KEYS:
                vals[EVENT_KEYS[name]] = str(num)
    return vals

def load_csv(path):
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        fieldnames = reader.fieldnames
    return rows, fieldnames

def write_csv(path, rows, fieldnames):
    with open(path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in rows:
            writer.writerow(r)

def key_for_row(r):
    # Match on identifying columns used by sync_bench
    return (r.get('suite',''), r.get('experiment',''), r.get('primitive',''), r.get('threads',''), r.get('trial',''))

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--orig-csv', required=True)
    p.add_argument('--new-csv', required=True)
    p.add_argument('--perf-stat', required=True)
    p.add_argument('--out-csv', required=True)
    args = p.parse_args()

    perf_vals = parse_perf_stat(args.perf_stat)

    orig_rows, orig_fields = load_csv(args.orig_csv)
    new_rows, new_fields = load_csv(args.new_csv)

    # build index from new_rows by key
    idx = { key_for_row(r): r for r in new_rows }

    # determine new fields to add
    add_fields = list(perf_vals.keys())
    # also possibly cpu_migrations/ctx_switches are present in new_rows
    for fname in ['cpu_migrations','ctx_switches']:
        if fname in new_fields and fname not in orig_fields:
            add_fields.append(fname)

    # extend fieldnames preserving original order
    out_fields = list(orig_fields)
    for f in add_fields:
        if f not in out_fields:
            out_fields.append(f)

    # Merge: for each orig row, supplement with perf_vals (same for all rows), and copy cpu_migrations/ctx_switches if available in new run (matched by key)
    out_rows = []
    for r in orig_rows:
        out = dict(r)
        # add perf totals
        for k,v in perf_vals.items():
            out.setdefault(k, v)
        # copy cpu_migrations/ctx_switches from matched new row if present and orig value missing or -1
        key = key_for_row(r)
        newr = idx.get(key)
        if newr:
            for f in ['cpu_migrations','ctx_switches']:
                if f in new_fields:
                    try:
                        orig_val = int(r.get(f, '-1'))
                    except Exception:
                        orig_val = -1
                    if orig_val < 0:
                        out[f] = newr.get(f, out.get(f, ''))
        out_rows.append(out)

    write_csv(args.out_csv, out_rows, out_fields)

if __name__ == '__main__':
    main()
