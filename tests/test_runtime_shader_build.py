import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runtime_shader_build", ROOT / "tools/compile_shader.py")
shader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shader)


class RuntimeShaderBuildTests(unittest.TestCase):
    def test_nested_includes_definitions_and_line_origins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "inner.h").write_text("float inner;\n")
            (root / "common.cg").write_text('#include "inner.h"\nfloat shared;\n')
            source = root / "main.cg"
            source.write_text('#ifdef COLOR\n#include "common.cg"\n#endif\nfloat result;\n')
            result = shader.runtime_source(source, ["COLOR=3", "FAST"], "pixel_f", "sce_fp_psp2")
            text = result.decode()
            self.assertIn("#define COLOR 3\n#define FAST 1\n", text)
            self.assertIn("#ifdef COLOR", text)
            self.assertIn("#endif", text)
            self.assertIn('#line 1 "inner.h"\nfloat inner;', text)
            self.assertIn('#line 3 "main.cg"', text)
            self.assertNotRegex(text, r"(?m)^\s*#\s*include")
            encoded = shader.embed(result + b"\0", "pixel_f")
            decoded = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", encoded))
            self.assertEqual(decoded, result + b"\0")

    def test_nested_filename_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "local file.cg").write_text("float value;\n")
            source = root / "main.cg"
            source.write_text('#include "local file.cg" // local only\n')
            self.assertIn("float value;", shader.expand_includes(source))

    def test_cycles_missing_and_external_includes_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "main.cg"
            for include in ('"main.cg"', '"missing.cg"', '"../outside.cg"', '<system.h>', 'MACRO'):
                with self.subTest(include=include):
                    source.write_text("#include " + include + "\n")
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        shader.expand_includes(source)

    def test_nul_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.cg"
            source.write_bytes(b"float x;\0float y;")
            with self.assertRaises(ValueError):
                shader.expand_includes(source)

    def test_bad_define_or_symbol_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.cg"
            source.write_text("float x;\n")
            for defines, name in [(["X\n#error"], "ok"), (["X="], "ok"), ([], "bad;")]:
                with self.assertRaises(ValueError):
                    shader.runtime_source(source, defines, name, "sce_fp_psp2")

    def test_runtime_cli_does_not_invoke_external_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "main.cg", root / "main.h"
            source.write_text("float x;\n")
            argv = ["compile_shader.py", "--runtime-source", "--profile", "sce_fp_psp2",
                    "--source", str(source), "--output", str(output), "--symbol", "main_f"]
            with patch("sys.argv", argv), patch.object(shader.subprocess, "run") as run:
                shader.main()
                run.assert_not_called()
            self.assertTrue(output.is_file())
            self.assertTrue(output.with_suffix(".source.cg").is_file())
            self.assertFalse(output.with_suffix(".gxp").exists())

    def test_every_production_cg_template_embeds(self):
        sources = sorted((ROOT / "src/video/opengl/shaders").glob("*.cg"))
        self.assertGreater(len(sources), 50)
        for source in sources:
            with self.subTest(shader=source.name):
                result = shader.runtime_source(source, [], "probe", "sce_fp_psp2")
                self.assertNotIn(b"\0", result)
                self.assertNotRegex(result.decode(), r"(?m)^\s*#\s*include")


if __name__ == "__main__":
    unittest.main()
