#!/usr/bin/env python3
"""
RTEF OS Assembly Benchmark
==========================

Compiles each OS source file to x86-64 assembly (-S), extracts per-function
assembly blocks (excluding external call internals), and evaluates:

  1. Instruction count     — total x86 instructions in the function body.
  2. Estimated clock cycles — weighted sum based on instruction latency classes.
  3. Cache-friendliness    — score from 0 (worst) to 10 (best), penalising
                             scattered memory accesses and large code size.
  4. Pipeline-flush risk   — count of instructions that may cause a pipeline
                             flush (conditional branches, indirect calls/jumps,
                             serialising instructions).

Usage:
    python3 benchmark/rtef_benchmark.py [--cc CC] [--cflags FLAGS] [--inc DIRS]

Designed to run from the repository root.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# ──────────────────────────────────────────────────────────────────────
#  Configuration
# ──────────────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent

# OS source files to benchmark (relative to REPO_ROOT)
OS_SOURCES = [
    "os/src/OS_Error.c",
    "os/src/OS_Event.c",
    "os/src/OS_Timer.c",
    "os/src/OS_Hsm.c",
    "os/src/OS_Watchdog.c",
]

# ──────────────────────────────────────────────────────────────────────
#  x86-64 instruction classification tables
# ──────────────────────────────────────────────────────────────────────

# Estimated latency in cycles (simplified, based on Intel Skylake-class)
# Categories: ALU=1, MOV/LEA=1, SHIFT/ROTATE=1, MUL=3, DIV=20-40,
#             BRANCH=1(predicted)/~15(miss), LOAD=4-5, STORE=3-4,
#             SSE/AVX basic=1-3, CALL/RET=~5 overhead.

# Instructions that can cause pipeline flushes / stalls
FLUSH_MNEMONICS = {
    # Conditional branches (misprediction → flush)
    "je", "jne", "jz", "jnz", "jg", "jge", "jl", "jle",
    "ja", "jae", "jb", "jbe", "js", "jns", "jo", "jno",
    "jc", "jnc", "jp", "jnp", "jcxz", "jecxz", "jrcxz",
    "loop", "loope", "loopne",
    # Indirect jumps and calls (hard to predict)
    # These are detected separately via operand analysis
    # Serialising / fencing instructions
    "cpuid", "mfence", "lfence", "sfence", "lock",
    "xchg",  # implicit lock prefix when memory operand
    "cmpxchg", "cmpxchg8b", "cmpxchg16b",
    # System
    "syscall", "sysenter", "int", "iret", "iretd", "iretq",
}

# Instructions whose latency is higher
MULTICYCLE_PREFIXES = {
    "div": 25, "idiv": 25,
    "mul": 3, "imul": 3,
    "sqrt": 15, "rsqrt": 5,
    "call": 5, "ret": 5,
    "push": 2, "pop": 2,
    "cpuid": 100,
    "syscall": 100, "sysenter": 100,
    "lock": 20,
    "mfence": 33, "lfence": 2, "sfence": 2,
    "cmpxchg": 15,
}

# Memory-access mnemonics (for cache analysis)
MEMORY_ACCESS_MNEMONICS = {"mov", "lea", "movzx", "movsx", "movabs",
                           "movd", "movq", "movdqa", "movdqu", "movaps",
                           "movups", "movsd", "movss", "movhps", "movlps",
                           "lods", "stos", "movs", "cmps", "scas"}


# ──────────────────────────────────────────────────────────────────────
#  Data structures
# ──────────────────────────────────────────────────────────────────────

@dataclass
class FunctionMetrics:
    """Metrics for a single function extracted from assembly."""
    name: str
    source_file: str
    instructions: int = 0
    est_cycles: int = 0
    cache_score: float = 10.0   # 0-10, higher is better
    pipeline_flushes: int = 0
    code_bytes_approx: int = 0  # rough estimate
    mem_accesses: int = 0
    branch_count: int = 0
    raw_lines: list[str] = field(default_factory=list, repr=False)


# ──────────────────────────────────────────────────────────────────────
#  Assembly generation
# ──────────────────────────────────────────────────────────────────────

def compile_to_asm(src: Path, cc: str, cflags: str, includes: str,
                   tmp_dir: str) -> Path:
    """Compile a C source to assembly (.s) in the temp directory."""
    out = Path(tmp_dir) / (src.stem + ".s")
    cmd = f"{cc} {cflags} {includes} -S -o {out} {src}"
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                            cwd=str(REPO_ROOT))
    if result.returncode != 0:
        print(f"ERROR compiling {src}:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    return out


# ──────────────────────────────────────────────────────────────────────
#  Assembly parsing — extract per-function blocks
# ──────────────────────────────────────────────────────────────────────

# Pattern for function start: label with @function type or "Begin function"
RE_FUNC_BEGIN = re.compile(
    r"^(\w[\w.]*):.*#\s*@(\w+)|"           # clang: "Name:  # @Name"
    r"^(\w[\w.]*):\s*$"                     # gcc:   "Name:"
)
RE_FUNC_TYPE = re.compile(r"\.type\s+(\w+),\s*@function")
RE_FUNC_END = re.compile(
    r"^\.Lfunc_end|"                        # clang end marker
    r"^\.size\s+\w+,\s*\.\-|"              # gcc end marker
    r"^\s+\.cfi_endproc"                    # fallback
)
RE_DIRECTIVE = re.compile(r"^\s*\.")        # assembler directives
RE_LABEL = re.compile(r"^[\w.]+:\s*$")     # labels on their own line
RE_COMMENT = re.compile(r"^\s*#")           # comment-only lines


def extract_functions(asm_path: Path, source_name: str) -> list[FunctionMetrics]:
    """Parse an assembly file and return metrics per function."""
    lines = asm_path.read_text().splitlines()

    # First pass: collect known function names from .type directives
    known_functions: set[str] = set()
    for line in lines:
        m = RE_FUNC_TYPE.search(line)
        if m:
            known_functions.add(m.group(1))

    # Second pass: extract function bodies
    functions: list[FunctionMetrics] = []
    current: FunctionMetrics | None = None
    in_function = False

    for line in lines:
        # Check for function end
        if in_function and RE_FUNC_END.match(line):
            if current is not None:
                functions.append(current)
            current = None
            in_function = False
            continue

        # Check for function start (label matching a known function)
        if not in_function:
            for fname in known_functions:
                if line.startswith(fname + ":"):
                    current = FunctionMetrics(name=fname,
                                              source_file=source_name)
                    in_function = True
                    break
            continue

        # Inside a function — collect instruction lines
        if current is not None:
            current.raw_lines.append(line)

    return functions


# ──────────────────────────────────────────────────────────────────────
#  Metrics computation
# ──────────────────────────────────────────────────────────────────────

def _get_mnemonic(line: str) -> str | None:
    """Extract the mnemonic from an instruction line, or None."""
    stripped = line.strip()
    if not stripped or stripped.startswith("#") or stripped.startswith("."):
        return None
    # Labels on their own line
    if RE_LABEL.match(stripped):
        return None
    # Remove leading label if present (e.g. ".LBB0_1: movl ...")
    if ":" in stripped:
        stripped = stripped.split(":", 1)[1].strip()
    if not stripped or stripped.startswith("#") or stripped.startswith("."):
        return None
    # First token is the mnemonic (possibly with prefix like "lock")
    tokens = stripped.split()
    mnemonic = tokens[0].lower().rstrip("bwlq")  # strip size suffix
    return mnemonic


def _is_memory_operand(operand: str) -> bool:
    """Check if an operand references memory (has parentheses or offset)."""
    return "(" in operand or "%" not in operand


def _is_indirect_target(line: str) -> bool:
    """Check if a call/jmp uses an indirect target (register/memory)."""
    stripped = line.strip()
    tokens = stripped.split(None, 1)
    if len(tokens) < 2:
        return False
    target = tokens[1].split("#")[0].strip()  # remove comments
    # Indirect: *%rax, *(%rax), *symbol(%rip) with * prefix, or register
    return target.startswith("*") or (target.startswith("%") and
                                       tokens[0].lower().rstrip("q") in
                                       ("call", "jmp"))


def compute_metrics(func: FunctionMetrics) -> None:
    """Fill in all metrics from the raw assembly lines."""
    instr_count = 0
    est_cycles = 0
    mem_accesses = 0
    branch_count = 0
    pipeline_flushes = 0
    code_size = 0

    for line in func.raw_lines:
        mnemonic = _get_mnemonic(line)
        if mnemonic is None:
            continue

        instr_count += 1
        code_size += 4  # rough average x86 instruction ~4 bytes

        # ── Cycle estimation ──
        cycles = 1  # default: simple ALU/MOV
        for prefix, cost in MULTICYCLE_PREFIXES.items():
            if mnemonic.startswith(prefix):
                cycles = cost
                break
        est_cycles += cycles

        # ── Memory access counting ──
        base_mnemonic = mnemonic.rstrip("bwlq")
        if base_mnemonic in MEMORY_ACCESS_MNEMONICS:
            # Check if operands actually reference memory
            stripped = line.strip()
            parts = stripped.split(None, 1)
            if len(parts) > 1:
                operands = parts[1].split("#")[0]  # remove comment
                if "(" in operands or ("%" not in operands and
                                        "$" not in operands):
                    mem_accesses += 1
                elif "," in operands:
                    # Two-operand: check each side
                    ops = operands.split(",")
                    for op in ops:
                        if "(" in op.strip():
                            mem_accesses += 1
                            break

        # ── Branch / pipeline flush detection ──
        is_branch = mnemonic.startswith("j") and mnemonic != "jmp"
        is_cond_branch = is_branch
        is_loop = mnemonic.startswith("loop")

        if is_cond_branch or is_loop:
            branch_count += 1
            pipeline_flushes += 1  # potential misprediction

        if mnemonic in ("call", "jmp", "callq", "jmpq"):
            if _is_indirect_target(line):
                pipeline_flushes += 1  # indirect → hard to predict

        if mnemonic in FLUSH_MNEMONICS and not is_cond_branch:
            pipeline_flushes += 1

        # Detect lock prefix (appears as separate token sometimes)
        if "lock" in line.lower().split() and mnemonic != "lock":
            pipeline_flushes += 1

    func.instructions = instr_count
    func.est_cycles = est_cycles
    func.mem_accesses = mem_accesses
    func.branch_count = branch_count
    func.pipeline_flushes = pipeline_flushes
    func.code_bytes_approx = code_size

    # ── Cache-friendliness score (0-10) ──
    # Penalties:
    #   - Large code size (> 256 bytes ≈ 4 cache lines at 64B)
    #   - High memory access density
    #   - Many branches (poor spatial locality)
    score = 10.0
    if code_size > 512:
        score -= min(3.0, (code_size - 512) / 256.0)
    if code_size > 1024:
        score -= 1.0  # extra penalty for very large functions

    if instr_count > 0:
        mem_density = mem_accesses / instr_count
        if mem_density > 0.5:
            score -= min(2.0, (mem_density - 0.5) * 4.0)

        branch_density = branch_count / instr_count
        if branch_density > 0.15:
            score -= min(2.0, (branch_density - 0.15) * 10.0)

    # Bonus for very small, tight functions
    if instr_count <= 20:
        score += 0.5
    if instr_count <= 10:
        score += 0.5

    func.cache_score = max(0.0, min(10.0, score))


# ──────────────────────────────────────────────────────────────────────
#  Table output
# ──────────────────────────────────────────────────────────────────────

HEADER = ("Function", "Source", "Instructions", "Est. Cycles",
          "Cache Score", "Pipeline Flushes")
SEP = "│"

def _col_widths(rows: list[tuple[str, ...]]) -> list[int]:
    widths = [len(h) for h in HEADER]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))
    return widths


def print_table(metrics: list[FunctionMetrics]) -> None:
    """Print a nicely formatted benchmark table."""
    rows: list[tuple[str, ...]] = []
    for m in metrics:
        rows.append((
            m.name,
            m.source_file,
            str(m.instructions),
            str(m.est_cycles),
            f"{m.cache_score:.1f}/10",
            str(m.pipeline_flushes),
        ))

    widths = _col_widths(rows)

    def fmt_row(cells: tuple[str, ...]) -> str:
        parts = []
        for i, cell in enumerate(cells):
            if i >= 2:  # numeric columns right-aligned
                parts.append(cell.rjust(widths[i]))
            else:
                parts.append(cell.ljust(widths[i]))
        return f" {SEP} ".join(parts)

    line_sep = "─" * (sum(widths) + 3 * len(widths) + 1)

    print()
    print("╔" + "═" * (len(line_sep)) + "╗")
    print("║  RTEF OS  —  Assembly Benchmark Results"
          + " " * (len(line_sep) - 41) + "║")
    print("╚" + "═" * (len(line_sep)) + "╝")
    print()
    print(fmt_row(HEADER))
    print(line_sep)

    current_source = ""
    for i, row in enumerate(rows):
        if row[1] != current_source:
            if current_source:
                print(line_sep)
            current_source = row[1]
        print(fmt_row(row))

    print(line_sep)

    # Summary
    total_instr = sum(m.instructions for m in metrics)
    total_cycles = sum(m.est_cycles for m in metrics)
    avg_cache = (sum(m.cache_score for m in metrics) / len(metrics)
                 if metrics else 0)
    total_flushes = sum(m.pipeline_flushes for m in metrics)

    print()
    print(f"  Total functions analysed : {len(metrics)}")
    print(f"  Total instructions       : {total_instr}")
    print(f"  Total estimated cycles   : {total_cycles}")
    print(f"  Average cache score      : {avg_cache:.1f}/10")
    print(f"  Total pipeline flushes   : {total_flushes}")
    print()


# ──────────────────────────────────────────────────────────────────────
#  Main
# ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="RTEF OS Assembly Benchmark")
    parser.add_argument("--cc", default="clang",
                        help="C compiler (default: clang)")
    parser.add_argument("--cflags",
                        default="-std=c11 -Wall -Wextra -Werror -Wpedantic -O2",
                        help="Compiler flags")
    parser.add_argument("--inc", default="-Ios/inc",
                        help="Include paths")
    args = parser.parse_args()

    all_metrics: list[FunctionMetrics] = []

    with tempfile.TemporaryDirectory(prefix="rtef_bench_") as tmp_dir:
        for src_rel in OS_SOURCES:
            src = REPO_ROOT / src_rel
            if not src.exists():
                print(f"WARNING: {src} not found, skipping.", file=sys.stderr)
                continue

            asm_path = compile_to_asm(src, args.cc, args.cflags, args.inc,
                                      tmp_dir)
            source_name = Path(src_rel).name
            funcs = extract_functions(asm_path, source_name)

            for f in funcs:
                compute_metrics(f)

            all_metrics.extend(funcs)

    # Sort by source file, then by name
    all_metrics.sort(key=lambda m: (m.source_file, m.name))

    print_table(all_metrics)


if __name__ == "__main__":
    main()
