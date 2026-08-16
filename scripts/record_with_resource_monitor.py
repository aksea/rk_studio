#!/usr/bin/env python3
"""Run rk_studio_cli record while collecting resource time series."""

from __future__ import annotations

import argparse
import csv
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


CLK_TCK = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
PAGE_SIZE = os.sysconf(os.sysconf_names["SC_PAGE_SIZE"])


def read_proc_stat(pid: int) -> dict[str, Any] | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError:
        return None
    end = text.rfind(")")
    if end < 0:
        return None
    fields = text[end + 2 :].split()
    if len(fields) < 22:
        return None
    try:
        utime = int(fields[11])
        stime = int(fields[12])
        rss_pages = int(fields[21])
    except ValueError:
        return None
    return {
        "cpu_ticks": utime + stime,
        "rss_mb": rss_pages * PAGE_SIZE / 1024.0 / 1024.0,
    }


def read_proc_status(pid: int) -> dict[str, Any]:
    result: dict[str, Any] = {}
    try:
        lines = Path(f"/proc/{pid}/status").read_text(encoding="utf-8").splitlines()
    except OSError:
        return result
    for line in lines:
        if line.startswith("VmHWM:"):
            result["rss_peak_mb"] = parse_status_kb(line)
        elif line.startswith("Threads:"):
            try:
                result["threads"] = int(line.split()[1])
            except (IndexError, ValueError):
                pass
    return result


def parse_status_kb(line: str) -> float | None:
    parts = line.split()
    if len(parts) < 2:
        return None
    try:
        return int(parts[1]) / 1024.0
    except ValueError:
        return None


def read_loadavg() -> tuple[float | None, float | None, float | None]:
    try:
        parts = Path("/proc/loadavg").read_text(encoding="utf-8").split()
        return float(parts[0]), float(parts[1]), float(parts[2])
    except (OSError, IndexError, ValueError):
        return None, None, None


def read_thermal() -> dict[str, float]:
    result: dict[str, float] = {}
    for path in sorted(Path("/sys/class/thermal").glob("thermal_zone*/temp")):
        try:
            milli_c = float(path.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            continue
        result[path.parent.name] = milli_c / 1000.0
    return result


def read_npu_load(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip().replace("\n", " | ")
    except OSError:
        return ""


def write_process_output(proc: subprocess.Popen[str], log_path: Path) -> None:
    assert proc.stdout is not None
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            log.write(line)
            log.flush()


def fmt(value: Any) -> Any:
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.3f}"
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run rk_studio_cli record and collect CPU/RSS/thermal/NPU time series.",
    )
    parser.add_argument("--duration", type=int, required=True, help="Recording duration in seconds.")
    parser.add_argument("--output", required=True, help="Recording output directory.")
    parser.add_argument(
        "--recognize",
        default="none",
        help="Recognition list passed to rk_studio_cli. Default: none.",
    )
    parser.add_argument(
        "--cli",
        default="./build/rk_studio_cli",
        help="Path to rk_studio_cli. Default: ./build/rk_studio_cli",
    )
    parser.add_argument(
        "--sample-interval",
        type=float,
        default=1.0,
        help="Resource sampling interval in seconds. Default: 1.0",
    )
    parser.add_argument(
        "--resource-csv",
        default=None,
        help="Resource CSV path. Default: <output>/resource_timeseries.csv",
    )
    parser.add_argument(
        "--cli-log",
        default=None,
        help="CLI log path. Default: <output>/rk_studio_cli.log",
    )
    parser.add_argument(
        "--npu-load-path",
        default="/sys/kernel/debug/rknpu/load",
        help="NPU load debug path. Default: /sys/kernel/debug/rknpu/load",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.duration <= 0:
        print("[resource-monitor] error: --duration must be positive", file=sys.stderr)
        return 2
    if args.sample_interval <= 0:
        print("[resource-monitor] error: --sample-interval must be positive", file=sys.stderr)
        return 2

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = Path(args.resource_csv) if args.resource_csv else output_dir / "resource_timeseries.csv"
    log_path = Path(args.cli_log) if args.cli_log else output_dir / "rk_studio_cli.log"
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    command = [
        args.cli,
        "record",
        "--duration",
        str(args.duration),
        "--output",
        str(output_dir),
        "--recognize",
        args.recognize,
    ]

    print(f"[resource-monitor] command: {' '.join(command)}")
    print(f"[resource-monitor] resource csv: {csv_path}")
    print(f"[resource-monitor] cli log: {log_path}")

    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    interrupted = False

    def handle_signal(signum: int, _frame: Any) -> None:
        nonlocal interrupted
        interrupted = True
        if proc.poll() is None:
            proc.send_signal(signum)

    old_int = signal.signal(signal.SIGINT, handle_signal)
    old_term = signal.signal(signal.SIGTERM, handle_signal)

    import threading

    output_thread = threading.Thread(target=write_process_output, args=(proc, log_path), daemon=True)
    output_thread.start()

    npu_path = Path(args.npu_load_path)
    prev_cpu_ticks: int | None = None
    prev_time: float | None = None
    start_time = time.monotonic()

    fieldnames = [
        "elapsed_s",
        "epoch_s",
        "pid",
        "cpu_percent",
        "rss_mb",
        "rss_peak_mb",
        "threads",
        "load1",
        "load5",
        "load15",
        "max_temp_c",
        "thermal_c",
        "npu_load",
    ]

    try:
        with csv_path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()

            while proc.poll() is None:
                now = time.monotonic()
                stat = read_proc_stat(proc.pid)
                status = read_proc_status(proc.pid)
                load1, load5, load15 = read_loadavg()
                thermal = read_thermal()
                max_temp = max(thermal.values()) if thermal else None
                npu_load = read_npu_load(npu_path) if npu_path.exists() else ""

                cpu_percent = None
                if stat is not None:
                    cpu_ticks = stat["cpu_ticks"]
                    if prev_cpu_ticks is not None and prev_time is not None and now > prev_time:
                        cpu_seconds = (cpu_ticks - prev_cpu_ticks) / CLK_TCK
                        cpu_percent = cpu_seconds / (now - prev_time) * 100.0
                    prev_cpu_ticks = cpu_ticks
                    prev_time = now

                row = {
                    "elapsed_s": fmt(now - start_time),
                    "epoch_s": fmt(time.time()),
                    "pid": proc.pid,
                    "cpu_percent": fmt(cpu_percent),
                    "rss_mb": fmt(stat.get("rss_mb") if stat else None),
                    "rss_peak_mb": fmt(status.get("rss_peak_mb")),
                    "threads": fmt(status.get("threads")),
                    "load1": fmt(load1),
                    "load5": fmt(load5),
                    "load15": fmt(load15),
                    "max_temp_c": fmt(max_temp),
                    "thermal_c": ";".join(f"{key}={value:.3f}" for key, value in thermal.items()),
                    "npu_load": npu_load,
                }
                writer.writerow(row)
                f.flush()
                time.sleep(args.sample_interval)
    finally:
        signal.signal(signal.SIGINT, old_int)
        signal.signal(signal.SIGTERM, old_term)

    output_thread.join(timeout=5)
    rc = proc.wait()
    if interrupted and rc == 0:
        return 130
    print(f"[resource-monitor] rk_studio_cli exited with code {rc}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
