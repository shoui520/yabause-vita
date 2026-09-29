"""Check the private patch applies exactly and retains deferred disposal."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StencilRetirementTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("patch"), "patch command required by dependency builder")
    def test_all_five_retirement_sites_keep_depth_and_retire_optional_stencil(self):
        patch = Path(__file__).resolve().parents[1] / "tools/patches/vitagl-stencil-retirement.patch"
        sites = {"framebuffers.c": [(284, 4), (368, 2), (400, 2)],
                 "shared.h": [(1391, 4)], "textures.c": [(1830, 8)]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "source").mkdir()
            for name, locations in sites.items():
                lines = ["// fixture\n"] * (locations[-1][0] + 2)
                for line, indent in locations:
                    lines[line - 1] = "\t" * indent + "mark_as_dirty(fb->depthbuffer_ptr->depthData);\n"
                (root / "source" / name).write_text("".join(lines))
            subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                           cwd=root, check=True, capture_output=True)
            for name, locations in sites.items():
                text = (root / "source" / name).read_text()
                self.assertEqual(text.count("mark_as_dirty(fb->depthbuffer_ptr->depthData)"), len(locations))
                self.assertEqual(text.count("if (fb->depthbuffer_ptr->stencilData)"), len(locations))
                self.assertEqual(text.count("mark_as_dirty(fb->depthbuffer_ptr->stencilData)"), len(locations))
                self.assertNotIn("free(", text)


class ShaderCacheTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("patch"), "patch command required by dependency builder")
    def test_cache_skips_failures_and_uses_flat_directories(self):
        patch = Path(__file__).resolve().parents[1] / "tools/patches/vitagl-shader-cache.patch"
        # Rebuild each patched file's original lines at their recorded positions.
        originals, target = {}, None
        for line in patch.read_text().splitlines(keepends=True):
            if line.startswith("--- a/"):
                target = originals.setdefault(line[6:].strip(), {})
            elif line.startswith("@@"):
                number = int(line.split()[1][1:].split(",")[0])
            elif line.startswith((" ", "-")) and target is not None:
                target[number] = line[1:]
                number += 1
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "source").mkdir()
            for name, lines in originals.items():
                text = [lines.get(n, "// fixture\n") for n in range(1, max(lines) + 4)]
                (root / name).write_text("".join(text))
            subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                           cwd=root, check=True, capture_output=True)
            shaders = (root / "source/custom_shaders.c").read_text()
            init = (root / "source/vgl.c").read_text()
            self.assertLess(shaders.index("if (!s->prog)\n\t\treturn;"),
                            shaders.index("sceIoOpen(cache_fname"))
            self.assertNotIn("%02X/%llX.gxp", shaders)
            self.assertEqual(shaders.count("/%llX.gxp"), 3)
            self.assertNotIn("0xFF", init)
            self.assertEqual(init.count("sceIoMkdir(fname, 0777);"), 2)


if __name__ == "__main__":
    unittest.main()
