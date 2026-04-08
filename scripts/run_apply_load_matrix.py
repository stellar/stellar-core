#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_STELLAR_CORE_BIN = SCRIPT_DIR.parent / "src" / "stellar-core"
DEFAULT_TEMPLATE_CONFIG = SCRIPT_DIR.parent / "docs" / "apply-load-benchmark-sac.cfg"
DEFAULT_OUTPUT_ROOT = Path.home() / "apply-load"
APPLY_LOAD_NUM_LEDGERS = 200

FLOAT_RE = r"([-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)"
RESULT_PATTERNS = {
    "median_time_ms": re.compile(rf"p50 close time:\s+{FLOAT_RE}\s+ms"),
    "p95_time_ms": re.compile(rf"p95 close time:\s+{FLOAT_RE}\s+ms"),
    "p99_time_ms": re.compile(rf"p99 close time:\s+{FLOAT_RE}\s+ms"),
}


@dataclass(frozen=True, slots=True)
class Scenario:
    model_tx: str
    tx_count: int
    thread_count: int
    time_writes: bool = True
    disable_metrics: bool = True
    sac_batch_size: int = 1

    def __post_init__(self) -> None:
        if self.sac_batch_size <= 0:
            raise ValueError("sac_batch_size must be positive")

        if self.model_tx == "sac":
            if self.sac_batch_size <= 0:
                raise ValueError(
                    f"Scenario '{self.identifier()}' must define a positive SAC batch size"
                )
        elif self.sac_batch_size != 1:
            raise ValueError(
                "sac_batch_size can only differ from 1 for model_tx='sac'"
            )

    def identifier(self) -> str:
        parts = [self.model_tx, f"TX={self.tx_count}", f"T={self.thread_count}"]
        if not self.time_writes:
            parts.append("TW=0")
        if not self.disable_metrics:
            parts.append("DM=0")
        if self.model_tx == "sac" and self.sac_batch_size != 1:
            parts.append(f"B={self.sac_batch_size}")
        return ",".join(parts)

    def slug(self) -> str:
        return re.sub(r"[^a-z0-9]+", "-", self.identifier().lower()).strip("-")

    def summary(self) -> str:
        return self.identifier()


SCENARIOS: tuple[Scenario, ...] = (
    Scenario(
        model_tx="sac",
        tx_count=6400,
        thread_count=1,
    ),
    Scenario(
        model_tx="sac",
        tx_count=6400,
        thread_count=8,
    ),
    Scenario(
        model_tx="custom_token",
        tx_count=3000,
        thread_count=1,
    ),
    Scenario(
        model_tx="custom_token",
        tx_count=3000,
        thread_count=8,
    ),
    Scenario(
        model_tx="soroswap",
        tx_count=1600,
        thread_count=1,
    ),
    Scenario(
        model_tx="soroswap",
        tx_count=1600,
        thread_count=8,
    ),
)


def validate_scenarios(scenarios: tuple[Scenario, ...]) -> None:
    seen_identifiers: set[str] = set()
    for scenario in scenarios:
        identifier = scenario.identifier()
        if identifier in seen_identifiers:
            raise ValueError(f"Duplicate scenario identifier: {identifier}")
        seen_identifiers.add(identifier)

        if scenario.model_tx != "sac":
            continue

        if scenario.tx_count % scenario.sac_batch_size != 0:
            raise ValueError(
                "Invalid SAC scenario "
                f"{identifier}: TX must be divisible by B"
            )

        sac_tx_envelopes = scenario.tx_count // scenario.sac_batch_size
        if sac_tx_envelopes < scenario.thread_count:
            raise ValueError(
                "Invalid SAC scenario "
                f"{identifier}: TX / B must be at least T"
            )

        if scenario.sac_batch_size > 1 and sac_tx_envelopes % scenario.thread_count != 0:
            raise ValueError(
                "Invalid SAC scenario "
                f"{identifier}: TX / B must be divisible by T when B > 1"
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a fixed matrix of apply-load scenarios and emit a CSV summary.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--stellar-core-bin",
        type=Path,
        default=DEFAULT_STELLAR_CORE_BIN,
        help="Path to the stellar-core executable to run.",
    )
    parser.add_argument(
        "--template-config",
        type=Path,
        default=DEFAULT_TEMPLATE_CONFIG,
        help="Path to the benchmark apply-load template config.",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=DEFAULT_OUTPUT_ROOT,
        help="Directory where apply-load/<run-id>/ outputs should be written.",
    )
    parser.add_argument(
        "--build-tag",
        help="Optional build tag to embed in the run identifier. Defaults to a hash of `stellar-core version` output.",
    )
    return parser.parse_args()


