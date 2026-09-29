#!/usr/bin/env python3
"""Embed offline GXP or self-contained Cg for Vita runtime compilation."""
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


def expand_includes(source, root=None, stack=()):
    """Inline local includes without evaluating shader preprocessor branches."""
    source = Path(source).resolve()
    root = source.parent if root is None else Path(root).resolve()
    if not source.is_relative_to(root):
        raise ValueError("Shader include escapes its source directory")
    if source in stack:
        raise ValueError("Cyclic shader include: " + source.name)
    text = source.read_text(encoding="utf-8")
    if "\0" in text:
        raise ValueError("NUL in shader source")
    name = source.relative_to(root).as_posix()
    if any(c in name for c in '\"\n\r\\'):
        raise ValueError("Unsupported shader include filename")
    output = [f'#line 1 "{name}"\n']
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        if re.match(r"\s*#\s*include\b", line):
            match = re.fullmatch(r'\s*#\s*include\s*"([^"\n]+)"\s*(?://[^\n]*)?\n?', line)
            if not match:
                raise ValueError("Only quoted local shader includes are supported")
            output.append(expand_includes(source.parent / match[1], root, (*stack, source)))
            output.append(f'\n#line {number + 1} "{name}"\n')
        else:
            output.append(line)
    return "".join(output)


def runtime_source(source, definitions, symbol, profile):
    define_options(definitions)  # Same accepted definitions as the offline path.
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol):
        raise ValueError("Invalid shader symbol")
    output = [f"// Runtime Cg: {symbol}, {profile}\n"]
    for definition in definitions:
        name, separator, value = definition.partition("=")
        output.append(f"#define {name} {value if separator else '1'}\n")
    output.append(expand_includes(source))
    return "".join(output).encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--compiler")
    mode.add_argument("--runtime-source", action="store_true")
    parser.add_argument("--profile", choices=["sce_vp_psp2", "sce_fp_psp2"], required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--define", action="append", default=[])
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.runtime_source:
        source = runtime_source(args.source, args.define, args.symbol, args.profile)
        args.output.with_suffix(".source.cg").write_bytes(source)
        args.output.write_text(embed(source + b"\0", args.symbol), encoding="ascii")
        return
    gxp = args.output.with_suffix(".gxp")
    windows = args.compiler.lower().endswith(".exe") and "microsoft" in platform.release().lower()
    subprocess.run([args.compiler, "--profile", args.profile, "-bestprecision", *define_options(args.define),
                    compiler_path(args.source, windows), "-o", compiler_path(gxp, windows)], check=True)
    args.output.write_text(embed(gxp.read_bytes(), args.symbol), encoding="ascii")


if __name__ == "__main__":
    main()
