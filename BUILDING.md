# Building and testing

Use CMake 3.21 or newer, Ninja, Python 3 and a VitaSDK installation with the
project's link dependencies (including FLAC, ogg, mathneon, taiHEN and
SceShaccCgExt). Set `VITASDK` to your SDK directory.

The accelerated renderer needs a separately built, patched vitaGL. Supply local
vitaGL and vitaShaRK Git checkouts to the builder; it selects the pinned revisions
and does not modify those checkouts or the SDK installation:

```sh
python3 tools/build_vitagl.py --source /path/to/vitaGL \
  --shark-source /path/to/vitaShaRK \
  --output build/deps/vitagl-serial-gc --single-threaded-gc
```

Set `YABAUSE_PSP2CGC` to your legally obtained PSP2 shader compiler. No Sony SDK,
compiler, BIOS or game data is distributed here. Then build the tested baseline:

```sh
cmake --preset vita-baseline
cmake --build --preset vita-baseline
```

The VPK is produced in `build/vita-baseline/`. The preset records the current
baseline's feature switches, including asynchronous audio playback; experimental
paths that did not pass performance gates remain disabled. It is a research
baseline, not a claim of broad game compatibility. A plain CMake configuration
has conservative defaults and is not equivalent to this preset.

`tools/compile_shader.py` supports native compiler executables and Windows
executables from WSL. Machine-specific overrides belong in the ignored
`CMakeUserPresets.json` or environment variables, not source files.

## Regression tests

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
bash tools/test_audio_pcm.sh
bash tools/test_telemetry.sh
```

Other `tools/test_*.sh` scripts test the corresponding CPU or renderer component.
Most require a host C/C++ compiler and sanitizers. ARM execution tests also need
`arm-linux-gnueabihf-gcc`, `arm-linux-gnueabihf-g++` and `qemu-arm`; some use VitaSDK
binutils. Inspect each script for its requirements and optional tool overrides.
Host tests cannot establish on-device performance or full Saturn compatibility.

The public tree deliberately excludes private device-control harnesses, machine
paths, hardware logs and agent notes.