def bool_literal(value: bool) -> str:
    return "true" if value else "false"


def quoted(value: str) -> str:
    return f'"{value}"'


def sanitize_tag(tag: str) -> str:
    cleaned = re.sub(r"[^a-zA-Z0-9._-]+", "-", tag.strip()).strip("-._")
    if not cleaned:
        raise ValueError("Build tag is empty after sanitization")
    return cleaned.lower()


def run_command(command: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
    )


def get_version_string(stellar_core_bin: Path) -> str:
    result = run_command([str(stellar_core_bin), "version"], cwd=stellar_core_bin.parent)
    if result.returncode != 0:
        raise RuntimeError(
            "Failed to run `stellar-core version`:\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    version_parts = []
    if result.stderr.strip():
        version_parts.append(result.stderr.strip())
    if result.stdout.strip():
        version_parts.append(result.stdout.strip())
    version_text = "\n".join(version_parts)
    if not version_text:
        raise RuntimeError("`stellar-core version` produced empty output")
    return version_text


def derive_build_tag(version_text: str, user_build_tag: str | None) -> str:
    if user_build_tag:
        return sanitize_tag(user_build_tag)
    version_hash = hashlib.sha256(version_text.encode("utf-8")).hexdigest()[:12]
    return version_hash


def create_run_id(build_tag: str) -> str:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    return f"{build_tag}-{timestamp}"


def read_template_config(template_config: Path) -> str:
    try:
        return template_config.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise FileNotFoundError(f"Template config not found: {template_config}") from exc


def apply_overrides(template_text: str, overrides: dict[str, str]) -> str:
    lines = template_text.splitlines()
    seen_keys: set[str] = set()
    rendered_lines: list[str] = []
    key_pattern = re.compile(r"^(\s*)([A-Z0-9_]+)\s*=.*$")
    section_pattern = re.compile(r"^\s*\[[^\]]+\]\s*$")
    first_section_index: int | None = None

    for line in lines:
        if first_section_index is None and section_pattern.match(line):
            first_section_index = len(rendered_lines)

        match = key_pattern.match(line)
        if match:
            indent, key = match.groups()
            if key in overrides:
                rendered_lines.append(f"{indent}{key} = {overrides[key]}")
                seen_keys.add(key)
                continue
        rendered_lines.append(line)

    missing_keys = [key for key in overrides if key not in seen_keys]
    if missing_keys:
        insertion_lines = ["# Overrides added by run_apply_load_matrix.py"]
        insertion_lines.extend(f"{key} = {overrides[key]}" for key in missing_keys)

        if first_section_index is None:
            if rendered_lines and rendered_lines[-1] != "":
                rendered_lines.append("")
            rendered_lines.extend(insertion_lines)
        else:
            if first_section_index > 0 and rendered_lines[first_section_index - 1] != "":
                insertion_lines.insert(0, "")
                first_section_index += 1
            insertion_lines.append("")
            rendered_lines[first_section_index:first_section_index] = insertion_lines

    return "\n".join(rendered_lines) + "\n"


def build_config_text(template_text: str, scenario: Scenario, log_name: str) -> str:
    overrides = {
        "APPLY_LOAD_MODEL_TX": quoted(scenario.model_tx),
        "APPLY_LOAD_MAX_SOROBAN_TX_COUNT": str(scenario.tx_count),
        "APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS": str(scenario.thread_count),
        "APPLY_LOAD_TIME_WRITES": bool_literal(scenario.time_writes),
        "DISABLE_SOROBAN_METRICS_FOR_TESTING": bool_literal(scenario.disable_metrics),
        "APPLY_LOAD_NUM_LEDGERS": str(APPLY_LOAD_NUM_LEDGERS),
        "LOG_FILE_PATH": quoted(log_name),
    }
    if scenario.model_tx == "sac":
        overrides["APPLY_LOAD_BATCH_SAC_COUNT"] = str(scenario.sac_batch_size)
    return apply_overrides(template_text, overrides)


def parse_benchmark_results(log_path: Path) -> dict[str, float]:
    log_text = log_path.read_text(encoding="utf-8")
    parsed: dict[str, float] = {}
    for field_name, pattern in RESULT_PATTERNS.items():
        matches = pattern.findall(log_text)
        if not matches:
            raise RuntimeError(
                f"Could not find `{field_name}` in benchmark log {log_path}"
            )
        parsed[field_name] = float(matches[-1])
    return parsed


def write_csv_header(results_csv: Path) -> None:
    with results_csv.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=["scenario", "median_time_ms", "p95_time_ms", "p99_time_ms"],
        )
        writer.writeheader()


