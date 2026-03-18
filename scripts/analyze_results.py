#!/usr/bin/env python3
"""Aggregate DBWaller benchmark JSON outputs into normalized CSV files.

Usage:
  scripts/analyze_results.py --run-id <run_id>
  scripts/analyze_results.py --latest
  scripts/analyze_results.py --input-dir results/raw/<run_id>
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Aggregate DBWaller benchmark JSON outputs")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--run-id", help="Run id under results/raw/")
    group.add_argument("--latest", action="store_true", help="Use the latest run in results/raw/")
    group.add_argument("--input-dir", help="Explicit input directory containing baseline.json/sharded.json")

    parser.add_argument(
        "--root",
        default=".",
        help="Repository root directory (default: current working directory)",
    )
    parser.add_argument(
        "--output",
        help="Optional output CSV path (default: results/processed/<run_id>.csv)",
    )
    return parser.parse_args()


def latest_run_dir(raw_root: Path) -> Path:
    candidates = [d for d in raw_root.iterdir() if d.is_dir()]
    if not candidates:
        raise FileNotFoundError(f"No run directories found in {raw_root}")
    return sorted(candidates, key=lambda p: p.name)[-1]


def read_json(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def to_int(value: Any) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(round(float(value)))
    except (TypeError, ValueError):
        return None


def to_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def parse_name_fields(name: str) -> Dict[str, Optional[str]]:
    # Example style from Google Benchmark: BenchmarkName/arg0/arg1/.../threads:8
    parts = name.split("/")
    bench_name = parts[0] if parts else name

    thread_text: Optional[str] = None
    for p in parts:
        if p.startswith("threads:"):
            thread_text = p.split(":", 1)[1]
            break

    arg_values: List[str] = []
    for p in parts[1:]:
        if p.startswith("threads:"):
            continue
        arg_values.append(p)

    return {
        "benchmark_name": bench_name,
        "arg0": arg_values[0] if len(arg_values) > 0 else None,
        "arg1": arg_values[1] if len(arg_values) > 1 else None,
        "arg2": arg_values[2] if len(arg_values) > 2 else None,
        "arg3": arg_values[3] if len(arg_values) > 3 else None,
        "arg4": arg_values[4] if len(arg_values) > 4 else None,
        "threads_from_name": thread_text,
    }


def normalize_rows(data: Dict[str, Any], impl_label: str, run_id: str) -> Iterable[Dict[str, Any]]:
    rows = data.get("benchmarks", [])
    for row in rows:
        if row.get("run_type") == "iteration":
            # Keep outputs compact and stable by defaulting to aggregate rows.
            continue

        name = str(row.get("name", ""))
        parsed = parse_name_fields(name)

        counters = dict(row)
        out: Dict[str, Any] = {
            "run_id": run_id,
            "impl": impl_label,
            "benchmark_name": parsed["benchmark_name"],
            "name": name,
            "aggregate_name": row.get("aggregate_name", ""),
            "run_type": row.get("run_type", ""),
            "repetitions": to_int(row.get("repetitions")),
            "real_time": to_float(row.get("real_time")),
            "cpu_time": to_float(row.get("cpu_time")),
            "time_unit": row.get("time_unit", ""),
            "threads": to_int(counters.get("threads")) or to_int(parsed["threads_from_name"]),
            "shards": to_int(counters.get("shards")) or to_int(parsed["arg0"]),
            "keys": to_int(counters.get("keys")) or to_int(parsed["arg1"]),
            "ops": to_int(counters.get("ops")) or to_int(parsed["arg2"]),
            "write_percent": to_int(counters.get("write_percent")) or to_int(parsed["arg3"]),
            "skew_mode": to_int(counters.get("skew_mode")) or to_int(parsed["arg4"]),
            "read_percent": to_int(counters.get("read_percent")),
            "reads": to_int(counters.get("reads")),
            "writes": to_int(counters.get("writes")),
            "baseline_ops_per_sec": to_float(counters.get("baseline_ops_per_sec")),
            "dbwaller_ops_per_sec": to_float(counters.get("dbwaller_ops_per_sec")),
            "cycles": to_float(counters.get("CYCLES")),
            "instructions": to_float(counters.get("INSTRUCTIONS")),
            "source_file": "",
        }

        yield out


def write_csv(rows: List[Dict[str, Any]], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    columns = [
        "run_id",
        "impl",
        "benchmark_name",
        "name",
        "aggregate_name",
        "run_type",
        "repetitions",
        "real_time",
        "cpu_time",
        "time_unit",
        "threads",
        "shards",
        "keys",
        "ops",
        "write_percent",
        "read_percent",
        "skew_mode",
        "reads",
        "writes",
        "baseline_ops_per_sec",
        "dbwaller_ops_per_sec",
        "cycles",
        "instructions",
        "source_file",
    ]

    with output_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for r in rows:
            writer.writerow(r)


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()

    if args.input_dir:
        run_dir = Path(args.input_dir).resolve()
        run_id = run_dir.name
    else:
        raw_root = root / "results" / "raw"
        if args.latest:
            run_dir = latest_run_dir(raw_root)
            run_id = run_dir.name
        else:
            run_id = str(args.run_id)
            run_dir = raw_root / run_id

    if not run_dir.exists():
        raise FileNotFoundError(f"Run directory does not exist: {run_dir}")

    baseline_path = run_dir / "baseline.json"
    sharded_path = run_dir / "sharded.json"

    baseline = read_json(baseline_path)
    sharded = read_json(sharded_path)

    rows: List[Dict[str, Any]] = []
    for row in normalize_rows(baseline, "baseline", run_id):
        row["source_file"] = str(baseline_path)
        rows.append(row)

    for row in normalize_rows(sharded, "sharded", run_id):
        row["source_file"] = str(sharded_path)
        rows.append(row)

    rows.sort(
        key=lambda r: (
            r.get("impl") or "",
            r.get("benchmark_name") or "",
            int(r.get("shards") or 0),
            int(r.get("threads") or 0),
            int(r.get("write_percent") or 0),
            int(r.get("skew_mode") or 0),
            str(r.get("aggregate_name") or ""),
        )
    )

    default_out = root / "results" / "processed" / f"{run_id}.csv"
    output_path = Path(args.output).resolve() if args.output else default_out

    write_csv(rows, output_path)

    summary = {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "run_id": run_id,
        "input_dir": str(run_dir),
        "output_csv": str(output_path),
        "row_count": len(rows),
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
