// Octahedral unit-vector mapping with y as the pole axis (the G-buffer normal, the GI probes' depth / irradiance tiles).
#pragma once

float2 rvkGiOctEncode(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    float2 o = n.xz;
    if (n.y < 0.0) o = (1.0 - abs(o.yx)) * float2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    return o * 0.5 + 0.5;
}

float3 rvkGiOctDecode(float2 uv) {
    float2 o = uv * 2.0 - 1.0;
    float3 n = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