def append_csv_row(results_csv: Path, row: dict[str, str | float]) -> None:
    with results_csv.open("a", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=["scenario", "median_time_ms", "p95_time_ms", "p99_time_ms"],
        )
        writer.writerow(row)


def ensure_inputs(stellar_core_bin: Path, template_config: Path) -> tuple[Path, Path]:
    stellar_core_bin = stellar_core_bin.expanduser().resolve()
    template_config = template_config.expanduser().resolve()

    if not stellar_core_bin.exists():
        raise FileNotFoundError(f"stellar-core binary not found: {stellar_core_bin}")
    if not stellar_core_bin.is_file():
        raise FileNotFoundError(f"stellar-core path is not a file: {stellar_core_bin}")
    if not template_config.exists():
        raise FileNotFoundError(f"Template config not found: {template_config}")

    return stellar_core_bin, template_config


def run_scenario(
    scenario_index: int,
    scenario: Scenario,
    *,
    stellar_core_bin: Path,
    template_text: str,
    run_id: str,
    logs_dir: Path,
) -> dict[str, float]:
    log_name = f"{run_id}-{scenario_index:02d}-{scenario.slug()}.log"
    with tempfile.TemporaryDirectory(prefix=f"apply-load-{scenario.slug()}-") as temp_dir:
        work_dir = Path(temp_dir)
        config_text = build_config_text(template_text, scenario, log_name)
        config_path = work_dir / "apply-load.cfg"
        config_path.write_text(config_text, encoding="utf-8")

        print(f"Running {scenario.summary()}")
        result = run_command(
            [str(stellar_core_bin), "--conf", str(config_path), "apply-load"],
            cwd=work_dir,
        )

        scenario_log = work_dir / log_name
        if scenario_log.exists():
            shutil.copy2(scenario_log, logs_dir / log_name)

        if result.returncode != 0:
            raise RuntimeError(
                f"Scenario '{scenario.identifier()}' failed with exit code {result.returncode}.\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

        if not scenario_log.exists():
            raise RuntimeError(
                f"Scenario '{scenario.identifier()}' completed but did not produce log file {log_name}"
            )

        return parse_benchmark_results(scenario_log)


def main() -> int:
    args = parse_args()

    try:
        stellar_core_bin, template_config = ensure_inputs(
            args.stellar_core_bin, args.template_config
        )
        scenarios = SCENARIOS
        validate_scenarios(scenarios)
        version_text = get_version_string(stellar_core_bin)
        build_tag = derive_build_tag(version_text, args.build_tag)
        run_id = create_run_id(build_tag)
        output_root = args.output_root.expanduser().resolve()
        run_dir = output_root / run_id
        logs_dir = run_dir / "logs"
        results_csv = run_dir / "results.csv"
        stamp_path = run_dir / "stamp"
        template_text = read_template_config(template_config)
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    try:
        logs_dir.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print(f"Error: run directory already exists: {run_dir}", file=sys.stderr)
        return 1

    stamp_path.write_text(version_text + "\n\n" + f"Benchmark ledgers={APPLY_LOAD_NUM_LEDGERS}", encoding="utf-8")
    write_csv_header(results_csv)

    print(f"Run ID: {run_id}")
    print(f"Version stamp: {stamp_path}")
    print(f"Results CSV: {results_csv}")

    try:
        for scenario_index, scenario in enumerate(scenarios, start=1):
            metrics = run_scenario(
            scenario_index,
                scenario,
                stellar_core_bin=stellar_core_bin,
                template_text=template_text,
                run_id=run_id,
                logs_dir=logs_dir,
            )
            append_csv_row(
                results_csv,
                {
                    "scenario": scenario.summary(),
                    "median_time_ms": metrics["median_time_ms"],
                    "p95_time_ms": metrics["p95_time_ms"],
                    "p99_time_ms": metrics["p99_time_ms"],
                },
            )
            print(
                "Captured "
                f"median={metrics['median_time_ms']}ms, "
                f"p95={metrics['p95_time_ms']}ms, "
                f"p99={metrics['p99_time_ms']}ms"
            )
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        print(f"Partial outputs retained in {run_dir}", file=sys.stderr)
        return 1

    print(f"Completed {len(scenarios)} scenario(s). Outputs written to {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())