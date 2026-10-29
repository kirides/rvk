// Include of test/dxc_probe.comp.hlsl
#pragma once

float rvkProbeValue(uint x, uint y) { return float(x + y * 8u) * 0.5; }
