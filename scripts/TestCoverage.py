#!/usr/bin/env python3

from __future__ import annotations

import argparse
import math
import os
import random
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator, Sequence

import polars as pl
from joblib import Parallel, delayed
from pyroaring import BitMap
from tree_sitter import Language, Parser

import tree_sitter_cpp


@dataclass(frozen=True)
class TestInfo:
	name: str
	profraw_path: Path
	duration_ms: float
	covered_branch_ids: BitMap
	covered_line_ids: BitMap
	covered_function_ids: BitMap


def _empty_bitmap() -> BitMap:
	return BitMap()


def _eprint(*args: object) -> None:
	print(*args, file=sys.stderr)


def _check_exists(path: Path, *, what: str) -> None:
	if not path.exists():
		raise SystemExit(f"error: {what} does not exist: {path}")


def _run(
	cmd: Sequence[str],
	*,
	cwd: Path | None = None,
	stdout_path: Path | None = None,
	quiet: bool = False,
) -> None:
	if not quiet:
		_eprint("+", " ".join(cmd))
	try:
		if stdout_path is None:
			subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)
			return

		with stdout_path.open("wb") as out:
			subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True, stdout=out)
	except FileNotFoundError as e:
		raise SystemExit(
			f"error: command not found: {cmd[0]} (use --llvm-cov/--llvm-profdata to override)"
		) from e
	except subprocess.CalledProcessError as e:
		raise SystemExit(f"error: command failed ({e.returncode}): {' '.join(cmd)}") from e


def _compile_patterns(patterns: Sequence[str]) -> list[re.Pattern[str]]:
	compiled: list[re.Pattern[str]] = []
	for pattern in patterns:
		compiled.append(re.compile(pattern))
	return compiled


