#!/usr/bin/env python3
"""Check generated runtime payloads against the original variant preprocessor inputs.
This verifies packaging, not Cg compilation or GPU equivalence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def preprocessed(compiler, source, definitions=()):
    command = [compiler, "-E", "-P", "-x", "c", *definitions, str(source)]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return " ".join(result.stdout.split())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--compiler", default="clang")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    shaders = root / "src/video/opengl/shaders"
    generated = args.build.resolve() / "vitagl"
    payloads = sorted(generated.glob("*.source.cg"))
    if not payloads:
        raise RuntimeError("No runtime Cg payloads; refusing empty coverage")
    digest = hashlib.sha256()
    for payload in payloads:
        text = payload.read_text()
        origin = re.search(r'^#line 1 "([^"]+)"$', text, re.MULTILINE)
        if not origin:
            raise RuntimeError("Missing shader origin: " + payload.name)
        source = (shaders / origin[1]).resolve()
        if not source.is_relative_to(shaders.resolve()):
            raise RuntimeError("Invalid shader source path")
        prefix = text[:origin.start()]
        definitions = ["-D" + name + "=" + value for name, value in
                       re.findall(r"^#define ([A-Za-z_][A-Za-z_0-9]*) (-?[0-9]+)$", prefix, re.MULTILINE)]
        header = payload.with_name(payload.name.removesuffix(".source.cg") + ".h")
        raw = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", header.read_text()))
        if raw != payload.read_bytes() + b"\0":
            raise RuntimeError("Embedded bytes differ: " + payload.name)
        original = preprocessed(args.compiler, source, definitions)
        bundled = preprocessed(args.compiler, payload)
        if original != bundled:
            raise RuntimeError("Variant preprocessor output differs: " + payload.name)
        digest.update(payload.name.encode() + b"\0" + raw)
    result = {"status": "PASS", "payloads": len(payloads),
              "payload_sha256": digest.hexdigest(),
              "scope": "Embedded bytes and variant preprocessor equivalence; no GPU execution"}
    (args.build.resolve().parent / "runtime-payload-validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
