"""Unit tests of tools/refcompare.py on synthetic images (no game needed).

    python -m unittest discover -s tools/tests        (or: python -m pytest tools/tests)
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import refcompare as rc  # noqa: E402


def thr(spot="x", data=None):
    return rc.Thresholds(data).for_spot(spot)


def cmp(fa, fb, **kw):
    return rc.compare_spot("s", None, None, kw.pop("thr", thr()), frames_a=fa, frames_b=fb, **kw)


def save(path: Path, img):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(np.rint(img), 0, 255).astype(np.uint8)).save(path)


def run_main(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = rc.main([str(a) for a in argv])
    return code, out.getvalue()


class Metrics(unittest.TestCase):
    def setUp(self):
        self.a = rc.synthetic()

    def test_identical_pass(self):
        r = cmp([self.a], [self.a.copy()])
        self.assertEqual(r["verdict"], "PASS")
        m = r["metrics"]
        self.assertEqual(m["mae"], 0)
        self.assertEqual(m["psnr"], rc.PSNR_CAP)
        self.assertAlmostEqual(m["ssim"], 1.0, places=6)
        self.assertEqual(m["shift"], [0, 0])
        self.assertFalse(m["shift_significant"])
        self.assertEqual(m["noise"], 0)
        self.assertEqual(m["tiles"], [])

    def test_one_pixel_shift_detected(self):
        for dx, dy in ((1, 0), (0, 1), (-1, 0), (2, -1)):
            r = cmp([self.a], [rc.shifted(self.a, dx, dy)])
            m = r["metrics"]
            self.assertEqual(m["shift"], [dx, dy], (dx, dy))
            self.assertTrue(m["shift_significant"])
            self.assertNotEqual(r["verdict"], "PASS")
            self.assertTrue(any(x.startswith("shift") for x in r["reasons"]), r["reasons"])

    def test_shift_only_rule_gives_warn(self):
        # with lax structure rules, a significant shift alone keeps a spot out of PASS
        lax = thr(data={"default": {"pass": {"ssim_min": 0, "bad32_max": 100, "psnr_q_min": 0},
                                    "warn": {"ssim_min": 0, "bad32_max": 100}}})
        r = cmp([self.a], [rc.shifted(self.a, 1, 0)], thr=lax)
        self.assertEqual(r["verdict"], "WARN")
        self.assertEqual(r["reasons"], ["shift 1 > 0 (pass)"])

    def test_brightness_bias_detected(self):
        r = cmp([self.a], [np.clip(self.a + 5, 0, 255)])
        m = r["metrics"]
        self.assertAlmostEqual(m["dluma"], 5, delta=0.1)
        for c in m["bias_rgb"]:
            self.assertAlmostEqual(c, 5, delta=0.2)
        self.assertEqual(m["shift"], [0, 0])
        self.assertEqual(r["verdict"], "FAIL")
        r = cmp([self.a], [np.clip(self.a - 2, 0, 255)])
        self.assertAlmostEqual(r["metrics"]["dluma"], -2, delta=0.1)
        self.assertEqual(r["verdict"], "WARN")

    def test_colour_bias(self):
        b = self.a.copy()
        b[..., 2] = np.clip(b[..., 2] + 6, 0, 255)
        m = cmp([self.a], [b])["metrics"]
        self.assertAlmostEqual(m["chroma"][1], 6, delta=0.3)  # B-G
        self.assertAlmostEqual(m["chroma"][0], 0, delta=0.01)  # R-G
        self.assertAlmostEqual(m["bias_rgb"][2], 6, delta=0.3)

    def test_missing_square_and_worst_tile(self):
        b = self.a.copy()
        b[64:96, 128:160] = 0  # exactly one 32x32 tile
        r = cmp([self.a], [b])
        self.assertEqual(r["verdict"], "FAIL")
        t = r["metrics"]["tiles"][0]
        self.assertEqual((t["x"], t["y"]), (128, 64))
        self.assertGreater(r["metrics"]["bad32"], 1)

    def test_psnr_q_tolerates_subpixel_noise(self):
        rng = np.random.default_rng(3)
        b = np.clip(self.a + rng.choice([-3, 3], self.a.shape), 0, 255)
        m = cmp([self.a], [b])["metrics"]
        self.assertGreater(m["psnr_q"], m["psnr"] + 8)

    def test_size_mismatch_invalid(self):
        r = cmp([self.a], [self.a[:, :-8]])
        self.assertEqual(r["verdict"], "INVALID")
        self.assertIn("size mismatch", r["reasons"][0])

    def test_missing_side_invalid(self):
        r = cmp([self.a], None)
        self.assertEqual(r["verdict"], "INVALID")
        self.assertEqual(r["reasons"], ["missing in B"])


class Noise(unittest.TestCase):
    def setUp(self):
        self.a = rc.synthetic()
        self.rng = np.random.default_rng(11)

    def burst(self, n=3, region=(slice(10, 40), slice(10, 60))):
        frames = [self.a.copy() for _ in range(n)]
        for f in frames:
            f[region] = self.rng.integers(0, 256, f[region].shape)
        return frames

    def test_burst_noise_masked(self):
        r = cmp(self.burst(), self.burst())
        self.assertEqual(r["verdict"], "PASS")
        m = r["metrics"]
        # 30x50 region dilated by 2 px -> 34x54 of 144x256
        self.assertAlmostEqual(m["noise"], 100 * 34 * 54 / (144 * 256), delta=0.5)
        self.assertEqual(m["mae"], 0)

    def test_noise_without_burst_fails(self):
        r = cmp(self.burst(1), self.burst(1))
        self.assertEqual(r["verdict"], "FAIL")

    def test_noise_on_one_side_masks_both(self):
        r = cmp(self.burst(), [self.a])
        self.assertEqual(r["verdict"], "PASS")
        self.assertGreater(r["metrics"]["noise"], 3)

    def test_too_much_noise_invalid(self):
        r = cmp(self.burst(region=(slice(0, 100), slice(0, 200))), [self.a])
        self.assertEqual(r["verdict"], "INVALID")
        self.assertIn("noise", r["reasons"][0])

    def test_lightning_frame_dropped(self):
        flash = np.clip(self.a * 1.6 + 40, 0, 255)
        r = cmp([self.a, flash, self.a.copy()], [self.a])
        self.assertEqual(r["burst"]["a"]["dropped"], [1])
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["metrics"]["noise"], 0)


class Pose(unittest.TestCase):
    META = {"id": "s", "camera": {"x": 1, "y": 2, "z": 3, "dx": 0, "dy": 0, "dz": 1, "fov": 1.0},
            "weather": {"mode": 2, "weights": {"Sunny_Full": 1.0}}, "time": [13, 0]}

    def setUp(self):
        self.a = rc.synthetic()

    def meta(self, **over):
        m = json.loads(json.dumps(self.META))
        for k, v in over.items():
            if isinstance(v, dict) and isinstance(m.get(k), dict):
                m[k].update(v)
            else:
                m[k] = v
        return m

    def test_same_pose_pass(self):
        r = cmp([self.a], [self.a], meta_a=self.meta(), meta_b=self.meta(camera={"x": 1.5}))
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["pose"]["unchecked"], [])

    def test_mismatches_invalid(self):
        cases = [dict(camera={"x": 3}), dict(camera={"dx": 0.01}), dict(time=[13, 5]),
                 dict(weather={"mode": 0}), dict(weather={"weights": {"Sunny_Full": 0.5, "Foggy": 0.5}})]
        for over in cases:
            r = cmp([self.a], [self.a], meta_a=self.meta(), meta_b=self.meta(**over))
            self.assertEqual(r["verdict"], "INVALID", over)
            self.assertTrue(r["reasons"][0].startswith("pose/state mismatch"), over)

    def test_clock_wraps_midnight(self):
        r = cmp([self.a], [self.a], meta_a=self.meta(time=[23, 59.5]), meta_b=self.meta(time={"h": 0, "m": 0}))
        self.assertEqual(r["verdict"], "PASS")

    def test_load_meta_formats(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "spass.log"
            log.write_text("12:00 [info] boot\n12:01 [info] RVKCMP " + json.dumps(self.META) + "\n"
                           "12:02 [info] RVKCMP " + json.dumps(dict(self.META, id="t")) + "\n", encoding="utf-8")
            self.assertEqual(set(rc.load_meta(log)), {"s", "t"})
            js = Path(d) / "meta.json"
            js.write_text(json.dumps({"s": self.META}), encoding="utf-8")
            self.assertEqual(rc.load_meta(js)["s"]["time"], [13, 0])
            js.write_text(json.dumps([self.META]), encoding="utf-8")
            self.assertIn("s", rc.load_meta(js))


class Thresholds(unittest.TestCase):
    def test_toml_and_spot_override(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.toml"
            p.write_text('[default.pass]\nssim_min = 0.9\n\n[spot.gui]\nknown = "glyphs"\n[spot.gui.warn]\nbad32_max = 0.1\n',
                         encoding="utf-8")
            t = rc.Thresholds.load(p)
            self.assertEqual(t.for_spot("x")["pass"]["ssim_min"], 0.9)
            self.assertEqual(t.for_spot("x")["pass"]["bad32_max"], 0.2)  # built-in default kept
            g = t.for_spot("gui")
            self.assertEqual(g["warn"]["bad32_max"], 0.1)
            self.assertEqual(g["known"], "glyphs")

    def test_derive_never_stricter(self):
        base = thr()
        perfect = {"ssim": 1.0, "psnr": 100, "psnr_q": 100, "mae": 0, "bad8": 0, "bad32": 0, "dluma": 0,
                   "chroma": [0, 0], "shift": [0, 0], "shift_significant": False}
        self.assertEqual(rc.derive_thresholds(base, perfect, 1.5), {})
        noisy = dict(perfect, ssim=0.97, bad32=0.5, dluma=-0.8, psnr_q=36.0)
        o = rc.derive_thresholds(base, noisy, 1.5)
        self.assertAlmostEqual(o["pass"]["ssim_min"], 0.955)
        self.assertAlmostEqual(o["pass"]["bad32_max"], 0.75)
        self.assertAlmostEqual(o["pass"]["dluma_abs_max"], 1.2)
        self.assertAlmostEqual(o["pass"]["psnr_q_min"], 36.0 - 3.52, places=2)
        self.assertNotIn("dluma_abs_max", o.get("warn", {}))  # 1.2 < 3.0


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.a = rc.synthetic()

    def tearDown(self):
        self.tmp.cleanup()

    def test_compare_end_to_end(self):
        A, B = self.d / "d3d9", self.d / "vk"
        rng = np.random.default_rng(5)
        for k in range(3):  # burst with an unstable region on both sides; tag prefixes d3d9_ / vk_
            f = self.a.copy()
            f[0:20, 0:30] = rng.integers(0, 256, (20, 30, 3))
            save(A / f"d3d9_beach_b{k}.png", f)
            f[0:20, 0:30] = rng.integers(0, 256, (20, 30, 3))
            save(B / f"vk_beach_b{k}.png", f)
        save(A / "d3d9_dark.png", self.a)
        save(B / "vk_dark.png", np.clip(self.a - 6, 0, 255))
        code, out = run_main(["compare", A, B, "-o", self.d / "out"])
        self.assertEqual(code, 1, out)
        rep = json.loads((self.d / "out" / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(rep["summary"]["PASS"], 1)
        self.assertEqual(rep["summary"]["FAIL"], 1)
        self.assertEqual(rep["spots"][0]["spot"], "dark")  # sorted by severity
        beach = rep["spots"][1]
        self.assertEqual(beach["files_a"], ["d3d9_beach_b0.png", "d3d9_beach_b1.png", "d3d9_beach_b2.png"])
        for key in ("diff", "ssim", "blink", "side"):
            self.assertTrue((self.d / "out" / beach["images"][key]).is_file(), key)
        html = (self.d / "out" / "report.html").read_text(encoding="utf-8")
        self.assertIn("data:image/jpeg;base64,", html)
        self.assertTrue((self.d / "out" / "summary.md").is_file())
        with Image.open(self.d / "out" / "beach_blink.gif") as g:
            self.assertEqual(g.n_frames, 2)
        # the heat map shows the masked region hatched (grey) and the rest dim
        heat = np.asarray(Image.open(self.d / "out" / "beach_diff.png"))
        self.assertTrue(np.all(heat[5, 5] == heat[5, 5][0]))

    def test_exit_codes(self):
        save(self.d / "a" / "s.png", self.a)
        save(self.d / "b" / "s.png", self.a)
        save(self.d / "c" / "s.png", self.a[:-2])
        self.assertEqual(run_main(["compare", self.d / "a", self.d / "b", "-o", self.d / "o1", "--no-images"])[0], 0)
        self.assertEqual(run_main(["compare", self.d / "a", self.d / "c", "-o", self.d / "o2", "--no-images"])[0], 2)
        (self.d / "e").mkdir()
        self.assertEqual(run_main(["compare", self.d / "e", self.d / "e", "-o", self.d / "o3"])[0], 2)

    def test_single_files_and_meta_invalid(self):
        save(self.d / "x.png", self.a)
        save(self.d / "y.png", self.a)
        meta = {"id": "x", "camera": {"x": 0, "y": 0, "z": 0}}
        (self.d / "ma.json").write_text(json.dumps(meta), encoding="utf-8")
        (self.d / "mb.json").write_text(json.dumps(dict(meta, camera={"x": 5, "y": 0, "z": 0})), encoding="utf-8")
        self.assertEqual(run_main(["compare", self.d / "x.png", self.d / "y.png", "-o", self.d / "o"])[0], 0)
        code, out = run_main(["compare", self.d / "x.png", self.d / "y.png", "-o", self.d / "o",
                              "--meta-a", self.d / "ma.json", "--meta-b", self.d / "mb.json"])
        self.assertEqual(code, 2, out)
        self.assertIn("pose/state mismatch", out)

    def test_calibrate_then_compare(self):
        rng = np.random.default_rng(9)
        noisy = lambda: np.clip(self.a + rng.normal(0, 4, self.a.shape), 0, 255)  # noqa: E731
        save(self.d / "a" / "s.png", noisy())
        save(self.d / "a2" / "s.png", noisy())
        save(self.d / "b" / "s.png", noisy())
        code, out = run_main(["compare", self.d / "a", self.d / "b", "-o", self.d / "o", "--no-images"])
        self.assertNotEqual(code, 0, out)  # run-to-run noise fails the default thresholds
        tf = self.d / "thr.json"
        self.assertEqual(run_main(["calibrate", self.d / "a", self.d / "a2", "-o", tf])[0], 0)
        t = json.loads(tf.read_text(encoding="utf-8"))
        self.assertIn("baseline", t["spot"]["s"])
        self.assertEqual(run_main(["compare", self.d / "a", self.d / "b", "-o", self.d / "o", "--no-images",
                                   "--thresholds", tf])[0], 0)
        self.assertEqual(run_main(["compare", self.d / "a", self.d / "b", "-o", self.d / "o", "--no-images",
                                   "--baseline", self.d / "a2"])[0], 0)

    def test_selftest(self):
        self.assertEqual(run_main(["selftest"])[0], 0)


if __name__ == "__main__":
    unittest.main()
