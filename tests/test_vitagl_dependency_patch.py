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


if __name__ == "__main__":
    unittest.main()
