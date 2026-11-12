// An empty pixel shader (no inputs, no outputs): the D3D12 diagnostic RVK_D3D12_NULL_PS gives the pixel-shader-less pipelines of the depth passes this one
// (frame-ownership.md 14.39: does the AMD D3D12 driver's path for a pipeline without a pixel shader matter?).
void PSMain() {}
