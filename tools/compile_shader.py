#!/usr/bin/env python3
"""Compile a Vita shader using a user-supplied compiler; embed the resulting GXP."""
import argparse
from pathlib import Path
import platform
import re
import subprocess


def compiler_path(path, windows):
    path = str(Path(path).resolve())
    if windows:
        return subprocess.check_output(["wslpath", "-w", path], text=True).strip()
    return path


def embed(data, symbol):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol):
        raise ValueError("Invalid shader symbol")
    if not data:
        raise ValueError("Compiler produced an empty shader")
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16])
            for i in range(0, len(data), 16)]
    return ("/* Generated shader; do not edit. */\n#pragma once\n"
            f"static const unsigned char {symbol}[] __attribute__((aligned(4))) = {{\n"
            + ",\n".join(rows) + "\n};\n")


def define_options(definitions):
    for definition in definitions:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(=-?[0-9]+)?", definition):
            raise ValueError("Shader macros must be identifiers with optional integer values")
    return [f"-D{definition}" for definition in definitions]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--profile", choices=["sce_vp_psp2", "sce_fp_psp2"], required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--define", action="append", default=[])
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    gxp = args.output.with_suffix(".gxp")
    windows = args.compiler.lower().endswith(".exe") and "microsoft" in platform.release().lower()
    subprocess.run([args.compiler, "--profile", args.profile, "-bestprecision", *define_options(args.define),
                    compiler_path(args.source, windows), "-o", compiler_path(gxp, windows)], check=True)
    args.output.write_text(embed(gxp.read_bytes(), args.symbol), encoding="ascii")


if __name__ == "__main__":
    main()
