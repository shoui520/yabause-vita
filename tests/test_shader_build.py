import importlib.util
import math
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "compile_shader", Path(__file__).resolve().parents[1] / "tools" / "compile_shader.py")
shader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shader)


class ShaderBuildTests(unittest.TestCase):
    def test_packed_byte_rounding_is_independent_of_integer_cast_ties(self):
        source = (Path(__file__).resolve().parents[1] /
                  "src/video/opengl/shaders/packed_texture.h").read_text()
        self.assertIn("(int)floor(value * 255.0f + 0.5f)", source)
        # Physical Cg execution exposed ties-to-even for the direct cast.
        # This models that failure; host C truncation alone cannot catch it.
        self.assertEqual(sum(round(n + .5) != n for n in range(256)), 128)
        for n in range(256):
            for error in (-1/4096, 0, 1/4096):
                integral = math.floor((n/255 + error)*255 + .5)
                self.assertEqual(round(integral), n)
                self.assertEqual(int(integral), n)

    def test_vitagl_matrix_storage_matches_gl_uploads(self):
        root = Path(__file__).resolve().parents[1] / "src/video/opengl/shaders"
        for name in ("normal_v", "sprite_v", "framebuffer_v", "window_v"):
            with self.subTest(shader=name):
                source = (root / (name + ".cg")).read_text()
                self.assertIn("uniform column_major float4x4 u_mvpMatrix;", source)

        # YglOrtho's row-major C bytes, uploaded GL_FALSE, are the columns
        # seen by GLSL/Cg column_major. The shaders multiply vector * matrix.
        coefficients = [2/320, 0, 0, -1, 0, -2/224, 0, 1,
                        0, 0, -1, 0, 0, 0, 0, 1]
        for vertex, expected in (((0, 0, 0, 1), (-1, 1, 0, 1)),
                                 ((320, 224, 0, 1), (1, -1, 0, 1))):
            result = [sum(vertex[row] * coefficients[col * 4 + row]
                          for row in range(4)) for col in range(4)]
            for actual, wanted in zip(result, expected):
                self.assertAlmostEqual(actual, wanted)

    def test_embed_is_aligned_and_preserves_all_bytes(self):
        data = bytes(range(256))
        result = shader.embed(data, "tile_v")
        self.assertIn("aligned(4)", result)
        values = shader.re.findall(r"0x([0-9a-f]{2})", result)
        self.assertEqual(bytes(int(v, 16) for v in values), data)

    def test_empty_output_is_rejected(self):
        with self.assertRaises(ValueError):
            shader.embed(b"", "tile_v")

    def test_symbol_cannot_inject_source(self):
        for symbol in ("", "0tile", "tile;", "tile\n#error"):
            with self.assertRaises(ValueError):
                shader.embed(b"test", symbol)

    def test_native_path_does_not_call_wsl(self):
        with patch.object(shader.subprocess, "check_output") as call:
            result = shader.compiler_path("shader.cg", False)
            self.assertTrue(Path(result).is_absolute())
            call.assert_not_called()

    def test_windows_path_is_one_argument_even_with_spaces(self):
        with patch.object(shader.subprocess, "check_output", return_value="C:\\shader files\\tile.cg\n") as call:
            result = shader.compiler_path("shader files/tile.cg", True)
            self.assertEqual(result, "C:\\shader files\\tile.cg")
            self.assertEqual(call.call_args.args[0][:2], ["wslpath", "-w"])
            self.assertEqual(len(call.call_args.args[0]), 3)


if __name__ == "__main__":
    unittest.main()
