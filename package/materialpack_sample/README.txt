Material pack: extra roughness / metalness data for Risen's materials
=====================================================================

A material pack gives chosen materials a physically based look on top of the game's own lighting: sharper or softer
highlights from the sun, candles and torches, a reflection of the sky, metal that looks like metal, glossy wood and stone.
Materials that are not in a pack look exactly as before.

This folder is a small SAMPLE pack (wood planks, stone bricks, cobblestones, rusty metal, weapons, glass and metal items,
plus families of materials by name). It is NOT active by default.

Install
-------
1. Copy this folder to  <Risen>\bin\rvk\materialpack\  (so that <Risen>\bin\rvk\materialpack\pack.toml exists).
   More packs: put each in its own sub-folder of materialpack\, later folder names override earlier ones.
2. Start the game. The setting "Material pack" (overlay F11, Graphics tab, Materials) must be on (default).
   It takes effect at the next start. "Material pack quality" off / low / medium / high is changeable at any time.
3. To remove the pack, delete the folder (the game then looks as without the pack).

Format (for authors)
--------------------
Files are named after the game's own names, case does not matter:

  <Name>_orm.png      RGBA map: R = occlusion, G = roughness, B = metalness, A = emissive mask
  <Name>_r.png        roughness only (grey image)
  (DDS files work as well: any format the game's textures use, with their own mip maps; PNG: 8 bit, mips are made on load)
  pack.toml           optional constants and wildcards, see the sample

<Name> is the NAME OF THE MATERIAL (e.g. ItMisc_01_Diffuse_01_Glass) or of its DIFFUSE TEXTURE (e.g.
Arch_Wood_Planks_01_Diffuse_01). Maps cover the whole texture (any resolution, a quarter of the diffuse is fine) and use the same
texture coordinates as the diffuse. Roughness 0 = mirror, 1 = rough. Metalness 0 = stone / wood / cloth, 1 = metal.

pack.toml:
  [material."Arch_Wood_*"]       # exact name or wildcard (* ?), later rules override earlier ones
  roughness = 0.42               # constant roughness (for materials WITHOUT a map)
  metalness = 0.0                # with an ORM map: multiplies its metalness channel
  roughness_scale = 1.0          # map roughness = map * scale + bias
  roughness_bias = 0.0
  emissive = 1.0                 # > 0: the ORM alpha channel marks glowing parts
  map = "my_file.png"            # a map file of another name
  wetness = 0.5                  # 0..1, how wet the surface looks in rain and after it (see below)
  ior = 1.5  transmission = 1.0  # glass: see the sample

Finding names: start the game once with the setting "Log level" debug (config.toml [log] level = "debug"); rvk.log then lists
every material once: "enh: materials: seen '<material>' (diffuse image '<image>'): pack entry | no entry".
Tool for first maps from a texture: tools/matpack_make.py in the source repository.
Hot reload while the game runs (developers): the console command "materials reload" (-Commands).

Debug views (overlay, Debug tab, needs "Debug mode"): "Material pack debug view": roughness, metalness, packed map, coverage
(green = a pack entry matched this draw).

The maps in this sample were generated from the game's own textures by a script (darker = rougher): a starting point, not art.
