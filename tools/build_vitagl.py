#!/usr/bin/env python3
"""Build the audited vitaGL privately; never replace the SDK's installed copy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

REVISION = "16fe309d87761112b813be77842b20338659c460"
SHARK_REVISION = "df24065e65098b2d1ac533760109ad4367573f28"
OPTIONS = {
    "STORE_DEPTH_STENCIL": "1",
    "HAVE_GLSL_TEXTURE_SIZE": "0",
    "HAVE_GLSL_UBOS": "0",
    "LOG_ERRORS": "1",
    "NO_SPLASHSCREEN": "1",
    "NO_DEBUG": "0",
    "SOFTFP_ABI": "0",
}


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def prepare(source, checkout, revision):
    if not checkout.exists():
        # Materialize partial clones before local upload-pack.
        run("git", "-C", str(source), "archive", revision,
            stdout=subprocess.DEVNULL)
        run("git", "clone", "--local", "--no-hardlinks", "--no-checkout",
            str(source), str(checkout))
        run("git", "-C", str(checkout), "checkout", "--detach", revision)
    head = subprocess.check_output(
        ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
    if head != revision:
        raise RuntimeError("Existing build checkout is not the audited revision")
    run("git", "-C", str(checkout), "diff", "--exit-code", "HEAD", "--")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Local git repository containing the audited revision")
    parser.add_argument("--shark-source", type=Path, required=True,
                        help="Local vitaShaRK git repository containing its pinned revision")
    parser.add_argument("--output", type=Path, required=True,
                        help="Private build directory, not the SDK installation")
    parser.add_argument("--sdk", type=Path, default=os.environ.get("VITASDK"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--single-threaded-gc", action="store_true",
                        help="Run vitaGL's deferred collector on the graphics owner thread")
    args = parser.parse_args()
    options = dict(OPTIONS, SINGLE_THREADED_GC="1" if args.single_threaded_gc else "0")
    if args.sdk is None or args.jobs < 1:
        parser.error("Set VITASDK or --sdk, and use at least one job")
    sdk, output, source = args.sdk.resolve(), args.output.resolve(), args.source.resolve()
    shark_source = args.shark_source.resolve()
    if output == sdk or sdk in output.parents or output == source or source in output.parents:
        parser.error("Output must be separate from the SDK and source repository")
    if output == shark_source or shark_source in output.parents:
        parser.error("Output must be separate from the vitaShaRK source repository")
    cc = sdk / "bin/arm-vita-eabi-gcc"
    if not cc.is_file():
        parser.error("VitaSDK compiler not found")
    output.mkdir(parents=True, exist_ok=True)
    checkout = output / "source"
    prepare(source, checkout, REVISION)
    # Keep the audited checkout pristine. Re-materialize tracked files into a
    # private build tree before applying our reviewed ownership correction.
    patched = output / "patched-source"
    patched.mkdir(exist_ok=True)
    archive = subprocess.Popen(["git", "-C", str(checkout), "archive", REVISION],
                               stdout=subprocess.PIPE)
    try:
        run("tar", "-x", "-C", str(patched), stdin=archive.stdout)
    finally:
        archive.stdout.close()
        if archive.wait() != 0:
            raise RuntimeError("Could not materialize pinned vitaGL source")
    patch = Path(__file__).resolve().parent / "patches/vitagl-stencil-retirement.patch"
    run("patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(patch), cwd=patched)
    shark_checkout = output / "shark-source"
    prepare(shark_source, shark_checkout, SHARK_REVISION)
    # Do not inherit MAKEFLAGS, CFLAGS, or make feature variables from the shell.
    # No safety-bypass feature is set. Force rebuilding objects so changes in
    # command-line options cannot silently reuse an incompatible archive.
    env = {"PATH": str(sdk / "bin") + os.pathsep + os.defpath, "VITASDK": str(sdk)}
    run("make", "-B", "-j" + str(args.jobs), "libvitashark.a",
        "CC=arm-vita-eabi-gcc -mcpu=cortex-a9 -mfloat-abi=hard",
        cwd=shark_checkout, env=env)
    # Private include path precedes the SDK's older vitaShaRK header.
    env["CPATH"] = str(shark_checkout / "source")
    # The upstream Makefile embeds git rev-parse output. The materialized tree
    # has no .git: point it at the audited dependency, not the enclosing app.
    env["GIT_DIR"] = str(checkout / ".git")
    run("make", "-B", "-j" + str(args.jobs), "libvitaGL.a",
        "CC=arm-vita-eabi-gcc -mcpu=cortex-a9 -mfloat-abi=hard",
        "CXX=arm-vita-eabi-g++ -mcpu=cortex-a9 -mfloat-abi=hard",
        *(key + "=" + value for key, value in options.items()),
        cwd=patched, env=env)
    for directory in ("include", "lib"):
        (output / directory).mkdir(exist_ok=True)
    shutil.copy2(patched / "libvitaGL.a", output / "lib/libvitaGL.a")
    shutil.copy2(checkout / "source/vitaGL.h", output / "include/vitaGL.h")
    shutil.copy2(shark_checkout / "libvitashark.a", output / "lib/libvitashark.a")
    shutil.copy2(shark_checkout / "source/vitashark.h", output / "include/vitashark.h")
    shutil.copy2(shark_checkout / "LICENSE", output / "vitaShaRK-LICENSE")
    shutil.copy2(checkout / "COPYING", output / "COPYING")
    shutil.copy2(checkout / "COPYING.LESSER", output / "COPYING.LESSER")
    compiler = subprocess.check_output([str(cc), "--version"], text=True).splitlines()[0]
    (output / "build.json").write_text(json.dumps({
        "revision": REVISION, "shark_revision": SHARK_REVISION, "options": options,
        "patches": {patch.name: hashlib.sha256(patch.read_bytes()).hexdigest()},
        "architecture": "cortex-a9", "float_abi": "hard",
        "compiler": compiler,
    }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
