// Enhanced shadows: the block the mask draw reads (push constant `draw` points at it instead of a DrawBlock: Device::prepareDraw
// takes DrawReplacement::block for a replaced draw). Included by shadow_mask.vert and shadow_mask.frag, so both see one layout.
// Mirror: enh::ShadowBlock (src/enh/shadows.h, static_assert'ed size; rvk_tests shadows_block_glsl_matches_cpp).

// Needs `#extension GL_EXT_buffer_reference_uvec2 : require` and rvk_abi.glsl included before it.

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowBlock {
    vec4 cascade[12];  // cascade c: rows 4c..4c+3 of DepthToShadowMatrix (texcoord = (dot(row0, p), .., dot(row3, p)))
    vec4 split;        // far end of each cascade in depth-map units (view depth / engine far)
    vec4 texel[3];     // per cascade: x map size, y 1 / size, z tap spacing (texels), w shadow depth per texel of a 45° slope
    uvec4 maps;        // bindless t2D of the cascades' depth maps, w cascades (1..3)
    uvec4 res;         // x depth map (t2D), y point clamp sampler, z variance map (t2D, 0 = none), w linear clamp sampler
    vec4 vsm[3];       // DepthToVarianceShadowMatrix rows
    vec4 vsmParams;    // x MinVariance, y MinLightBleeding, z 1 / (1 - MinLightBleeding)
    vec4 params;       // x blend band (fraction of a cascade's depth range), y fade band (of the last cascade), z constant
                       // bias (texels of a 45° slope), w steepest receiver slope the bias follows (tan)
    vec4 target;       // x 1 / width, y 1 / height, z width, w height
    uvec4 mode;        // x taps per axis (1..4), y debug view (0 off, 1 cascades, 2 cascade term only, 3 terrain term only,
                       // 4 the ray-traced term only, 5 the ray-traced coverage), zw 0
    uvec4 rt;          // x the ray-traced visibility (t2D, enh/rt_shadows; 0 = none), yzw 0
    vec4 rtParams;     // x range (depth-map units), y blend band (fraction of the range), zw 0
    vec4 depthRemap;   // read by the VERTEX shader (this block replaces the draw's DrawBlock: posFix is not there): x, y = the
                       // reversed-depth remap Device::depthRemap (z' = z + x + y at z = w = 1; 0 when the depth is not reversed)
};