def _is_excluded(
	path: str,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> bool:
	# Include overrides exclude.
	if any(p.search(path) for p in include):
		return False
	return any(p.search(path) for p in exclude)


def _filter_lcov(
	*,
	input_lcov: Path,
	output_lcov: Path,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> None:
	_check_exists(input_lcov, what="LCOV coverage file")

	output_lcov.parent.mkdir(parents=True, exist_ok=True)
	current_sf: str | None = None
	current_included = True

	with input_lcov.open("r", encoding="utf-8", errors="replace") as src, output_lcov.open(
		"w", encoding="utf-8"
	) as dst:
		for raw in src:
			line = raw.rstrip("\n")
			if line.startswith("SF:"):
				current_sf = line[3:]
				current_included = not _is_excluded(current_sf, include=include, exclude=exclude)
			if current_included:
				dst.write(raw)
			if line == "end_of_record":
				current_sf = None
				current_included = True


def _safe_filename_fragment(name: str, *, limit: int = 80) -> str:
	s = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_")
	if not s:
		s = "test"
	return s[:limit]


def _read_test_list_csv(test_list: Path) -> list[tuple[str, Path, int]]:
	_check_exists(test_list, what="test list CSV")
	try:
		df = pl.read_csv(
			test_list,
			has_header=False,
			new_columns=["name", "profraw", "duration"],
			schema_overrides={"name": pl.Utf8, "profraw": pl.Utf8, "duration": pl.Int64},
		)
	except Exception as e:
		raise SystemExit(f"error: failed reading test list CSV {test_list}: {e}") from e

	rows: list[tuple[str, Path, int]] = []
	# Be forgiving about incidental whitespace.
	df = df.with_columns(
		pl.col("name").str.strip_chars(),
		pl.col("profraw").str.strip_chars(),
	)
	for name, profraw, duration in df.iter_rows():
		if name is None or profraw is None or duration is None:
			continue
		rows.append((str(name), Path(str(profraw)), int(duration)))
	if not rows:
		raise SystemExit(f"error: no tests found in {test_list}")
	return rows


def _read_candidates_csv(candidates_csv: Path) -> set[str]:
	_check_exists(candidates_csv, what="candidates CSV")
	try:
		df = pl.read_csv(
			candidates_csv,
			has_header=False,
			new_columns=["name"],
			schema_overrides={"name": pl.Utf8},
		)
	except Exception as e:
		raise SystemExit(f"error: failed reading candidates CSV {candidates_csv}: {e}") from e
	# Match prior behavior: strip whitespace and ignore blank names.
	names = (
		df.select(pl.col("name").str.strip_chars())
		.to_series()
		.drop_nulls()
		.to_list()
	)
	candidates = {n for n in (str(x) for x in names) if n}
	if not candidates:
		raise SystemExit(f"error: no candidates found in {candidates_csv}")
	return candidates


def _normalize_tag_name(tag: str) -> str:
	# Accept either "tag" or "[tag]".
	t = tag.strip()
	if t.startswith("[") and t.endswith("]") and len(t) >= 2:
		t = t[1:-1].strip()
	if not t:
		raise SystemExit("error: --slow-tag must be non-empty")
	if any(ch in t for ch in "[]"):
		raise SystemExit("error: --slow-tag must not contain '[' or ']' characters")
	return t


def _duration_us_to_ms(duration_us: int) -> float:
	# stellar-core profraw.csv durations are in microseconds.
	return float(duration_us) / 1000.0


def _merge_profraws_to_profdata(
	*,
	llvm_profdata: str,
	profraw_paths: Sequence[Path],
	output_profdata: Path,
	tmp_dir: Path,
) -> None:
	if not profraw_paths:
		raise SystemExit("error: no .profraw inputs to merge")

	for p in profraw_paths:
		_check_exists(p, what=".profraw file")

	# Try a single merge first; if it fails due to arg-length, fall back to
	# chunked incremental merges.
	base_cmd = [llvm_profdata, "merge", "-sparse"]
	cmd = [*base_cmd, *[str(p) for p in profraw_paths], "-o", str(output_profdata)]
	try:
		_run(cmd, quiet=True)
		return
	except OSError as e:
		if getattr(e, "errno", None) != 7:  # E2BIG
			raise

	_eprint("note: argument list too long; using chunked llvm-profdata merge")
	chunk_size = 200
	current: Path | None = None
	for chunk_start in range(0, len(profraw_paths), chunk_size):
		chunk = profraw_paths[chunk_start : chunk_start + chunk_size]
		next_out = tmp_dir / f"merge-{chunk_start}.profdata"
		inputs: list[str] = []
		if current is not None:
			inputs.append(str(current))
		inputs.extend(str(p) for p in chunk)
		_run([*base_cmd, *inputs, "-o", str(next_out)], quiet=True)
		current = next_out

	if current is None:
		raise SystemExit("error: failed to merge profiles")
	current.replace(output_profdata)


def _export_coverage_lcov(
	*,
	llvm_cov: str,
	binary: Path,
	profdata: Path,
	output_lcov: Path,
	quiet: bool,
) -> None:
	_check_exists(binary, what="coverage binary")
	_check_exists(profdata, what=".profdata file")

	cmd = [
		llvm_cov,
		"export",
		"-format=lcov",
		f"--instr-profile={profdata}",
		f"--object={binary}",
	]
	_run(cmd, stdout_path=output_lcov, quiet=quiet)


ElementKey = tuple[Any, ...]


def _parse_int(s: str) -> int:
	try:
		return int(s)
	except ValueError as e:
		raise SystemExit(f"error: invalid integer in LCOV: {s!r}") from e


def _iter_covered_branches_from_lcov(
	lcov_path: Path,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> Iterator[ElementKey]:
	_check_exists(lcov_path, what="LCOV coverage file")

	current_sf: str | None = None
	with lcov_path.open("r", encoding="utf-8", errors="replace") as f:
		for raw in f:
			line = raw.rstrip("\n")
			if not line:
				continue

			if line.startswith("SF:"):
				sf = line[3:]
				current_sf = sf

			if current_sf is None:
				continue
			if _is_excluded(current_sf, include=include, exclude=exclude):
				continue

			# Branch data: BRDA:<line>,<block>,<branch>,<taken>
			if line.startswith("BRDA:"):
				fields = line[5:].split(",")
				if len(fields) != 4:
					raise SystemExit(f"error: malformed BRDA line: {line!r}")
				lno = _parse_int(fields[0])
				block = _parse_int(fields[1])
				br = _parse_int(fields[2])
				taken_raw = fields[3]
				taken = 0 if taken_raw == "-" else _parse_int(taken_raw)
				if taken > 0:
					yield ("b", current_sf, lno, block, br)
				continue


def _iter_covered_lines_from_lcov(
	lcov_path: Path,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> Iterator[ElementKey]:
	_check_exists(lcov_path, what="LCOV coverage file")

	current_sf: str | None = None
	with lcov_path.open("r", encoding="utf-8", errors="replace") as f:
		for raw in f:
			line = raw.rstrip("\n")
			if not line:
				continue
			if line.startswith("SF:"):
				current_sf = line[3:]
				continue
			if current_sf is None:
				continue
			if _is_excluded(current_sf, include=include, exclude=exclude):
				continue
			# Line data: DA:<line>,<count>
			if line.startswith("DA:"):
				fields = line[3:].split(",")
				if len(fields) < 2:
					raise SystemExit(f"error: malformed DA line: {line!r}")
				lno = _parse_int(fields[0])
				count = _parse_int(fields[1])
				if count > 0:
					yield ("l", current_sf, lno)
				continue


def _iter_covered_functions_from_lcov(
	lcov_path: Path,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> Iterator[ElementKey]:
	"""Yields function elements derived from FNDA lines with count > 0.

	Keys are ("f", sf, function_name).
	"""
	_check_exists(lcov_path, what="LCOV coverage file")

	current_sf: str | None = None
	with lcov_path.open("r", encoding="utf-8", errors="replace") as f:
		for raw in f:
			line = raw.rstrip("\n")
			if not line:
				continue
			if line.startswith("SF:"):
				current_sf = line[3:]
				continue
			if current_sf is None:
				continue
			if _is_excluded(current_sf, include=include, exclude=exclude):
				continue
			# Function data: FNDA:<count>,<name>
			if line.startswith("FNDA:"):
				fields = line[5:].split(",", 1)
				if len(fields) != 2:
					raise SystemExit(f"error: malformed FNDA line: {line!r}")
				count = _parse_int(fields[0])
				name = fields[1]
				if count > 0:
					yield ("f", current_sf, name)
				continue


def _build_branch_universe_map(
	lcov_path: Path,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> dict[ElementKey, int]:
	universe: dict[ElementKey, int] = {}
	for elem in _iter_covered_branches_from_lcov(lcov_path, include=include, exclude=exclude):
		if elem not in universe:
			universe[elem] = len(universe)
	return universe


def _build_line_universe_map(
	lcov_path: Path,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> dict[ElementKey, int]:
	universe: dict[ElementKey, int] = {}
	for elem in _iter_covered_lines_from_lcov(lcov_path, include=include, exclude=exclude):
		if elem not in universe:
			universe[elem] = len(universe)
	return universe


def _build_function_universe_map(
	lcov_path: Path,
	*,
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],
) -> dict[ElementKey, int]:
	universe: dict[ElementKey, int] = {}
	for elem in _iter_covered_functions_from_lcov(lcov_path, include=include, exclude=exclude):
		if elem not in universe:
			universe[elem] = len(universe)
	return universe


def _collect_test_branch_ids(
	lcov_path: Path,
	*,
	universe: dict[ElementKey, int],
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],

) -> BitMap:
	ids: list[int] = []
	for elem in _iter_covered_branches_from_lcov(lcov_path, include=include, exclude=exclude):
		elem_id = universe.get(elem)
		if elem_id is not None:
			ids.append(elem_id)
	return BitMap(ids)


def _collect_test_line_ids(
	lcov_path: Path,
	*,
	universe: dict[ElementKey, int],
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],

) -> BitMap:
	ids: list[int] = []
	for elem in _iter_covered_lines_from_lcov(lcov_path, include=include, exclude=exclude):
		elem_id = universe.get(elem)
		if elem_id is not None:
			ids.append(elem_id)
	return BitMap(ids)


def _collect_test_function_ids(
	lcov_path: Path,
	*,
	universe: dict[ElementKey, int],
	include: Sequence[re.Pattern[str]],
	exclude: Sequence[re.Pattern[str]],

) -> BitMap:
	ids: list[int] = []
	for elem in _iter_covered_functions_from_lcov(lcov_path, include=include, exclude=exclude):
		elem_id = universe.get(elem)
		if elem_id is not None:
			ids.append(elem_id)
	return BitMap(ids)


def _choose_tests_greedy_multi(
	tests: Sequence[TestInfo],
	*,
	target_branches: int,
	target_lines: int,
	target_functions: int,
) -> list[TestInfo]:
	if target_branches <= 0 and target_lines <= 0 and target_functions <= 0:
		return []

	covered_branches = _empty_bitmap()
	covered_lines = _empty_bitmap()
	covered_functions = _empty_bitmap()
	chosen: list[TestInfo] = []
	remaining = list(tests)

	def done() -> bool:
		return (
			len(covered_branches) >= target_branches
			and len(covered_lines) >= target_lines
			and len(covered_functions) >= target_functions
		)

	while not done():
		best_index: int | None = None
		best_score = -1.0
		best_ratio = -1.0
		best_time = 0.0

		for i, test in enumerate(remaining):
			if test.duration_ms <= 0:
				continue
			b_gain = 0
			l_gain = 0
			f_gain = 0
			if len(covered_branches) < target_branches:
				b_gain = len(test.covered_branch_ids - covered_branches)
			if len(covered_lines) < target_lines:
				l_gain = len(test.covered_line_ids - covered_lines)
			if len(covered_functions) < target_functions:
				f_gain = len(test.covered_function_ids - covered_functions)
			if b_gain <= 0 and l_gain <= 0 and f_gain <= 0:
				continue

			# Normalize gains so each target contributes comparably.
			score = 0.0
			if target_branches > 0:
				score += b_gain / float(target_branches)
			if target_lines > 0:
				score += l_gain / float(target_lines)
			if target_functions > 0:
				score += f_gain / float(target_functions)
			ratio = score / test.duration_ms

			if ratio > best_ratio:
				best_index, best_score, best_ratio, best_time = i, score, ratio, test.duration_ms
				continue
			if ratio == best_ratio and (score > best_score or (score == best_score and test.duration_ms < best_time)):
				best_index, best_score, best_ratio, best_time = i, score, ratio, test.duration_ms

		if best_index is None:
			raise SystemExit(
				"error: cannot reach target coverage; "
				f"covered branches {len(covered_branches)}/{target_branches}, "
				f"lines {len(covered_lines)}/{target_lines}, "
				f"functions {len(covered_functions)}/{target_functions}"
			)

		selected = remaining.pop(best_index)
		chosen.append(selected)
		covered_branches |= selected.covered_branch_ids
		covered_lines |= selected.covered_line_ids
		covered_functions |= selected.covered_function_ids

	return chosen


def _sum_duration_ms(tests: Sequence[TestInfo]) -> float:
	return sum(t.duration_ms for t in tests)


def _covered_sizes(tests: Sequence[TestInfo]) -> tuple[int, int, int]:
	b = _empty_bitmap()
	l = _empty_bitmap()
	f = _empty_bitmap()
	for t in tests:
		b |= t.covered_branch_ids
		l |= t.covered_line_ids
		f |= t.covered_function_ids
	return len(b), len(l), len(f)


def _choose_tests_greedy_repair_multi(
	remaining: Sequence[TestInfo],
	*,
	covered_branches: BitMap,
	covered_lines: BitMap,
	covered_functions: BitMap,
	target_branches: int,
	target_lines: int,
	target_functions: int,
) -> list[TestInfo]:
	"""Greedy selection starting from an existing covered-set state."""
	if target_branches <= 0 and target_lines <= 0 and target_functions <= 0:
		return []

	covered_b = covered_branches
	covered_l = covered_lines
	covered_f = covered_functions
	chosen: list[TestInfo] = []
	remaining_list = list(remaining)

	def done() -> bool:
		return (
			len(covered_b) >= target_branches
			and len(covered_l) >= target_lines
			and len(covered_f) >= target_functions
		)

	while not done():
		best_index: int | None = None
		best_score = -1.0
		best_ratio = -1.0
		best_time = 0.0

		for i, test in enumerate(remaining_list):
			if test.duration_ms <= 0:
				continue
			b_gain = 0
			l_gain = 0
			f_gain = 0
			if len(covered_b) < target_branches:
				b_gain = len(test.covered_branch_ids - covered_b)
			if len(covered_l) < target_lines:
				l_gain = len(test.covered_line_ids - covered_l)
			if len(covered_f) < target_functions:
				f_gain = len(test.covered_function_ids - covered_f)
			if b_gain <= 0 and l_gain <= 0 and f_gain <= 0:
				continue

			score = 0.0
			if target_branches > 0:
				score += b_gain / float(target_branches)
			if target_lines > 0:
				score += l_gain / float(target_lines)
			if target_functions > 0:
				score += f_gain / float(target_functions)
			ratio = score / test.duration_ms

			if ratio > best_ratio:
				best_index, best_score, best_ratio, best_time = i, score, ratio, test.duration_ms
				continue
			if ratio == best_ratio and (
				score > best_score or (score == best_score and test.duration_ms < best_time)
			):
				best_index, best_score, best_ratio, best_time = i, score, ratio, test.duration_ms

		if best_index is None:
			raise SystemExit(
				"error: cannot reach target coverage (repair); "
				f"covered branches {len(covered_b)}/{target_branches}, "
				f"lines {len(covered_l)}/{target_lines}, "
				f"functions {len(covered_f)}/{target_functions}"
			)

		selected = remaining_list.pop(best_index)
		chosen.append(selected)
		covered_b |= selected.covered_branch_ids
		covered_l |= selected.covered_line_ids
		covered_f |= selected.covered_function_ids

	return chosen


def _choose_tests_grasp_multi(
	tests: Sequence[TestInfo],
	*,
	target_branches: int,
	target_lines: int,
	target_functions: int,
	turns: int,
	rcl_size: int,
	local_search_iters: int,
	seed: int | None,
	jobs: int,
	quiet: bool,
) -> list[TestInfo]:
	"""GRASP + simple local search.

	- Construction: greedy ratio selection, but choose randomly among the top-k candidates.
	- Local search: repeatedly try drop+repair+prune, keep improvements.
	"""
	if target_branches <= 0 and target_lines <= 0 and target_functions <= 0:
		return []
	if turns <= 0:
		raise SystemExit("error: --grasp-turns must be > 0")
	if rcl_size <= 0:
		raise SystemExit("error: --grasp-rcl must be > 0")
	if local_search_iters < 0:
		raise SystemExit("error: --grasp-local-iters must be >= 0")

	tests_list = list(tests)

	def grasp_one(turn_index: int) -> tuple[float, list[TestInfo]]:
		turn_seed = (seed if seed is not None else random.randrange(1 << 30)) + turn_index
		rng = random.Random(turn_seed)

		covered_b = _empty_bitmap()
		covered_l = _empty_bitmap()
		covered_f = _empty_bitmap()
		chosen: list[TestInfo] = []
		remaining = list(tests_list)

		def done() -> bool:
			return (
				len(covered_b) >= target_branches
				and len(covered_l) >= target_lines
				and len(covered_f) >= target_functions
			)

		while not done():
			cands: list[tuple[float, float, float, int]] = []
			for i, test in enumerate(remaining):
				if test.duration_ms <= 0:
					continue
				b_gain = 0
				l_gain = 0
				f_gain = 0
				if len(covered_b) < target_branches:
					b_gain = len(test.covered_branch_ids - covered_b)
				if len(covered_l) < target_lines:
					l_gain = len(test.covered_line_ids - covered_l)
				if len(covered_f) < target_functions:
					f_gain = len(test.covered_function_ids - covered_f)
				if b_gain <= 0 and l_gain <= 0 and f_gain <= 0:
					continue

				score = 0.0
				if target_branches > 0:
					score += b_gain / float(target_branches)
				if target_lines > 0:
					score += l_gain / float(target_lines)
				if target_functions > 0:
					score += f_gain / float(target_functions)
				ratio = score / test.duration_ms
				cands.append((ratio, score, test.duration_ms, i))

			if not cands:
				raise SystemExit(
					"error: cannot reach target coverage (grasp); "
					f"covered branches {len(covered_b)}/{target_branches}, "
					f"lines {len(covered_l)}/{target_lines}, "
					f"functions {len(covered_f)}/{target_functions}"
				)

			# Sort best-first and pick randomly among the top-k.
			cands.sort(reverse=True)
			k = min(rcl_size, len(cands))
			_, _, _, best_i = rng.choice(cands[:k])
			selected = remaining.pop(best_i)
			chosen.append(selected)
			covered_b |= selected.covered_branch_ids
			covered_l |= selected.covered_line_ids
			covered_f |= selected.covered_function_ids

		# Local search: try removing random tests and repairing.
		current = _prune_redundant_tests_multi(
			chosen,
			target_branches=target_branches,
			target_lines=target_lines,
			target_functions=target_functions,
		)
		best_time = _sum_duration_ms(current)

		if local_search_iters > 0 and len(current) > 1:
			for _ in range(local_search_iters):
				# Randomly remove one test, then repair greedily.
				idx = rng.randrange(len(current))
				trial = current[:idx] + current[idx + 1 :]
				cov_b = _empty_bitmap()
				cov_l = _empty_bitmap()
				cov_f = _empty_bitmap()
				chosen_names = set()
				for t in trial:
					chosen_names.add(t.name)
					cov_b |= t.covered_branch_ids
					cov_l |= t.covered_line_ids
					cov_f |= t.covered_function_ids

				if (
					len(cov_b) < target_branches
					or len(cov_l) < target_lines
					or len(cov_f) < target_functions
				):
					remaining2 = [t for t in tests_list if t.name not in chosen_names]
					added = _choose_tests_greedy_repair_multi(
						remaining2,
						covered_branches=cov_b,
						covered_lines=cov_l,
						covered_functions=cov_f,
						target_branches=target_branches,
						target_lines=target_lines,
						target_functions=target_functions,
					)
					trial = trial + added

				trial = _prune_redundant_tests_multi(
					trial,
					target_branches=target_branches,
					target_lines=target_lines,
					target_functions=target_functions,
				)
				trial_time = _sum_duration_ms(trial)
				if trial_time < best_time:
					current = trial
					best_time = trial_time

		return best_time, current

	jobs_eff = max(1, jobs)
	results = Parallel(n_jobs=jobs_eff, prefer="threads")(
		delayed(grasp_one)(i) for i in range(turns)
	)
	results.sort(key=lambda x: x[0])
	best_time, best_solution = results[0]
	if not quiet:
		b, l, f = _covered_sizes(best_solution)
		_eprint(
			f"note: GRASP best-of-{turns}: time {best_time:.1f} ms, "
			f"covered branches {b}/{target_branches}, lines {l}/{target_lines}, functions {f}/{target_functions}"
		)
	return best_solution


def _prune_redundant_tests_multi(
	chosen: Sequence[TestInfo],
	*,
	target_branches: int,
	target_lines: int,
	target_functions: int,
) -> list[TestInfo]:
	if target_branches <= 0 and target_lines <= 0 and target_functions <= 0:
		return []

	from collections import Counter

	b_counts: Counter[int] = Counter()
	l_counts: Counter[int] = Counter()
	f_counts: Counter[int] = Counter()
	for t in chosen:
		b_counts.update(t.covered_branch_ids)
		l_counts.update(t.covered_line_ids)
		f_counts.update(t.covered_function_ids)

	covered_b = len(b_counts)
	covered_l = len(l_counts)
	covered_f = len(f_counts)
	kept = list(chosen)

	for t in sorted(kept, key=lambda x: x.duration_ms, reverse=True):
		lost_b = 0
		lost_l = 0
		lost_f = 0
		for b in t.covered_branch_ids:
			if b_counts[b] == 1:
				lost_b += 1
		for l in t.covered_line_ids:
			if l_counts[l] == 1:
				lost_l += 1
		for f in t.covered_function_ids:
			if f_counts[f] == 1:
				lost_f += 1

		if covered_b - lost_b < target_branches:
			continue
		if covered_l - lost_l < target_lines:
			continue
		if covered_f - lost_f < target_functions:
			continue

		for b in t.covered_branch_ids:
			b_counts[b] -= 1
			if b_counts[b] == 0:
				del b_counts[b]
		for l in t.covered_line_ids:
			l_counts[l] -= 1
			if l_counts[l] == 0:
				del l_counts[l]
		for f in t.covered_function_ids:
			f_counts[f] -= 1
			if f_counts[f] == 0:
				del f_counts[f]
		covered_b = len(b_counts)
		covered_l = len(l_counts)
		covered_f = len(f_counts)
		kept.remove(t)

	return kept


def _write_atomic_text(path: Path, content: str) -> None:
	tmp = path.with_suffix(path.suffix + ".tmp")
	try:
		tmp.write_text(content, encoding="utf-8")
		tmp.replace(path)
	except OSError as e:
		raise SystemExit(f"error: failed writing {path}: {e}") from e


def _write_atomic_bytes(path: Path, content: bytes) -> None:
	tmp = path.with_suffix(path.suffix + ".tmp")
	try:
		tmp.write_bytes(content)
		tmp.replace(path)
	except OSError as e:
		raise SystemExit(f"error: failed writing {path}: {e}") from e


def _decode_cpp_plain_string_literal(raw: bytes) -> str:
	"""Decode a simple C++ string literal token ("...") with no escapes.

	This intentionally fails closed: no prefixes (u8, L, raw-string literals), no escapes.
	"""
	if len(raw) < 2 or raw[:1] != b'"' or raw[-1:] != b'"':
		raise ValueError("unsupported string literal form")
	inner = raw[1:-1]
	if b"\\" in inner:
		raise ValueError("escape sequence in string literal")
	return inner.decode("utf-8", errors="strict")


def _update_tags_in_sources(
	*,
	root: Path,
	src_dir: Path | None,
	all_tests: set[str],
	candidates: set[str],
	slow_tag: str,
	quiet: bool,
) -> None:
	slow_tag_name = _normalize_tag_name(slow_tag)
	slow_tag_token = f"[{slow_tag_name}]"
	if not candidates.issubset(all_tests):
		extra = sorted(candidates - all_tests)
		raise SystemExit(
			"error: candidates contain tests not present in --test-list universe (first 20): "
			+ ", ".join(extra[:20])
		)

	src_root = src_dir if src_dir is not None else (root / "src")
	if not src_root.is_dir():
		raise SystemExit(f"error: src directory not found: {src_root}")

	supported_macros = {"TEST_CASE", "TEST_CASE_VERSIONS"}
	unsupported_seen: dict[str, int] = {}

	# Tree-sitter C++ parser (prebuilt grammar).
	lang = Language(tree_sitter_cpp.language())
	parser = Parser()
	parser.language = lang

	found: dict[str, Path] = {}
	edits_by_file: dict[Path, list[tuple[int, int, bytes]]] = {}

	def note_unsupported(macro: str) -> None:
		unsupported_seen[macro] = unsupported_seen.get(macro, 0) + 1

	def parse_test_name(raw: bytes, node) -> str | None:
		# Accept either a single string literal or a concatenated string.
		try:
			if node.type == "string_literal":
				return _decode_cpp_plain_string_literal(raw[node.start_byte : node.end_byte])
			if node.type == "concatenated_string":
				parts: list[str] = []
				for c in node.named_children:
					if c.type != "string_literal":
						return None
					parts.append(_decode_cpp_plain_string_literal(raw[c.start_byte : c.end_byte]))
				return "".join(parts)
		except ValueError:
			return None
		return None

	def parse_tags_span(raw: bytes, node) -> tuple[str, int, int] | None:
		# Require a single plain string literal so we can safely do an in-place edit.
		if node.type != "string_literal":
			return None
		lit = raw[node.start_byte : node.end_byte]
		try:
			value = _decode_cpp_plain_string_literal(lit)
		except ValueError:
			return None
		# Span inside quotes.
		return value, node.start_byte + 1, node.end_byte - 1

	# Scan source files.
	for path in sorted(src_root.glob("**/*.cpp")):
		try:
			raw = path.read_bytes()
		except OSError as e:
			raise SystemExit(f"error: failed reading {path}: {e}") from e

		tree = parser.parse(raw)
		stack = [tree.root_node]
		while stack:
			node = stack.pop()
			stack.extend(node.children)

			if node.type != "call_expression":
				continue
			# For macro-like calls we expect: identifier + argument_list
			named = node.named_children
			if len(named) < 2:
				continue
			func = named[0]
			args = named[1]
			if func.type != "identifier" or args.type != "argument_list":
				continue
			ident = raw[func.start_byte : func.end_byte].decode("utf-8", errors="ignore")
			if ident not in supported_macros:
				if ident.startswith("TEST_CASE") or ident.endswith("TEST_CASE"):
					note_unsupported(ident)
				continue
			arg_nodes = args.named_children
			if len(arg_nodes) < 2:
				continue
			test_name = parse_test_name(raw, arg_nodes[0])
			if not test_name:
				continue
			if test_name not in all_tests:
				continue
			if test_name in found:
				raise SystemExit(
					f"error: conflicting test name found multiple times: {test_name!r} "
					f"in {found[test_name]} and {path}"
				)
			tag_info = parse_tags_span(raw, arg_nodes[1])
			if tag_info is None:
				raise SystemExit(
					f"error: cannot parse tags string literal for {test_name!r} in {path}: "
					"tags arg is not a simple string literal"
				)
			tags_value, span_start, span_end = tag_info
			want_tag = test_name not in candidates
			has_tag = slow_tag_token in tags_value
			new_tags_value = tags_value
			if want_tag and not has_tag:
				new_tags_value = tags_value + slow_tag_token
			elif (not want_tag) and has_tag:
				new_tags_value = tags_value.replace(slow_tag_token, "")

			found[test_name] = path
			if new_tags_value != tags_value:
				edits_by_file.setdefault(path, []).append(
					(span_start, span_end, new_tags_value.encode("utf-8"))
				)

	# Warn on unsupported macros we saw (requested: don't support, but warn).
	if unsupported_seen and not quiet:
		for macro, count in sorted(unsupported_seen.items()):
			_eprint(f"warning: unsupported macro seen ({count}): {macro}")

	missing = sorted(all_tests - set(found.keys()))
	if missing:
		raise SystemExit(
			"error: tests from --test-list not found in src/**/*.cpp TEST_CASE macros (first 20): "
			+ ", ".join(missing[:20])
		)

	# Apply edits in-place, per-file, from end to start.
	changed_files = 0
	changed_tests = 0
	for path, edits in sorted(edits_by_file.items(), key=lambda kv: str(kv[0])):
		try:
			raw = path.read_bytes()
		except OSError as e:
			raise SystemExit(f"error: failed reading {path}: {e}") from e
		out = raw
		for start, end, repl in sorted(edits, key=lambda x: x[0], reverse=True):
			out = out[:start] + repl + out[end:]
			changed_tests += 1
		if out != raw:
			_write_atomic_bytes(path, out)
			changed_files += 1

	if not quiet:
		_eprint(
			f"note: updated {slow_tag_token} tags in {changed_tests} tests across {changed_files} files"
		)


def main(argv: Sequence[str] | None = None) -> int:
	parser = argparse.ArgumentParser(
		description=(
			"Analyze per-test LLVM coverage (.profraw) and select a high-ROI subset "
			"of tests that reaches a target fraction of full-suite coverage while "
			"minimizing total runtime."
		)
	)
	parser.add_argument("--test-list", required=True, type=Path, help="CSV: test name, .profraw path, duration")
	parser.add_argument(
		"--branch-threshold",
		default=80.0,
		type=float,
		help="Target branch coverage percentage (0-100). Set to 0 to disable. (default: 80)",
	)
	parser.add_argument(
		"--line-threshold",
		default=0.0,
		type=float,
		help="Target line coverage percentage (0-100) based on DA records. Set to 0 to disable. (default: 0)",
	)
	parser.add_argument(
		"--function-threshold",
		default=90.0,
		type=float,
		help="Target function coverage percentage (0-100) based on FNDA records. Set to 0 to disable. (default: 90)",
	)
	parser.add_argument(
		"--candidates-csv",
		type=Path,
		default=Path("candidates.csv"),
		help="Write selected test names to this CSV (default: candidates.csv)",
	)
	parser.add_argument(
		"--exclude-regex",
		action="append",
		default=[],
		help="Regex to exclude source files from coverage (repeatable)",
	)
	parser.add_argument(
		"--include-regex",
		action="append",
		default=[],
		help=(
			"Regex to force-include source files even if excluded (repeatable)."
		),
	)
	parser.add_argument(
		"--debug",
		type=Path,
		default=None,
		metavar="DIR",
		help="If set, writes filtered LCOV files into DIR (full + per-test)",
	)
	parser.add_argument(
		"--binary",
		type=Path,
		default=Path("src/stellar-core"),
		help="Binary to pass to llvm-cov (default: src/stellar-core)",
	)
	parser.add_argument(
		"--llvm-cov",
		default="llvm-cov-20",
		help="llvm-cov executable name/path (default: llvm-cov-20)",
	)
	parser.add_argument(
		"--llvm-profdata",
		default="llvm-profdata-20",
		help="llvm-profdata executable name/path (default: llvm-profdata-20)",
	)
	parser.add_argument(
		"--solver",
		choices=["greedy", "grasp"],
		default="greedy",
		help="Candidate selection strategy (default: greedy)",
	)
	parser.add_argument(
		"--grasp-turns",
		type=int,
		default=50,
		help="Number of GRASP construction+local-search turns (default: 50)",
	)
	parser.add_argument(
		"--grasp-rcl",
		type=int,
		default=10,
		help="GRASP restricted candidate list size (default: 10)",
	)
	parser.add_argument(
		"--grasp-local-iters",
		type=int,
		default=50,
		help="GRASP local-search iterations per turn (default: 50)",
	)
	parser.add_argument(
		"--seed",
		type=int,
		default=None,
		help="Random seed for stochastic solvers (default: unset)",
	)
	parser.add_argument(
		"--jobs",
		type=int,
		default=None,
		help="Parallel workers for per-test processing (default: detected CPU count)",
	)

	parser.add_argument(
		"--update-tests-tags",
		choices=["no", "update-only", "scan-and-update"],
		default="no",
		help=(
			"Update Catch2 slow-tag in src/**/*.cpp based on selected candidates. "
			"Modes: no (default), update-only (read --candidates-csv and edit sources), scan-and-update (compute then edit sources)."
		),
	)
	parser.add_argument(
		"--slow-tag",
		default="acceptance",
		help=(
			"Catch2 tag (without brackets) to apply to tests that are not in the candidate list (default: acceptance). "
			"The script adds/removes the bracketed form, e.g. [acceptance]."
		),
	)
	parser.add_argument(
		"--src-dir",
		type=Path,
		default=None,
		help="Path to the src directory to scan for tests when updating tags (default: <repo>/src)",
	)
	parser.add_argument("--quiet", action="store_true", help="Reduce logging")
	args = parser.parse_args(list(argv) if argv is not None else None)

	for opt, val in (
		("--branch-threshold", args.branch_threshold),
		("--line-threshold", args.line_threshold),
		("--function-threshold", args.function_threshold),
	):
		if val < 0 or val > 100:
			raise SystemExit(f"error: {opt} must be in [0, 100]")
	if args.update_tests_tags != "update-only":
		if args.branch_threshold == 0 and args.line_threshold == 0 and args.function_threshold == 0:
			raise SystemExit(
				"error: all thresholds are 0; enable at least one of --branch-threshold, --line-threshold, --function-threshold"
			)

	test_rows = _read_test_list_csv(args.test_list)
	all_tests = {name for (name, _, _) in test_rows}

	include_compiled = _compile_patterns(args.include_regex)
	exclude_compiled = _compile_patterns(args.exclude_regex)

	debug_dir: Path | None = args.debug
	if debug_dir is not None:
		debug_dir.mkdir(parents=True, exist_ok=True)
		if not debug_dir.is_dir():
			raise SystemExit(f"error: --debug must be a directory: {debug_dir}")

	if args.update_tests_tags == "update-only":
		candidates = _read_candidates_csv(args.candidates_csv)
		_update_tags_in_sources(
			root=Path(__file__).resolve().parents[1],
			src_dir=args.src_dir,
			all_tests=all_tests,
			candidates=candidates,
			slow_tag=args.slow_tag,
			quiet=args.quiet,
		)
		return 0

	# Verify binary early (needed when computing candidates).
	_check_exists(args.binary, what="coverage binary (use --binary to override)")

	profraw_paths = [profraw for (_, profraw, _) in test_rows]
	for p in profraw_paths:
		_check_exists(p, what=".profraw file")

	with tempfile.TemporaryDirectory(prefix="stellar-core-testcov-") as td:
		tmp_dir = Path(td)

		# 1) Full suite universes
		full_profdata = tmp_dir / "full.profdata"
		_merge_profraws_to_profdata(
			llvm_profdata=args.llvm_profdata,
			profraw_paths=profraw_paths,
			output_profdata=full_profdata,
			tmp_dir=tmp_dir,
		)

		full_raw_lcov_path = tmp_dir / "full.raw.lcov"
		full_lcov_path = tmp_dir / "full.filtered.lcov"
		_export_coverage_lcov(
			llvm_cov=args.llvm_cov,
			binary=args.binary,
			profdata=full_profdata,
			output_lcov=full_raw_lcov_path,
			quiet=args.quiet,
		)
		_filter_lcov(
			input_lcov=full_raw_lcov_path,
			output_lcov=full_lcov_path,
			include=include_compiled,
			exclude=exclude_compiled,
		)
		if debug_dir is not None:
			(debug_dir / "full.filtered.lcov").write_bytes(full_lcov_path.read_bytes())

		branch_universe = _build_branch_universe_map(
			full_lcov_path, include=include_compiled, exclude=exclude_compiled
		)
		line_universe: dict[ElementKey, int] = {}
		func_universe: dict[ElementKey, int] = {}
		branch_size = len(branch_universe)
		line_size = 0
		func_size = 0

		branch_target = 0 if args.branch_threshold == 0 else int(math.ceil(args.branch_threshold * branch_size / 100.0))
		line_target = 0
		func_target = 0

		if args.line_threshold != 0:
			line_universe = _build_line_universe_map(
				full_lcov_path, include=include_compiled, exclude=exclude_compiled
			)
			line_size = len(line_universe)
			line_target = int(math.ceil(args.line_threshold * line_size / 100.0))

		if args.function_threshold != 0:
			func_universe = _build_function_universe_map(
				full_lcov_path, include=include_compiled, exclude=exclude_compiled
			)
			func_size = len(func_universe)
			func_target = int(math.ceil(args.function_threshold * func_size / 100.0))
		if not args.quiet:
			_eprint(f"note: branch universe covered by full suite: {branch_size}")
			_eprint(f"note: target branches @ {args.branch_threshold}%: {branch_target}")
			_eprint(f"note: line universe covered by full suite: {line_size}")
			_eprint(f"note: target lines @ {args.line_threshold}%: {line_target}")
			_eprint(f"note: function universe covered by full suite: {func_size}")
			_eprint(f"note: target functions @ {args.function_threshold}%: {func_target}")
			if args.line_threshold == 0 and branch_size < 100:
				_eprint(
					"note: branch universe is very small; consider setting --line-threshold > 0 "
					"for a more informative coverage target"
				)
			if args.function_threshold != 0 and func_size == 0:
				_eprint(
					"note: function-threshold is enabled but full suite produced no FNDA-covered functions; "
					"function target will be 0"
				)

		# 2) Per-test coverage sets (parallel)
		n_jobs = args.jobs if (args.jobs is not None and args.jobs > 0) else (os.cpu_count() or 1)

		if not args.quiet:
			_eprint(f"note: processing {len(test_rows)} tests in parallel with {n_jobs} workers...")

		def process_one(i: int, name: str, profraw: Path, duration_us: int) -> TestInfo:
			test_profdata = tmp_dir / f"test-{i}.profdata"
			_merge_profraws_to_profdata(
				llvm_profdata=args.llvm_profdata,
				profraw_paths=[profraw],
				output_profdata=test_profdata,
				tmp_dir=tmp_dir,
			)
			test_raw_lcov_path = tmp_dir / f"test-{i}.raw.lcov"
			test_lcov_path = tmp_dir / f"test-{i}.filtered.lcov"
			_export_coverage_lcov(
				llvm_cov=args.llvm_cov,
				binary=args.binary,
				profdata=test_profdata,
				output_lcov=test_raw_lcov_path,
				quiet=True,
			)
			_filter_lcov(
				input_lcov=test_raw_lcov_path,
				output_lcov=test_lcov_path,
				include=include_compiled,
				exclude=exclude_compiled,
			)
			if debug_dir is not None:
				frag = _safe_filename_fragment(name)
				out = debug_dir / f"test-{i:05d}-{frag}.filtered.lcov"
				out.write_bytes(test_lcov_path.read_bytes())

			branch_ids = _empty_bitmap()
			line_ids = _empty_bitmap()
			func_ids = _empty_bitmap()
			if branch_target > 0:
				branch_ids = _collect_test_branch_ids(
					test_lcov_path,
					universe=branch_universe,
					include=include_compiled,
					exclude=exclude_compiled,
				)
			if line_target > 0:
				line_ids = _collect_test_line_ids(
					test_lcov_path,
					universe=line_universe,
					include=include_compiled,
					exclude=exclude_compiled,
				)
			if func_target > 0:
				func_ids = _collect_test_function_ids(
					test_lcov_path,
					universe=func_universe,
					include=include_compiled,
					exclude=exclude_compiled,
				)

			return TestInfo(
				name=name,
				profraw_path=profraw,
				duration_ms=_duration_us_to_ms(duration_us),
				covered_branch_ids=branch_ids,
				covered_line_ids=line_ids,
				covered_function_ids=func_ids,
			)

		tests = Parallel(n_jobs=n_jobs, prefer="threads")(
			delayed(process_one)(i, name, profraw, duration)
			for i, (name, profraw, duration) in enumerate(test_rows, start=1)
		)
		total_ms_all = sum(t.duration_ms for t in tests)
		
		if not args.quiet:
			_eprint(f"note: solving...")

		# 3) Select subset
		if args.solver == "greedy":
			chosen = _choose_tests_greedy_multi(
				tests,
				target_branches=branch_target,
				target_lines=line_target,
				target_functions=func_target,
			)
			pruned = _prune_redundant_tests_multi(
				chosen,
				target_branches=branch_target,
				target_lines=line_target,
				target_functions=func_target,
			)
		elif args.solver == "grasp":
			pruned = _choose_tests_grasp_multi(
				tests,
				target_branches=branch_target,
				target_lines=line_target,
				target_functions=func_target,
				turns=args.grasp_turns,
				rcl_size=args.grasp_rcl,
				local_search_iters=args.grasp_local_iters,
				seed=args.seed,
				jobs=n_jobs,
				quiet=args.quiet,
			)
		else:
			raise SystemExit(f"error: unknown solver: {args.solver}")

		achieved_b = _empty_bitmap()
		achieved_l = _empty_bitmap()
		achieved_f = _empty_bitmap()
		total_ms = 0.0
		for t in pruned:
			achieved_b |= t.covered_branch_ids
			achieved_l |= t.covered_line_ids
			achieved_f |= t.covered_function_ids
			total_ms += t.duration_ms
		ach_b = len(achieved_b)
		ach_l = len(achieved_l)
		ach_f = len(achieved_f)
		pct_b = 0.0 if branch_size == 0 else (100.0 * ach_b / branch_size)
		pct_l = 0.0 if line_size == 0 else (100.0 * ach_l / line_size)
		pct_f = 0.0 if func_size == 0 else (100.0 * ach_f / func_size)
		pct_t = 0.0 if total_ms_all == 0 else (100.0 * total_ms / total_ms_all)

		_eprint(
			f"selected {len(pruned)} tests; achieved "
			f"branches {ach_b}/{branch_target} ({pct_b:.2f}%), "
			f"lines {ach_l}/{line_target} ({pct_l:.2f}%), "
			f"functions {ach_f}/{func_target} ({pct_f:.2f}%); "
			f"total time {total_ms:.1f} ms ({pct_t:.2f}% of {total_ms_all:.1f} ms)"
		)
		# Write selected tests to stdout (one per line).
		for t in pruned:
			print(t.name)

		# Also write selected tests to a CSV file.
		try:
			args.candidates_csv.parent.mkdir(parents=True, exist_ok=True)
			df = pl.DataFrame({"name": [t.name for t in pruned]})
			df.write_csv(args.candidates_csv, include_header=False)
		except OSError as e:
			raise SystemExit(f"error: failed writing candidates CSV {args.candidates_csv}: {e}") from e

		if args.update_tests_tags == "scan-and-update":
			candidates = {t.name for t in pruned}
			_update_tags_in_sources(
				root=Path(__file__).resolve().parents[1],
				src_dir=args.src_dir,
				all_tests=all_tests,
				candidates=candidates,
				slow_tag=args.slow_tag,
				quiet=args.quiet,
			)

	return 0


def _run_self_tests() -> int:
	"""Runs a small unittest suite for main behaviors.

	This intentionally excludes testing llvm-profdata/llvm-cov invocation.
	"""
	import unittest
	from tempfile import TemporaryDirectory

	class _SelfTests(unittest.TestCase):
		def bm(self, values: Sequence[int]) -> BitMap:
			return BitMap(list(values))

		def test_read_test_list_csv_valid(self):
			with TemporaryDirectory() as td:
				p = Path(td) / "profraw.csv"
				(Path(td) / "t1.profraw").write_bytes(b"x")
				(Path(td) / "t2.profraw").write_bytes(b"y")
				p.write_text(
					"\n".join(
						[
							"t1,{} ,1000".format(Path(td) / "t1.profraw"),
							"t2,{},2000".format(Path(td) / "t2.profraw"),
						]
					)
					+ "\n",
					encoding="utf-8",
				)
				rows = _read_test_list_csv(p)
				self.assertEqual(len(rows), 2)
				self.assertEqual(rows[0][0], "t1")
				self.assertIsInstance(rows[0][1], Path)
				self.assertEqual(rows[0][2], 1000)

		def test_read_test_list_csv_invalid_columns(self):
			with TemporaryDirectory() as td:
				p = Path(td) / "bad.csv"
				p.write_text("a,b\n", encoding="utf-8")
				with self.assertRaises(SystemExit):
					_read_test_list_csv(p)

		def test_filter_lcov_include_overrides_exclude(self):
			with TemporaryDirectory() as td:
				inp = Path(td) / "in.lcov"
				out = Path(td) / "out.lcov"
				inp.write_text(
					"TN:\n"
					"SF:/src/lib/foo.cpp\n"
					"DA:1,1\n"
					"end_of_record\n"
					"SF:/src/app/bar.cpp\n"
					"DA:1,1\n"
					"end_of_record\n",
					encoding="utf-8",
				)
				include = _compile_patterns([r"foo\.cpp$"])
				exclude = _compile_patterns([r"/lib/"])
				_filter_lcov(input_lcov=inp, output_lcov=out, include=include, exclude=exclude)
				data = out.read_text(encoding="utf-8")
				self.assertIn("SF:/src/lib/foo.cpp", data)
				self.assertIn("SF:/src/app/bar.cpp", data)

		def test_iter_elements_branches_and_lines(self):
			with TemporaryDirectory() as td:
				p = Path(td) / "t.lcov"
				p.write_text(
					"TN:\n"
					"SF:/src/app/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,-\n"
					"BRDA:12,0,0,0\n"
					"DA:20,3\n"
					"DA:21,0\n"
					"end_of_record\n",
					encoding="utf-8",
				)
				branches = list(_iter_covered_branches_from_lcov(p, include=[], exclude=[]))
				lines = list(_iter_covered_lines_from_lcov(p, include=[], exclude=[]))
				self.assertIn(("b", "/src/app/a.cpp", 10, 0, 0), branches)
				self.assertNotIn(("b", "/src/app/a.cpp", 11, 0, 0), branches)
				self.assertIn(("l", "/src/app/a.cpp", 20), lines)
				self.assertNotIn(("l", "/src/app/a.cpp", 21), lines)

		def test_greedy_multi_and_prune_basic(self):
			t1 = TestInfo("t1", Path("/x"), 10.0, self.bm([1, 2, 3]), _empty_bitmap(), _empty_bitmap())
			t2 = TestInfo("t2", Path("/x"), 1.0, self.bm([1]), _empty_bitmap(), _empty_bitmap())
			t3 = TestInfo("t3", Path("/x"), 2.0, self.bm([4]), _empty_bitmap(), _empty_bitmap())
			chosen = _choose_tests_greedy_multi(
				[t1, t2, t3],
				target_branches=4,
				target_lines=0,
				target_functions=0,
			)
			self.assertEqual([t.name for t in chosen], ["t2", "t3", "t1"])
			pruned = _prune_redundant_tests_multi(
				chosen,
				target_branches=4,
				target_lines=0,
				target_functions=0,
			)
			self.assertEqual(set(t.name for t in pruned), {"t1", "t3"})

		def test_grasp_multi_feasible_seeded(self):
			t1 = TestInfo("t1", Path("/x"), 10.0, self.bm([1, 2]), self.bm([10]), _empty_bitmap())
			t2 = TestInfo(
				"t2", Path("/x"), 30.0, self.bm([3]), self.bm([11, 12]), self.bm([100])
			)
			t3 = TestInfo("t3", Path("/x"), 5.0, self.bm([2, 3]), self.bm([12]), self.bm([100]))

			chosen = _choose_tests_grasp_multi(
				[t1, t2, t3],
				target_branches=3,
				target_lines=3,
				target_functions=1,
				turns=25,
				rcl_size=3,
				local_search_iters=20,
				seed=123,
				jobs=1,
				quiet=True,
			)
			self.assertTrue(chosen)
			branches, lines, functions = _covered_sizes(chosen)
			self.assertGreaterEqual(branches, 3)
			self.assertGreaterEqual(lines, 3)
			self.assertGreaterEqual(functions, 1)

		def test_candidate_selection_branch_vs_branch_and_line(self):
			with TemporaryDirectory() as td:
				base = Path(td)
				full = base / "full.lcov"
				fast = base / "fast.lcov"
				slow = base / "slow.lcov"

				# Full suite covers 2 branches + 2 lines.
				full.write_text(
					"TN:\n"
					"SF:/src/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,1\n"
					"DA:20,1\n"
					"DA:21,1\n"
					"end_of_record\n",
					encoding="utf-8",
				)

				# Fast test covers both branches but no covered lines.
				fast.write_text(
					"TN:\n"
					"SF:/src/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,1\n"
					"DA:20,0\n"
					"DA:21,0\n"
					"end_of_record\n",
					encoding="utf-8",
				)

				# Slow test covers both lines, but only one of the branches.
				slow.write_text(
					"TN:\n"
					"SF:/src/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,0\n"
					"DA:20,1\n"
					"DA:21,1\n"
					"end_of_record\n",
					encoding="utf-8",
				)

				# Branch-only candidate set should pick only the fast test.
				universe_b = _build_branch_universe_map(full, include=[], exclude=[])
				fast_ids_b = _collect_test_branch_ids(fast, universe=universe_b, include=[], exclude=[])
				slow_ids_b = _collect_test_branch_ids(slow, universe=universe_b, include=[], exclude=[])
				tests_b = [
					TestInfo("fast", Path("/fast"), 1.0, fast_ids_b, _empty_bitmap(), _empty_bitmap()),
					TestInfo("slow", Path("/slow"), 10.0, slow_ids_b, _empty_bitmap(), _empty_bitmap()),
				]
				target_b = len(universe_b)
				pruned_b = _prune_redundant_tests_multi(
					_choose_tests_greedy_multi(
						tests_b, target_branches=target_b, target_lines=0, target_functions=0
					),
					target_branches=target_b,
					target_lines=0,
					target_functions=0,
				)
				self.assertEqual([t.name for t in pruned_b], ["fast"])
				total_b = sum(t.duration_ms for t in pruned_b)

				# Branch+line candidate set must include both tests to satisfy line coverage too.
				universe_l = _build_line_universe_map(full, include=[], exclude=[])
				fast_ids_l = _collect_test_line_ids(fast, universe=universe_l, include=[], exclude=[])
				slow_ids_l = _collect_test_line_ids(slow, universe=universe_l, include=[], exclude=[])
				tests_bl = [
					TestInfo("fast", Path("/fast"), 1.0, fast_ids_b, fast_ids_l, _empty_bitmap()),
					TestInfo("slow", Path("/slow"), 10.0, slow_ids_b, slow_ids_l, _empty_bitmap()),
				]
				target_l = len(universe_l)
				pruned_bl = _prune_redundant_tests_multi(
					_choose_tests_greedy_multi(
						tests_bl,
						target_branches=target_b,
						target_lines=target_l,
						target_functions=0,
					),
					target_branches=target_b,
					target_lines=target_l,
					target_functions=0,
				)
				self.assertEqual(set(t.name for t in pruned_bl), {"fast", "slow"})
				total_bl = sum(t.duration_ms for t in pruned_bl)
				self.assertGreater(total_bl, total_b)

		def test_candidate_selection_with_function_threshold(self):
			with TemporaryDirectory() as td:
				base = Path(td)
				full = base / "full.lcov"
				fast = base / "fast.lcov"
				slow = base / "slow.lcov"

				# Full suite covers 2 branches and 2 functions.
				full.write_text(
					"TN:\n"
					"SF:/src/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,1\n"
					"FNDA:1,funcA\n"
					"FNDA:1,funcB\n"
					"end_of_record\n",
					encoding="utf-8",
				)

				# Fast covers branches but no functions.
				fast.write_text(
					"TN:\n"
					"SF:/src/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,1\n"
					"FNDA:0,funcA\n"
					"FNDA:0,funcB\n"
					"end_of_record\n",
					encoding="utf-8",
				)

				# Slow covers functions but only one branch.
				slow.write_text(
					"TN:\n"
					"SF:/src/a.cpp\n"
					"BRDA:10,0,0,1\n"
					"BRDA:11,0,0,0\n"
					"FNDA:1,funcA\n"
					"FNDA:1,funcB\n"
					"end_of_record\n",
					encoding="utf-8",
				)

				universe_b = _build_branch_universe_map(full, include=[], exclude=[])
				universe_f = _build_function_universe_map(full, include=[], exclude=[])
				fast_b = _collect_test_branch_ids(fast, universe=universe_b, include=[], exclude=[])
				slow_b = _collect_test_branch_ids(slow, universe=universe_b, include=[], exclude=[])
				fast_f = _collect_test_function_ids(fast, universe=universe_f, include=[], exclude=[])
				slow_f = _collect_test_function_ids(slow, universe=universe_f, include=[], exclude=[])

				t_fast = TestInfo("fast", Path("/fast"), 1.0, fast_b, _empty_bitmap(), fast_f)
				t_slow = TestInfo("slow", Path("/slow"), 10.0, slow_b, _empty_bitmap(), slow_f)

				# Without the function constraint, only fast is needed.
				target_b = len(universe_b)
				pruned_e = _prune_redundant_tests_multi(
					_choose_tests_greedy_multi(
						[t_fast, t_slow],
						target_branches=target_b,
						target_lines=0,
						target_functions=0,
					),
					target_branches=target_b,
					target_lines=0,
					target_functions=0,
				)
				self.assertEqual([t.name for t in pruned_e], ["fast"])
				total_e = sum(t.duration_ms for t in pruned_e)

				# With a function target, both are required.
				target_f = len(universe_f)
				pruned_ef = _prune_redundant_tests_multi(
					_choose_tests_greedy_multi(
						[t_fast, t_slow],
						target_branches=target_b,
						target_lines=0,
						target_functions=target_f,
					),
					target_branches=target_b,
					target_lines=0,
					target_functions=target_f,
				)
				self.assertEqual(set(t.name for t in pruned_ef), {"fast", "slow"})
				total_ef = sum(t.duration_ms for t in pruned_ef)
				self.assertGreater(total_ef, total_e)

		def test_update_tags_add_remove_default(self):
			with TemporaryDirectory() as td:
				root = Path(td)
				src = root / "src"
				src.mkdir(parents=True)
				cpp = src / "a.cpp"
				cpp.write_text(
					"TEST_CASE(\n  \"t1\",\n  \"[x][acceptance]\"\n)\n\n"
					"TEST_CASE(\"t2\",\"[y]\")\n",
					encoding="utf-8",
				)
				_update_tags_in_sources(
					root=root,
					src_dir=None,
					all_tests={"t1", "t2"},
					candidates={"t1"},
					slow_tag="acceptance",
					quiet=True,
				)
				out = cpp.read_text(encoding="utf-8")
				self.assertIn('"[x]"', out)
				self.assertIn('"[y][acceptance]"', out)

		def test_update_tags_custom_slow_tag(self):
			with TemporaryDirectory() as td:
				root = Path(td)
				src = root / "src"
				src.mkdir(parents=True)
				cpp = src / "a.cpp"
				cpp.write_text('TEST_CASE("t1","[x]")\n', encoding="utf-8")
				_update_tags_in_sources(
					root=root,
					src_dir=None,
					all_tests={"t1"},
					candidates=set(),
					slow_tag="slow",
					quiet=True,
				)
				out = cpp.read_text(encoding="utf-8")
				self.assertIn('"[x][slow]"', out)

		def test_update_tags_src_dir_override(self):
			with TemporaryDirectory() as td:
				root = Path(td)
				mysrc = root / "other" / "src"
				mysrc.mkdir(parents=True)
				cpp = mysrc / "a.cpp"
				cpp.write_text('TEST_CASE("t1","[x]")\n', encoding="utf-8")
				_update_tags_in_sources(
					root=root,
					src_dir=mysrc,
					all_tests={"t1"},
					candidates=set(),
					slow_tag="acceptance",
					quiet=True,
				)
				out = cpp.read_text(encoding="utf-8")
				self.assertIn('"[x][acceptance]"', out)

		def test_update_tags_missing_test_errors(self):
			with TemporaryDirectory() as td:
				root = Path(td)
				src = root / "src"
				src.mkdir(parents=True)
				(src / "a.cpp").write_text('TEST_CASE("t2","[x]")\n', encoding="utf-8")
				with self.assertRaises(SystemExit):
					_update_tags_in_sources(
						root=root,
						src_dir=None,
						all_tests={"t1"},
						candidates=set(),
						slow_tag="acceptance",
						quiet=True,
					)

		def test_update_tags_conflict_errors(self):
			with TemporaryDirectory() as td:
				root = Path(td)
				src = root / "src"
				src.mkdir(parents=True)
				(src / "a.cpp").write_text('TEST_CASE("t1","[x]")\n', encoding="utf-8")
				(src / "b.cpp").write_text('TEST_CASE("t1","[y]")\n', encoding="utf-8")
				with self.assertRaises(SystemExit):
					_update_tags_in_sources(
						root=root,
						src_dir=None,
						all_tests={"t1"},
						candidates=set(),
						slow_tag="acceptance",
						quiet=True,
					)

	suite = unittest.defaultTestLoader.loadTestsFromTestCase(_SelfTests)
	result = unittest.TextTestRunner().run(suite)
	return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
	if "--run-tests" in sys.argv[1:]:
		raise SystemExit(_run_self_tests())
	raise SystemExit(main())

