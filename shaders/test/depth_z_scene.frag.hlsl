// RHI backend test (S-6): PSMain is the engine's z pass pixel shader (view depth / far to an R32F colour target), PSDepthOnly the colour-less form.

static const float kFar = 1000.0;

struct VsOut {
    float4 position : SV_Position;
    float viewZ : TEXCOORD0;
};

float PSMain(VsOut i) : SV_Target0 { return i.viewZ / kFar; }

void PSDepthOnly(VsOut i) {}
