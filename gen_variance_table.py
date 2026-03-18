#!/usr/bin/env python3
"""Generate cross-trial variance table."""

import csv
import glob
import os
import statistics

files = sorted(glob.glob('results/processed/*.csv'))
run_data = {}

for fpath in files:
    basename = os.path.basename(fpath)
    # Extract trial name
    if 'trial' in basename:
        trial = 'trial' + basename.split('trial')[1].split('.')[0]
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
                
                baseline_ops = float(row.get('baseline_ops_per_sec', '') or 0)
                dbwaller_ops = float(row.get('dbwaller_ops_per_sec', '') or 0)
                
                if baseline_ops > 0:
                    run_data[key][(trial, 0)] = baseline_ops
                if dbwaller_ops > 0:
                    run_data[key][(trial, 1)] = dbwaller_ops
            except (ValueError, KeyError, TypeError):
                pass

# Compute speedups
speedup_stats = []
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

with open('results/CROSS_TRIAL_VARIANCE_TABLE.txt', 'w') as out:
    out.write("CROSS-TRIAL VARIANCE TABLE (All 240 scenarios sorted by mean speedup)\n")
    out.write("=" * 90 + "\n")
    out.write("Threads Shards W%  Mode Runs   Mean    Min    Max  StdDev  CV%\n")
    out.write("-" * 90 + "\n")
    
    for s in speedup_stats:
        out.write(f"{s['t']:7d} {s['s']:7d} {s['w']:3d}  {s['sk']:>2s}   {s['n']:4d} "
                  f"{s['avg']:6.2f}x {s['min']:6.2f}x {s['max']:6.2f}x {s['std']:6.3f} {s['cv']:6.1f}%\n")
    
    out.write("\n" + "=" * 90 + "\n")
    out.write("Legend: T=Threads, S=Shards, W%=Write%, Mode: U=Uniform, H=Hotspot\n")
    out.write("Mean = average repeat speedup, Min/Max = range across trials\n")
    out.write("StdDev = standard deviation, CV% = coefficient of variation (repeatability)\n")

print("Saved to results/CROSS_TRIAL_VARIANCE_TABLE.txt")
