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

// The G-buffer's material channel (material pack, docs/research/material-pack.md): the B channel of the G-buffer normal target (10 bit unorm; it was
// always 0) holds roughness (5 bit) and metalness (4 bit) of a draw with a pack entry; 0 = no entry (consumers use their own defaults).
float rvkGbufferMaterialEncode(float rough, float metal) {
    uint code = 512u | (uint(saturate(metal) * 15.0 + 0.5) << 5) | uint(saturate(rough) * 31.0 + 0.5);
    return float(code) * (1.0 / 1023.0);
}
// -> (roughness, metalness, has an entry); b = the normal target's .b
float3 rvkGbufferMaterialDecode(float b) {
    uint code = uint(b * 1023.0 + 0.5);
    return code >= 512u ? float3(float(code & 31u) * (1.0 / 31.0), float((code >> 5) & 15u) * (1.0 / 15.0), 1.0) : float3(1.0, 0.0, 0.0);
}
