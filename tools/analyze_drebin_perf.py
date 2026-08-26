#!/usr/bin/env python3
"""Summarize Drebin P0 transport telemetry from one or more DREBIN.LOG files.

The EE deliberately emits compact key=value checkpoints instead of formatting a
large benchmark table on-console. This host tool groups each raw-card session and
reports the quantities needed by the v2 optimization corpus: setup/active/total
wall time, raw SIF batch latency, async ready/wait behaviour, USB read/write
transaction counts and tails, plus final image CRC/verification state.

Examples:
    python3 tools/analyze_drebin_perf.py DREBIN.LOG
    python3 tools/analyze_drebin_perf.py logs/*.LOG --csv p0.csv

Do not compare rows unless console/card/USB device/workload are otherwise held
constant. A faster row whose final image is not verified or whose CRC differs
from the baseline is not a performance win.
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional

TAG_RE = re.compile(r"\[([A-Z0-9_-]+)\]")
KV_RE = re.compile(r"\b([A-Za-z0-9_]+)=([^\s]+)")
CRC_RE = re.compile(r"\bcrc=([0-9A-Fa-f]{8})\b")
VERIFIED_RE = re.compile(r"\bverified=(-?\d+)\b")
RESULT_RE = re.compile(r"\bresult=([^\s]+(?:\s+[^\s]+)*)\s+port=")


def parse_int(value: Optional[str]) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def parse_kv(line: str) -> Dict[str, str]:
    return {match.group(1): match.group(2).rstrip(",;") for match in KV_RE.finditer(line)}


@dataclass
class Session:
    source: str
    ordinal: int
    setup: Dict[str, str] = field(default_factory=dict)
    stop: Dict[str, str] = field(default_factory=dict)
    raw: Dict[str, str] = field(default_factory=dict)
    read_io: Dict[str, str] = field(default_factory=dict)
    write_io: Dict[str, str] = field(default_factory=dict)
    image: Dict[str, str] = field(default_factory=dict)
    image_result_text: str = ""
    image_crc: str = ""
    image_verified: str = ""

    def value(self, mapping: Dict[str, str], key: str) -> str:
        return mapping.get(key, "")

    def row(self) -> Dict[str, str]:
        rpc_calls = parse_int(self.raw.get("rpc_calls"))
        pages = parse_int(self.raw.get("pages"))
        submits = parse_int(self.raw.get("async_submit"))
        ready = parse_int(self.raw.get("ready"))
        waits = parse_int(self.raw.get("waits"))

        pages_per_rpc = ""
        if rpc_calls and pages is not None:
            pages_per_rpc = f"{pages / rpc_calls:.3f}"

        ready_ratio = ""
        if submits and ready is not None:
            ready_ratio = f"{ready / submits:.4f}"

        wait_ratio = ""
        if submits and waits is not None:
            wait_ratio = f"{waits / submits:.4f}"

        return {
            "source": self.source,
            "session": str(self.ordinal),
            "setup_us": self.stop.get("setup_us", self.setup.get("setup_us", "")),
            "active_us": self.stop.get("active_us", ""),
            "total_us": self.stop.get("total_us", ""),
            "raw_batch_pages": self.raw.get("batch_pages", ""),
            "raw_async": self.raw.get("async", ""),
            "raw_page_limit": self.raw.get("page_limit", ""),
            "raw_rpc_calls": self.raw.get("rpc_calls", ""),
            "raw_pages": self.raw.get("pages", ""),
            "raw_pages_per_rpc": pages_per_rpc,
            "raw_cache_hits": self.raw.get("hits", ""),
            "raw_fallbacks": self.raw.get("fallbacks", ""),
            "raw_ecc_warn": self.raw.get("ecc_warn", ""),
            "raw_async_submits": self.raw.get("async_submit", ""),
            "raw_async_ready": self.raw.get("ready", ""),
            "raw_async_waits": self.raw.get("waits", ""),
            "raw_async_discards": self.raw.get("discards", ""),
            "raw_async_ready_ratio": ready_ratio,
            "raw_async_wait_ratio": wait_ratio,
            "raw_rpc_p50_ms_floor": self.raw.get("rpc_p50_ms_floor", ""),
            "raw_rpc_p95_ms_floor": self.raw.get("rpc_p95_ms_floor", ""),
            "raw_rpc_p99_ms_floor": self.raw.get("rpc_p99_ms_floor", ""),
            "raw_rpc_max_us": self.raw.get("rpc_max_us", ""),
            "raw_wait_p95_ms_floor": self.raw.get("wait_p95_ms_floor", ""),
            "raw_wait_p99_ms_floor": self.raw.get("wait_p99_ms_floor", ""),
            "raw_wait_max_us": self.raw.get("wait_max_us", ""),
            "raw_rpc_ticks": self.raw.get("rpc_ticks", ""),
            "raw_wait_ticks": self.raw.get("wait_ticks", ""),
            "read_batch_pages": self.read_io.get("batch_pages", ""),
            "read_logical_calls": self.read_io.get("logical_calls", ""),
            "read_logical_bytes": self.read_io.get("logical_bytes", ""),
            "read_underlying_calls": self.read_io.get("underlying_calls", ""),
            "read_underlying_bytes": self.read_io.get("underlying_bytes", ""),
            "read_underlying_ticks": self.read_io.get("underlying_ticks", ""),
            "read_operation_ticks": self.read_io.get("operation_ticks", ""),
            "read_p50_ms_floor": self.read_io.get("batch_p50_ms_floor", ""),
            "read_p95_ms_floor": self.read_io.get("batch_p95_ms_floor", ""),
            "read_p99_ms_floor": self.read_io.get("batch_p99_ms_floor", ""),
            "read_max_us": self.read_io.get("batch_max_us", ""),
            "write_batch_pages": self.write_io.get("batch_pages", ""),
            "write_async": self.write_io.get("async", ""),
            "write_logical_calls": self.write_io.get("logical_calls", ""),
            "write_logical_bytes": self.write_io.get("logical_bytes", ""),
            "write_underlying_calls": self.write_io.get("underlying_calls", ""),
            "write_underlying_bytes": self.write_io.get("underlying_bytes", ""),
            "write_underlying_ticks": self.write_io.get("underlying_ticks", ""),
            "write_operation_ticks": self.write_io.get("operation_ticks", ""),
            "write_async_submits": self.write_io.get("async_submit", ""),
            "write_async_ready": self.write_io.get("ready", ""),
            "write_async_waits": self.write_io.get("waits", ""),
            "write_p50_ms_floor": self.write_io.get("batch_p50_ms_floor", ""),
            "write_p95_ms_floor": self.write_io.get("batch_p95_ms_floor", ""),
            "write_p99_ms_floor": self.write_io.get("batch_p99_ms_floor", ""),
            "write_max_us": self.write_io.get("batch_max_us", ""),
            "image_crc": self.image_crc,
            "image_verified": self.image_verified,
            "image_result": self.image_result_text,
        }


def classify_image_io(line: str, kv: Dict[str, str], current: Session) -> None:
    if "read-ahead end" in line:
        current.read_io = kv
    elif "write-behind end" in line:
        current.write_io = kv


def parse_file(path: Path) -> List[Session]:
    sessions: List[Session] = []
    current: Optional[Session] = None
    ordinal = 0

    text = path.read_text(errors="replace")
    for line in text.splitlines():
        tag_match = TAG_RE.search(line)
        tag = tag_match.group(1) if tag_match else ""
        kv = parse_kv(line)

        if tag == "RAW-PERF" and "session ready" in line:
            ordinal += 1
            current = Session(path.name, ordinal, setup=kv)
            sessions.append(current)
            continue

        if current is None:
            continue

        if tag == "RAW-BULK-PERF":
            current.raw = kv
        elif tag == "RAW-PERF" and "session stop" in line:
            current.stop = kv
        elif tag == "IMAGE-IO":
            classify_image_io(line, kv, current)
        elif tag == "IMAGE" and " export end " in f" {line} ":
            current.image = kv
            crc = CRC_RE.search(line)
            verified = VERIFIED_RE.search(line)
            result = RESULT_RE.search(line)
            if crc:
                current.image_crc = crc.group(1).upper()
            if verified:
                current.image_verified = verified.group(1)
            if result:
                current.image_result_text = result.group(1)

    return sessions


def fmt_seconds(us_text: str) -> str:
    value = parse_int(us_text)
    if value is None:
        return "?"
    return f"{value / 1_000_000.0:.3f}s"


def print_summary(sessions: Iterable[Session]) -> None:
    for session in sessions:
        row = session.row()
        print(
            f"{row['source']} #{row['session']}: "
            f"setup={fmt_seconds(row['setup_us'])} "
            f"active={fmt_seconds(row['active_us'])} "
            f"total={fmt_seconds(row['total_us'])} | "
            f"raw={row['raw_batch_pages'] or '?'}p/async{row['raw_async'] or '?'} "
            f"rpc={row['raw_rpc_calls'] or '?'} "
            f"pages/rpc={row['raw_pages_per_rpc'] or '?'} "
            f"p95={row['raw_rpc_p95_ms_floor'] or '?'}ms+ "
            f"p99={row['raw_rpc_p99_ms_floor'] or '?'}ms+ "
            f"max={row['raw_rpc_max_us'] or '?'}us | "
            f"ready/wait={row['raw_async_ready'] or '0'}/{row['raw_async_waits'] or '0'} | "
            f"read={row['read_batch_pages'] or '?'}p/{row['read_underlying_calls'] or '?'}calls | "
            f"write={row['write_batch_pages'] or '?'}p/async{row['write_async'] or '?'}"
            f"/{row['write_underlying_calls'] or '?'}calls | "
            f"crc={row['image_crc'] or '?'} verified={row['image_verified'] or '?'}"
        )


def write_csv(path: Path, sessions: List[Session]) -> None:
    rows = [session.row() for session in sessions]
    if not rows:
        path.write_text("")
        return
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--csv", type=Path, help="write machine-readable session table")
    args = parser.parse_args()

    sessions: List[Session] = []
    for log in args.logs:
        sessions.extend(parse_file(log))

    if not sessions:
        print("No [RAW-PERF] sessions found.")
        return 2

    print_summary(sessions)
    if args.csv:
        write_csv(args.csv, sessions)
        print(f"CSV: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
