#!/usr/bin/env python3
"""Analyze MCINSPECTOR P0 hardware-campaign run sheets.

This tool never promotes a candidate that fails correctness, has insufficient
samples, or trades a lower median for an unreviewed tail regression. It uses
nearest-rank p50/p95/p99/max from raw operation_ticks and reports a Pareto
frontier across those four latency metrics.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

PASS_WORDS = {"1", "ok", "pass", "passed", "success", "verified", "true"}
FAIL_WORDS = {"0", "fail", "failed", "error", "false", "corrupt", "mismatch"}
ERROR_FIELDS = (
    "filexio_errors",
    "mc_errors",
    "sif_errors",
    "errors",
    "hangs",
    "resets",
    "reset",
    "hang",
)
COUNTER_FIELDS = (
    "rpc_calls",
    "rpc_ticks",
    "async_submits",
    "async_ready",
    "async_waits",
    "async_wait_ticks",
)


def parse_int(text: str | None) -> int | None:
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def truthy_failure(text: str | None) -> bool:
    if text is None:
        return False
    value = text.strip().lower()
    if not value:
        return False
    number = parse_int(value)
    if number is not None:
        return number != 0
    return value in {"yes", "y", "true", "error", "fail", "failed", "hang", "reset"}


def verify_passes(text: str | None) -> bool:
    if text is None:
        return False
    value = text.strip().lower()
    if not value:
        return False
    if value in PASS_WORDS:
        return True
    if value in FAIL_WORDS:
        return False
    number = parse_int(value)
    return number is not None and number > 0


def nearest_rank(values: Sequence[int], q: float) -> int:
    ordered = sorted(values)
    rank = max(1, math.ceil(q * len(ordered)))
    return ordered[rank - 1]


def candidate_key(row: Dict[str, str], stage: int) -> str:
    variant = (row.get("variant") or "").strip()
    if stage == 4:
        conflict = (row.get("conflict_batch") or "?").strip()
        pages = (row.get("raw_pages") or "?").strip()
        async_mode = (row.get("raw_async") or "?").strip()
        return f"conflict{conflict}-raw{pages}-async{async_mode}"
    if variant:
        return variant

    bits = []
    for field in ("conflict_batch", "raw_pages", "raw_async"):
        value = (row.get(field) or "").strip()
        if value:
            bits.append(f"{field}={value}")
    return ",".join(bits) or "unnamed"


@dataclass
class Candidate:
    key: str
    n: int
    p50: int | None
    p95: int | None
    p99: int | None
    maximum: int | None
    correctness_hashes: List[str]
    verify_failures: int
    error_rows: int
    missing_hash_rows: int
    counters: Dict[str, int]
    qualified: bool
    reasons: List[str]

    @property
    def metrics(self) -> Tuple[int, int, int, int]:
        assert self.p50 is not None
        assert self.p95 is not None
        assert self.p99 is not None
        assert self.maximum is not None
        return (self.p50, self.p95, self.p99, self.maximum)


def analyze_rows(
    rows: Iterable[Dict[str, str]],
    stage: int,
    min_samples: int,
    expected_hash: str | None,
) -> List[Candidate]:
    grouped: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in rows:
        if parse_int(row.get("operation_ticks")) is not None:
            grouped[candidate_key(row, stage)].append(row)

    result: List[Candidate] = []
    for key, group in sorted(grouped.items()):
        ticks = [parse_int(row.get("operation_ticks")) for row in group]
        values = [value for value in ticks if value is not None and value > 0]
        hashes = sorted(
            {
                (row.get("correctness_hash") or "").strip().lower()
                for row in group
                if (row.get("correctness_hash") or "").strip()
            }
        )
        missing_hash = sum(
            1 for row in group if not (row.get("correctness_hash") or "").strip()
        )
        verify_failures = sum(
            1 for row in group if not verify_passes(row.get("verify_result"))
        )
        error_rows = sum(
            1
            for row in group
            if any(
                truthy_failure(row.get(field))
                for field in ERROR_FIELDS
                if field in row
            )
        )

        counters: Dict[str, int] = {}
        for field in COUNTER_FIELDS:
            parsed = [parse_int(row.get(field)) for row in group]
            counters[field] = sum(value for value in parsed if value is not None)

        reasons: List[str] = []
        if len(values) < min_samples:
            reasons.append(f"samples {len(values)} < required {min_samples}")
        if missing_hash:
            reasons.append(f"{missing_hash} row(s) missing correctness_hash")
        if len(hashes) != 1:
            reasons.append(
                f"candidate has {len(hashes)} distinct correctness hashes"
            )
        if expected_hash and hashes and hashes[0] != expected_hash.lower():
            reasons.append(
                f"hash {hashes[0]} != expected {expected_hash.lower()}"
            )
        if verify_failures:
            reasons.append(f"{verify_failures} verify failure(s)")
        if error_rows:
            reasons.append(
                f"{error_rows} row(s) report I/O/SIF/hang/reset errors"
            )

        result.append(
            Candidate(
                key=key,
                n=len(values),
                p50=nearest_rank(values, 0.50) if values else None,
                p95=nearest_rank(values, 0.95) if values else None,
                p99=nearest_rank(values, 0.99) if values else None,
                maximum=max(values) if values else None,
                correctness_hashes=hashes,
                verify_failures=verify_failures,
                error_rows=error_rows,
                missing_hash_rows=missing_hash,
                counters=counters,
                qualified=not reasons,
                reasons=reasons,
            )
        )
    return result


def apply_cross_candidate_hash_gate(
    candidates: List[Candidate], expected_hash: str | None
) -> None:
    if expected_hash:
        return

    hashes = {
        candidate.correctness_hashes[0]
        for candidate in candidates
        if candidate.qualified and len(candidate.correctness_hashes) == 1
    }
    if len(hashes) <= 1:
        return

    for candidate in candidates:
        if candidate.qualified:
            candidate.qualified = False
            candidate.reasons.append(
                "qualified candidates disagree on correctness_hash"
            )


def dominates(a: Candidate, b: Candidate) -> bool:
    am = a.metrics
    bm = b.metrics
    return all(x <= y for x, y in zip(am, bm)) and any(
        x < y for x, y in zip(am, bm)
    )


def pareto_frontier(candidates: List[Candidate]) -> List[Candidate]:
    qualified = [
        candidate for candidate in candidates if candidate.qualified and candidate.n > 0
    ]
    return [
        candidate
        for candidate in qualified
        if not any(
            dominates(other, candidate)
            for other in qualified
            if other is not candidate
        )
    ]


def print_report(
    candidates: List[Candidate],
    frontier: List[Candidate],
    stage: int,
    min_samples: int,
) -> None:
    print(f"P0 stage {stage}: minimum samples={min_samples}")
    print("candidate | n | p50 | p95 | p99 | max | status")

    for candidate in candidates:
        status = (
            "QUALIFIED"
            if candidate.qualified
            else "REJECTED: " + "; ".join(candidate.reasons)
        )
        print(
            f"{candidate.key} | {candidate.n} | "
            f"{candidate.p50 or '-'} | {candidate.p95 or '-'} | "
            f"{candidate.p99 or '-'} | {candidate.maximum or '-'} | {status}"
        )
        useful = {
            key: value for key, value in candidate.counters.items() if value
        }
        if useful:
            print(
                "  counters: "
                + " ".join(
                    f"{key}={value}" for key, value in sorted(useful.items())
                )
            )

    if not frontier:
        print("\nPROMOTION: BLOCKED, no correctness-qualified candidate.")
        return

    if len(frontier) == 1:
        print(f"\nPROMOTION CANDIDATE: {frontier[0].key}")
        print(
            "It is the sole non-dominated candidate across p50/p95/p99/max. "
            "Hardware metadata and workload equivalence still remain mandatory."
        )
        return

    fastest = min(
        frontier,
        key=lambda candidate: (
            candidate.p50 if candidate.p50 is not None else sys.maxsize
        ),
    )
    print("\nPROMOTION: MANUAL REVIEW REQUIRED")
    print("Pareto frontier: " + ", ".join(candidate.key for candidate in frontier))
    print(
        f"Lowest p50 on frontier: {fastest.key}. At least one tail metric "
        "trades off, so the tool refuses a median-only promotion."
    )


def self_test() -> int:
    rows = []
    for variant, sequence in (
        ("A", [100, 101, 99, 102, 100]),
        ("B", [95, 96, 97, 130, 140]),
    ):
        for index, ticks in enumerate(sequence):
            rows.append(
                {
                    "variant": variant,
                    "operation_ticks": str(ticks),
                    "correctness_hash": "deadbeef",
                    "verify_result": "PASS",
                    "run_index": str(index + 1),
                }
            )

    candidates = analyze_rows(rows, 2, 5, None)
    apply_cross_candidate_hash_gate(candidates, None)
    frontier = pareto_frontier(candidates)
    assert {candidate.key for candidate in frontier} == {"A", "B"}

    rows[0]["verify_result"] = "FAIL"
    candidates = analyze_rows(rows, 2, 5, None)
    candidate_a = next(candidate for candidate in candidates if candidate.key == "A")
    assert not candidate_a.qualified
    assert candidate_a.verify_failures == 1

    for row in rows:
        row["verify_result"] = "PASS"
        if row["variant"] == "B":
            row["correctness_hash"] = "cafebabe"
    candidates = analyze_rows(rows, 2, 5, None)
    apply_cross_candidate_hash_gate(candidates, None)
    assert not any(candidate.qualified for candidate in candidates)

    print("self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "csv", nargs="*", type=Path, help="filled campaign CSV run sheets"
    )
    parser.add_argument(
        "--stage",
        type=int,
        choices=range(1, 6),
        default=0,
        help="campaign stage; auto-detected from filename when omitted",
    )
    parser.add_argument(
        "--min-samples",
        type=int,
        default=0,
        help="override gate (defaults: 5 for stages 1-4, 20 for stage 5)",
    )
    parser.add_argument(
        "--expected-hash", help="require this correctness hash"
    )
    parser.add_argument(
        "--json", type=Path, help="write machine-readable summary"
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.csv:
        parser.error("at least one CSV is required unless --self-test is used")

    all_candidates: List[Candidate] = []
    json_payload = []

    for path in args.csv:
        stage = args.stage
        if not stage:
            match = re.search(r"STAGE([1-5])", path.name.upper())
            if not match:
                raise SystemExit(
                    f"cannot infer stage from {path}; pass --stage"
                )
            stage = int(match.group(1))

        min_samples = args.min_samples or (20 if stage == 5 else 5)
        with path.open(newline="", encoding="utf-8-sig") as handle:
            rows = list(csv.DictReader(handle))

        candidates = analyze_rows(
            rows, stage, min_samples, args.expected_hash
        )
        apply_cross_candidate_hash_gate(candidates, args.expected_hash)
        frontier = pareto_frontier(candidates)

        print(f"\n== {path} ==")
        print_report(candidates, frontier, stage, min_samples)
        all_candidates.extend(candidates)
        json_payload.append(
            {
                "file": str(path),
                "stage": stage,
                "min_samples": min_samples,
                "frontier": [candidate.key for candidate in frontier],
                "candidates": [asdict(candidate) for candidate in candidates],
            }
        )

    if args.json:
        args.json.write_text(
            json.dumps(json_payload, indent=2) + "\n", encoding="utf-8"
        )
        print(f"\nJSON: {args.json}")

    return (
        0
        if all_candidates and all(candidate.qualified for candidate in all_candidates)
        else 3
    )


if __name__ == "__main__":
    raise SystemExit(main())
