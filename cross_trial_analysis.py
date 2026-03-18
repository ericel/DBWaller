#!/usr/bin/env python3
"""Cross-trial variance analysis for DBWaller benchmarks."""

import csv
import glob
import os
import statistics

files = sorted(glob.glob('results/processed/*.csv'))
print(f"Processing {len(files)} CSV files...\n")

speedup_stats = []
run_data = {}

for fpath in files:
    basename = os.path.basename(fpath)
    # Extract trial name
    if 'trial' in basename:
        trial = basename.split('trial')[1].split('.')[0]
        trial = 'trial' + trial
    else:
        trial = '1'
    
    with open(fpath, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                threads = int(float(row.get('threads', 0) or 0))
                shards = int(float(row.get('shards', 0) or 0))
                write_pct = int(float(row.get('write_percent', 0) or 0))
                skew = int(float(row.get('skew_mode', 0) or 0))
                
                if threads == 0 or shards == 0:
                    continue
                
                key = (threads, shards, write_pct, skew)
                if key not in run_data:
                    run_data[key] = {}
                
                # Get ops from either column depending on impl
                baseline_ops = float(row.get('baseline_ops_per_sec', '') or 0)
                dbwaller_ops = float(row.get('dbwaller_ops_per_sec', '') or 0)
                
                if baseline_ops > 0:
                    run_data[key][(trial, 0)] = baseline_ops
                if dbwaller_ops > 0:
                    run_data[key][(trial, 1)] = dbwaller_ops
            except (ValueError, KeyError, TypeError):
                pass

# Compute speedups across trials
for workload, ops_dict in run_data.items():
    speedups = []
    for trial in ['1', 'trial2', 'trial3', 'trial4']:
        baseline = ops_dict.get((trial, 0), 0)
        sharded = ops_dict.get((trial, 1), 0)
        if baseline > 0:
            speedups.append(sharded / baseline)
    
    if len(speedups) >= 2:
        threads, shards, write, skew = workload
        speedup_stats.append({
            't': threads, 's': shards, 'w': write, 'sk': 'H' if skew else 'U',
            'n': len(speedups),
            'avg': statistics.mean(speedups),
            'min': min(speedups), 'max': max(speedups),
            'std': statistics.stdev(speedups) if len(speedups) > 1 else 0,
            'cv': statistics.stdev(speedups) / statistics.mean(speedups) * 100 if len(speedups) > 1 else 0,
        })

speedup_stats.sort(key=lambda x: x['avg'], reverse=True)

print("=" * 95)
print("TOP 15 SCENARIOS (by mean speedup)")
print("=" * 95)
print("Threads Shards W%  Mode Runs   Mean    Min    Max  StdDev  CV%")
print("-" * 95)
for s in speedup_stats[:15]:
    print(f"{s['t']:7d} {s['s']:7d} {s['w']:3d}  {s['sk']:>2s}   {s['n']:4d} "
          f"{s['avg']:6.2f}x {s['min']:6.2f}x {s['max']:6.2f}x {s['std']:6.3f}  {s['cv']:5.1f}%")

all_sp = [s['avg'] for s in speedup_stats]
print(f"\n{'=' * 95}")
print(f"SUMMARY STATISTICS")
print(f"{'=' * 95}")
print(f"Unique workload scenarios (with 2+ trial runs): {len(all_sp)}")
print(f"Total samples: {len(all_sp) * 4} (scenarios × 4 trials)")
print(f"Median speedup: {statistics.median(all_sp):.2f}x")
print(f"Mean speedup: {statistics.mean(all_sp):.2f}x")
print(f"P90 speedup: {sorted(all_sp)[int(0.9*len(all_sp))]:.2f}x")
print(f"P95 speedup: {sorted(all_sp)[int(0.95*len(all_sp))]:.2f}x")
print(f"Max speedup: {max(all_sp):.2f}x")
print(f"Min speedup: {min(all_sp):.2f}x")

cvs = [s['cv'] for s in speedup_stats]
print(f"\nRepeatability (Coefficient of Variation):")
print(f"  Mean CV: {statistics.mean(cvs):.1f}%")
print(f"  Median CV: {statistics.median(cvs):.1f}%")
print(f"  Max CV: {max(cvs):.1f}%")
print(f"  % scenarios with CV < 5%: {sum(1 for c in cvs if c < 5) / len(cvs) * 100:.1f}%")
print(f"  % scenarios with CV < 10%: {sum(1 for c in cvs if c < 10) / len(cvs) * 100:.1f}%")
print(f"  % scenarios with CV < 20%: {sum(1 for c in cvs if c < 20) / len(cvs) * 100:.1f}%")

# Identify peak speedup scenarios
peak = max(speedup_stats, key=lambda x: x['avg'])
print(f"\nPeak speedup scenario:")
print(f"  Threads={peak['t']:d}, Shards={peak['s']:d}, Write%={peak['w']:d}, "
      f"Skew={peak['sk']:s}")
print(f"  Mean speedup: {peak['avg']:.2f}x (±{peak['std']:.3f} σ, {peak['cv']:.1f}% CV)")
