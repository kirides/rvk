// FFT ocean: h(k, t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t} and the spectra of the displacement and its derivatives,
// packed as four complex fields whose inverse transforms are real (two real fields per complex FFT):
//   A.xy = Dx + i Dz        (Dx = -i kx / k h, Dz = -i kz / k h)
//   A.zw = Dy + i dDy/dx    (Dy = h, dDy/dx = i kx h)
//   B.xy = dDy/dz + i dDx/dx  (i kz h, kx^2 / k h)
//   B.zw = dDz/dz + i dDx/dz  (kz^2 / k h, kx kz / k h)
#include "ocean_compute.hlsli"

OCEAN_H0(h0Image);
OCEAN_WORK_A(workA);
OCEAN_WORK_B(workB);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    int n = int(pc.tile.w);
    if (p.x >= n || p.y >= n) return;
    float2 kv = oceanWaveVector(p);
    float k = length(kv);
    if (k < 1e-6) {
        workA[p] = float4(0.0, 0.0, 0.0, 0.0);
        workB[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float4 h0 = h0Image[p];
    float ph = oceanDispersion(k) * pc.time.x;
    float2 e = float2(cos(ph), sin(ph));
    float2 h = cmul(h0.xy, e) + cmul(h0.zw, cconj(e));
    float2 ih = ci(h);
    float2 dx = -ih * (kv.x / k);
    float2 dz = -ih * (kv.y / k);
    float2 dyx = ih * kv.x;
    float2 dyz = ih * kv.y;
    float2 dxx = h * (kv.x * kv.x / k);
    float2 dzz = h * (kv.y * kv.y / k);
    float2 dxz = h * (kv.x * kv.y / k);
    workA[p] = float4(dx + ci(dz), h + ci(dyx));
    workB[p] = float4(dyz + ci(dxx), dzz + ci(dxz));
}
