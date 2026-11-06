"""Unit tests of tools/material_graph_census.py on synthetic records."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import material_graph_census as mgc  # noqa: E402


def rec(diffuse, entry="PS_Default", stage="ps", **extra):
    macros = [["XFX_PS_DIFFUSE", diffuse], ["XFX_QUALITY", "2"]]
    macros += [[k, v] for k, v in extra.items()]
    return {"file": "ge_default_phong.fx", "entry": entry, "stage": stage, "macros": macros}


A = "(tex2D( g_SamplerData[0], XFX_PS_TEX0 )) * g_ShaderData[0]"
A2 = "(tex2D( g_SamplerData[2], XFX_PS_TEX0 )) * g_ShaderData[5]"


class Census(unittest.TestCase):
    def test_normalise(self):
        self.assertEqual(mgc.normalise("g_ShaderData[3].xy + g_vShaderData[ 12 ] * g_SamplerData[2]"),
                         "g_ShaderData[#].xy + g_vShaderData[#] * g_SamplerData[#]")
        self.assertIn("1.0f", mgc.normalise("x * 1.0f"))

    def test_index_only_difference_is_one_graph(self):
        a = mgc.split_request(rec(A))
        b = mgc.split_request(rec(A2))
        self.assertEqual(a["graph"], b["graph"])
        self.assertNotEqual(a["raw"], b["raw"])

    def test_alpha_keeps_aliasing(self):
        same = mgc.split_request(rec("g_ShaderData[1] * g_ShaderData[1]"))
        diff = mgc.split_request(rec("g_ShaderData[1] * g_ShaderData[2]"))
        self.assertEqual(same["graph"], diff["graph"])
        self.assertNotEqual(same["alpha"], diff["alpha"])

    def test_constants(self):
        a = mgc.split_request(rec("x * 1.0f"))
        b = mgc.split_request(rec("x * 0.5f"))
        self.assertNotEqual(a["graph"], b["graph"])
        self.assertEqual(a["noconst"], b["noconst"])

    def test_dynamic_split(self):
        r = mgc.split_request(rec(A, XFX_DIRLIGHT="true", XFX_PNTLIGHTCOUNT="2", XFX_SKINNED="true"))
        self.assertEqual(r["dynamic"], {"XFX_DIRLIGHT": "true", "XFX_PNTLIGHTCOUNT": "2"})
        self.assertEqual(dict(r["static"]), {"XFX_QUALITY": "2", "XFX_SKINNED": "true"})

    def test_vs_stage_ignores_ps_graph(self):
        r = mgc.split_request(rec(A, entry="VS_Default", stage="vs", XFX_VS_TEX0="XFX_VS_INPUT_TEX0"))
        self.assertNotIn("XFX_PS_DIFFUSE", r["graph"])
        self.assertIn("XFX_VS_TEX0", r["graph"])

    def test_analyse_counts(self):
        reqs = [mgc.split_request(x) for x in [
            rec(A, XFX_DIRLIGHT="true"),
            rec(A2, XFX_DIRLIGHT="false", XFX_PNTLIGHTCOUNT="1"),   # same graph, other light macros: one pipeline
            rec(A, XFX_SKINNED="true"),                              # same graph, other static macro
            rec("g_ShaderData[0]"),
        ]]
        a = mgc.analyse(reqs, 5)
        self.assertEqual(a["requests"], 4)
        self.assertEqual(a["distinct_graph"], 2)
        self.assertEqual(a["pipelines"], 3)
        self.assertEqual(a["coverage_graphs"]["50%"], 1)

    def test_coverage(self):
        from collections import Counter
        c = Counter({"a": 90, "b": 9, "c": 1})
        self.assertEqual(mgc.coverage(c), {"50%": 1, "90%": 1, "99%": 2})

    def test_cli_union_and_json(self):
        with tempfile.TemporaryDirectory() as d:
            p1, p2, out = Path(d, "a.jsonl"), Path(d, "b.jsonl"), Path(d, "o.json")
            p1.write_text(json.dumps(rec(A)) + "\n")
            p2.write_text(json.dumps(rec(A2)) + "\n" + json.dumps(rec("g_ShaderData[0]")) + "\n")
            self.assertEqual(mgc.main([str(p1), str(p2), "--json", str(out), "--top", "3"]), 0)
            res = json.loads(out.read_text())
            self.assertEqual(res["union"]["requests"], 3)
            self.assertEqual(res["union"]["distinct_graph"], 2)


if __name__ == "__main__":
    unittest.main()
