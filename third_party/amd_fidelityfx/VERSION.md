# Vendored AMD FSR SDK (FidelityFX) 2.4.0

Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.4.0` ("AMD FSR SDK 2.4.0", 2026-10-28),
commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`. The release has no assets; the files are the tag's own
(`git clone --filter=blob:none --depth 1 --branch v2.4.0`, sparse checkout, blob ids checked by git).

| here | SDK path |
|---|---|
| `api/include/**` | `Kits/FidelityFX/api/include/**` (MIT) |
| `upscalers/include/ffx_upscale.h[pp]` | `Kits/FidelityFX/upscalers/include/` (MIT) |
| `signedbin/amd_fidelityfx_loader_vk.dll` | `Kits/FidelityFX/signedbin/` |
| `signedbin/amd_fidelityfx_upscaler_vk.dll` | `Kits/FidelityFX/signedbin/` |
| `LICENSE.md` | `Kits/FidelityFX/docs/license.md` (the binaries: redistribution in binary form only, no reverse engineering,
  notice reproduced: `scripts/package.ps1` puts the notice into the zip as `rvk\amd_fidelityfx_LICENSE.txt`) |

Not vendored (Vulkan DLLs that exist but are not used): `amd_fidelityfx_framegeneration_vk.dll` (36.8 MB, FSR Frame Generation 4.0.2
+ Swapchain, FSR 3.1.6 frame generation). There is no Vulkan denoiser (Ray Regeneration) or radiance cache DLL (DX12 only).

## Signatures (checked before anything was loaded: `Get-AuthenticodeSignature`)

All Status `Valid`, signer `CN=Advanced Micro Devices, O=Advanced Micro Devices, S=California, C=US`, certificate not after
2027-04-27, DigiCert SHA256 RSA4096 timestamp.

| file | size | version | SHA-256 |
|---|---|---|---|
| `amd_fidelityfx_loader_vk.dll` | 39,184 | 1.0.2.4555 | `38A56A01FB3565C0757D9F5C1E4E69DE4B65E5F2E5769B323B200DC1F142BAB9` |
| `amd_fidelityfx_upscaler_vk.dll` | 36,135,696 | 4.1.2.4555 | `4A7E895AF6E0B615D40A84D59EB8C2B1054AD91CDD0C739567940270D51BDAB7` |
| (not vendored) `amd_fidelityfx_framegeneration_vk.dll` | 36,806,928 | 4.0.2.4555 | `D002DABFF8C5E71EAA715FC99438CD4DC81230262A7897B16BC402EFEC3498B4` |

Replaced: SDK 1.1.4's single `amd_fidelityfx_vk.dll` (9.3 MB, 1.0.1.41314; FSR 3.1.4 only) and the headers `ffx-api/include/ffx_api/*`
(+ the provider source `ffx-api/src`, which was reference material for section 13 of docs/research/upscaling.md; it is in the git
history before the commit that introduced this file).

The application loads `amd_fidelityfx_loader_vk.dll`; the loader finds `amd_fidelityfx_upscaler_vk.dll` in the executable's folder
or next to itself (new in 2.4.0), so both go to `bin\rvk`.

To update: clone the new tag into an empty directory, verify the signatures, replace the files, update this file.
