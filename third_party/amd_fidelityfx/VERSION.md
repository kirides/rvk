# Vendored AMD FSR SDK (FidelityFX) 2.4.0

Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.4.0` ("AMD FSR SDK 2.4.0", 2026-10-28),
commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`. The release has no assets; the files are the tag's own
(`git clone --filter=blob:none --depth 1 --branch v2.4.0`, sparse checkout, blob ids checked by git).

| here | SDK path |
|---|---|
| `api/include/**` | `Kits/FidelityFX/api/include/**` (MIT) |
| `upscalers/include/ffx_upscale.h[pp]` | `Kits/FidelityFX/upscalers/include/` (MIT) |
| `framegeneration/include/ffx_framegeneration.h`, `ffx_framegeneration_api_types.h`, `vk/ffx_api_framegeneration_vk.h`, `dx12/ffx_api_framegeneration_dx12.h[pp]` | `Kits/FidelityFX/framegeneration/include/` (MIT) |
| `signedbin/amd_fidelityfx_loader_vk.dll` | `Kits/FidelityFX/signedbin/` |
| `signedbin/amd_fidelityfx_upscaler_vk.dll` | `Kits/FidelityFX/signedbin/` |
| `signedbin/amd_fidelityfx_framegeneration_vk.dll` | `Kits/FidelityFX/signedbin/` (FSR 3.1.6 frame generation, FSR Frame Generation 4.0.2 (ML), the frame generation swapchain 3.1.7) |
| `LICENSE.md` | `Kits/FidelityFX/docs/license.md` (the binaries: redistribution in binary form only, no reverse engineering,
  notice reproduced: `scripts/package.ps1` puts the notice into the zip as `rvk\amd_fidelityfx_LICENSE.txt`) |

The DX12 DLLs (WP7 of docs/research/dx12-game-port.md: FSR + frame generation on the D3D12 backend) are the same tag's `signedbin/amd_fidelityfx_{loader,upscaler,framegeneration}_dx12.dll`
(`api/include/dx12/ffx_api_dx12.h[pp]` was already vendored). Not vendored: the denoiser (Ray Regeneration) and radiance cache DLLs (DX12 only, no use here).

## Signatures (checked before anything was loaded: `Get-AuthenticodeSignature`)

All Status `Valid`, signer `CN=Advanced Micro Devices, O=Advanced Micro Devices, S=California, C=US`, certificate not after
2027-04-27, DigiCert SHA256 RSA4096 timestamp.

| file | size | version | SHA-256 |
|---|---|---|---|
| `amd_fidelityfx_loader_vk.dll` | 39,184 | 1.0.2.4555 | `38A56A01FB3565C0757D9F5C1E4E69DE4B65E5F2E5769B323B200DC1F142BAB9` |
| `amd_fidelityfx_upscaler_vk.dll` | 36,135,696 | 4.1.2.4555 | `4A7E895AF6E0B615D40A84D59EB8C2B1054AD91CDD0C739567940270D51BDAB7` |
| `amd_fidelityfx_framegeneration_vk.dll` | 36,806,928 | 4.0.2.4555 | `D002DABFF8C5E71EAA715FC99438CD4DC81230262A7897B16BC402EFEC3498B4` |
| `amd_fidelityfx_loader_dx12.dll` | 39,184 | 2.3.0.4555 | `E2191D3C0828BE45EB8AFFD4E3EC0378F2293ADC345B772A7170EDCF3B021ECB` |
| `amd_fidelityfx_upscaler_dx12.dll` | 57,664,272 | 4.1.2.4555 | `AAD4FCCCCB29084A655F9A4F9F613BD22C8C03FB4A0F8840148CC6543B4CCACE` |
| `amd_fidelityfx_framegeneration_dx12.dll` | 29,679,376 | 4.0.2.4555 | `E6843252F58B0341A0D80F6A20BC0D840643EE95BC48C22CAC08745344577BB6` |

The three `_dx12` files: fetched 2026-11-03 from the tag's commit `7539ae8b` (same clone method), Authenticode Valid, signer as above, checked before anything
was loaded; their `signedbin` siblings `amd_fidelityfx_denoiser_dx12.dll` / `amd_fidelityfx_radiancecache_dx12.dll` / the `.lib` files were not taken.
Replaced: SDK 1.1.4's single `amd_fidelityfx_vk.dll` (9.3 MB, 1.0.1.41314; FSR 3.1.4 only) and the headers `ffx-api/include/ffx_api/*`
(+ the provider source `ffx-api/src`, which was reference material for section 13 of docs/research/upscaling.md; it is in the git
history before the commit that introduced this file).

The application loads `amd_fidelityfx_loader_vk.dll`; the loader finds `amd_fidelityfx_upscaler_vk.dll` and
`amd_fidelityfx_framegeneration_vk.dll` in the executable's folder or next to itself (new in 2.4.0). The D3D12 backend loads `amd_fidelityfx_loader_dx12.dll` the same way (`rhi::fidelityFxLoader`); the three `_vk` and three `_dx12` files all go to `bin\rvk`.
The frame generation DLL was re-checked (Authenticode Valid, signer as above, SHA-256 as in the table) on 2026-10-29 when it was
vendored; `scripts/package.ps1` puts it, with the other FFX DLLs and the licence notice, into the main zip.

To update: clone the new tag into an empty directory, verify the signatures, replace the files, update this file.
