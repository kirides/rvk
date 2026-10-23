-- The spots of the D3D9-vs-Vulkan reference comparison (docs/research/reference-compare.md section 5). Data only; main.lua walks them in order.
--
--   id           file name stem: <tag>_<id>_b<k>.png
--   player       anchor { x, y, z, yaw } in engine units (y up); snapped to the nearest walkable point (risen.world.nav_point) unless snap = false
--   camera       fixed camera: { target = { dx, dy, dz } relative to the anchor (abs = true: absolute), yaw, pitch, distance, fov? }
--                yaw/pitch = the view direction (Entity:pose convention: yaw 0 looks along +z, 90 along +x; pitch < 0 looks down)
--   hide_player  the player is not rendered (no idle animation, no shadow); hide_npcs = radius: NPCs and monsters around the player are not rendered
--   time         { hour, minute }, held every tick until the freeze
--   weather      a state name or { State = weight, ... } (forced, mode 2, thunder off); interiors use the weights their weather zone gives (calibration)
--   thunder      true: the forced weather keeps its lightning flashes (default off)
--   freeze       false: the game time runs on through the capture (default: frozen)
--   explicit     true: only run when named in -Param spots=... (not part of "all": the reference comparison)
--   hud          true: HUD visible (default false); gui = { page = n } opens a GUI page, { key = vk, page = n } presses a key (ESC menu)
--   settle       rendered frames between placement and freeze (weather/wetness/sea need hundreds); default from -Param settle
--   notes        what the spot exercises
-- Anchors were verified by teleporting there (mode=calibrate, see main.lua) and looking at the pictures.
return {
    {
        id = "beach_rain",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 120, 0 }, yaw = 238, pitch = -6, distance = 700 },
        hide_player = true, hide_npcs = 2500,
        time = { 10, 0 }, weather = "Stormy_Rain",
        settle = 900,
        notes = "the new-game beach in rain: rain particles, wet ground, sea water, palms (SpeedTree), fog",
    },
    {
        id = "beach_sun",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 120, 0 }, yaw = 238, pitch = -6, distance = 700 },
        hide_player = true, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full",
        settle = 900,
        notes = "the same view in sunshine: sun shadows, water specular, sky dome, bloom",
    },
    {
        id = "sea_cliff",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { -44000, 0, -9500 }, abs = true, yaw = 238, pitch = -16, distance = 5500 },
        hide_player = true, hide_npcs = 2500,
        time = { 16, 0 }, weather = "Sunny_Full",
        settle = 900,
        notes = "the opening beach's sea from 15 m up (over the beach), towards the afternoon sun: the open sea to the horizon, the waves rolling in over the shallows, the surf",
    },
    {
        id = "sea_horizon",
        player = { x = -5588, y = 245, z = -22007, yaw = 190 },
        camera = { target = { -15715, 338, -33345 }, abs = true, yaw = 228, pitch = -3, distance = 5000 },
        hide_player = true, hide_npcs = 2500,
        time = { 11, 0 }, weather = "Sunny_Half_02",
        settle = 900,
        notes = "the open sea south-west of the harbour mole from 6 m above the water (camera at -12,000 / -30,000), towards the horizon: the large waves to the far plane",
    },
    {
        id = "harbour_mouth",
        player = { x = -5588, y = 245, z = -22007, yaw = 190 },
        camera = { target = { -7874, 39, -23932 }, abs = true, yaw = 266, pitch = -18, distance = 5000 },
        hide_player = true, hide_npcs = 2500,
        time = { 11, 0 }, weather = "Sunny_Half_02",
        settle = 900,
        notes = "from 16 m above the inner harbour basin (camera at -3,130 / -23,600) westwards out through its mouth between the town's quays and the mole: sheltered water in front, the open sea beyond (the camera's distance is capped near 6,000: poses are given by target + distance <= 5,500)",
    },
    {
        id = "jungle",
        player = { x = 8130, y = 5541, z = -1188, yaw = 0 },
        camera = { target = { 0, 200, 0 }, yaw = 0, pitch = -12, distance = 1200 },
        hide_player = true, hide_npcs = 3000,
        time = { 15, 0 }, weather = "Sunny_Half_02",
        notes = "dense vegetation: alpha-tested foliage, grass, SpeedTree, flowing water",
    },
    {
        id = "harbour_docks",
        player = { x = -5588, y = 245, z = -22007, yaw = 190 },
        camera = { target = { 0, 300, 0 }, yaw = 180, pitch = -8, distance = 900 },
        hide_player = true, hide_npcs = 2500,
        time = { 18, 30 }, weather = "Sunny_Half_02",
        notes = "the harbour at sunset: long shadows, low sun colours, ships, large water, distant LOD",
    },
    {
        id = "harbour_night",
        player = { x = -4361, y = 486, z = -19659, yaw = 0 },
        camera = { target = { 0, 200, 0 }, yaw = 90, pitch = -8, distance = 800 },
        hide_player = true, hide_npcs = 2500,
        time = { 23, 30 }, weather = "Sunny_Full",
        notes = "Harbour Town at night: torches (point lights, fire particles), moon and stars, dark tonemapping",
    },
    {
        id = "pub_interior",
        player = { x = -1100, y = 600, z = -19300, yaw = 0 },
        camera = { target = { 0, 150, 0 }, yaw = 90, pitch = -10, distance = 400 },
        hide_player = true, hide_npcs = 2000,
        time = { 20, 0 }, weather = { Sunny_Half_02 = 0.87, Sunny_Full = 0.13 }, -- the zone weights at the pub (calibration)
        notes = "the Harbour Town pub: candles, sconces, indoor ambient, normal maps",
    },
    {
        id = "monastery_hall",
        player = { x = 7100, y = 12549, z = 22040, yaw = 0 },
        camera = { target = { 0, 200, 0 }, yaw = 90, pitch = -5, distance = 600 },
        hide_player = true, hide_npcs = 2500,
        time = { 12, 0 }, weather = "Sunny_Full", -- the zone weight there (calibration)
        notes = "the monastery hall: candelabras, stone interior, windows",
    },
    {
        id = "lava_cave",
        player = { x = -3009, y = 10397, z = 60283, yaw = 0 },
        camera = { target = { 0, 180, 0 }, yaw = 150, pitch = -10, distance = 250 },
        hide_player = true, hide_npcs = 3000,
        time = { 12, 0 }, weather = "Dungeon_Blue6_Big", -- the zone state there (calibration)
        notes = "the volcano dungeon: emissive lava, smoke, red point lights, distortion",
    },
    {
        id = "crystal_cave",
        player = { x = -343, y = 5361, z = 11529, yaw = 0 },
        camera = { target = { 0, 200, 0 }, yaw = 270, pitch = -8, distance = 500 },
        hide_player = true, hide_npcs = 3000,
        time = { 12, 0 }, weather = { Sunny_Full = 0.67, Sunny_Half_02 = 0.33 }, -- the zone weights there (the cave has no zone of its own)
        notes = "a cave lit by glowing crystals: coloured point lights, wet rock, dungeon fog",
    },
    {
        id = "hero_closeup",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 160, 0 }, yaw = 238, pitch = -5, distance = 220 },
        hide_player = false, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full",
        hud = true,
        notes = "the hero close up with the HUD: skinned mesh, armour/skin/hair, self-shadowing, HUD bars and compass",
    },
    -- live idle animation (freeze = false: the clock and so the animations run through the capture): the per-object motion
    -- vectors of characters (docs/research/upscaling.md); the pictures are not reproducible, judge sharpness / ghosting only
    {
        id = "hero_idle",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 160, 0 }, yaw = 238, pitch = -5, distance = 260 },
        hide_player = false, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full", freeze = false, explicit = true,
        notes = "the hero in his idle animation, the world clock running",
    },
    {
        id = "pub_idle",
        player = { x = -1100, y = 600, z = -19300, yaw = 0 },
        camera = { target = { 0, 150, 0 }, yaw = 90, pitch = -10, distance = 400 },
        hide_player = true, hide_npcs = 2000,
        time = { 20, 0 }, weather = { Sunny_Half_02 = 0.87, Sunny_Full = 0.13 }, freeze = false, explicit = true,
        notes = "the Harbour Town pub with its NPCs (run with -Param show_actors=1) in their idle animations, the world clock running",
    },
    {
        id = "gui_inventory",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 160, 0 }, yaw = 238, pitch = -5, distance = 220 },
        hide_player = false, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full",
        gui = { page = 2 }, settle = 120,
        notes = "the inventory: panels, item icons (DXT), text",
    },
    {
        id = "gui_character",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 160, 0 }, yaw = 238, pitch = -5, distance = 220 },
        hide_player = false, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full",
        gui = { page = 3 }, settle = 120,
        notes = "the character screen (C): attribute and skill lists, numbers, text",
    },
    {
        id = "menu_esc",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 160, 0 }, yaw = 238, pitch = -5, distance = 220 },
        hide_player = false, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full",
        gui = { key = 0x1B, page = 13 }, settle = 120,
        notes = "the in-game menu (ESC): menu bar, tab images, title font",
    },
    {
        id = "swamp_camp",
        player = { x = -30621, y = 8649, z = 27292, yaw = 0 },
        camera = { target = { -30621, 8649, 27292 }, abs = true, yaw = 225, pitch = -24, distance = 2400 },
        hide_player = true, hide_npcs = 3000,
        time = { 14, 0 }, weather = "Sunny_Half_02",
        notes = "Don's bandit camp in the swamp (SP_Don), from the north-east over the walkways: green mud ground (Nat_Ground_GreenMud_*) under ground fog (Levelmesh_Lakemesh_07 is Special_Fog_Ground_01); no water surface is drawn there (water.md 2.2)",
    },
    {
        id = "swamp_pond",
        player = { x = -17305, y = 6700, z = 15200, yaw = 0 },
        camera = { target = { -17305, 6700, 17000 }, abs = true, yaw = 90, pitch = -18, distance = 1800 },
        hide_player = true, hide_npcs = 3000,
        time = { 14, 0 }, weather = "Sunny_Half_02",
        notes = "the swamp water (Levelmesh_Lakemesh_06, Special_Water_Swamp_01, the only swamp material): murky olive, reeds",
    },
    {
        id = "lake_jungle",
        player = { x = -6649, y = 5320, z = 8600, yaw = 0 },
        camera = { target = { -6649, 5300, 11000 }, abs = true, yaw = 0, pitch = -15, distance = 2200 },
        hide_player = true, hide_npcs = 3000,
        time = { 12, 0 }, weather = "Sunny_Full",
        notes = "an inland lake (Levelmesh_Lakemesh_05, Special_Water_Island_01): clear water, no waves, no shore",
    },
    {
        id = "waterfall_jungle",
        player = { x = 9299, y = 2700, z = 6800, yaw = 0 },
        camera = { target = { 9299, 3600, 9196 }, abs = true, yaw = 270, pitch = -8, distance = 2600 },
        hide_player = true, hide_npcs = 3000,
        time = { 12, 0 }, weather = "Sunny_Full",
        notes = "the waterfall Levelmesh_Waterfall_0001 between the lake Levelmesh_Lakemesh_04 and the river Levelmesh_Rivermesh_01",
    },
    {
        id = "beach_night_storm",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 120, 0 }, yaw = 227, pitch = -6, distance = 700 },
        hide_player = true, hide_npcs = 2500,
        time = { 23, 0 }, weather = "Stormy_Rain", thunder = true, freeze = false, explicit = true,
        settle = 900,
        notes = "the new-game beach at night in a thunderstorm, looking out to sea along a diagonal (the sky cube's face edge +x / -z behind the screen centre), the clock running: lightning flashes during the engine's sky cube refresh left half of the sea bright (ocean.md 9.8)",
    },
    -- the walk inland from the opening beach towards the starting area's cliff (yaw 58), for the sun shadows' range
    -- (docs/research/shadows.md): the cliff's shadow popped in a few metres ahead with the game's shadows
    {
        id = "shadow_inland_top",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 1500, 0 }, yaw = 58, pitch = -12, distance = 2500 },
        hide_player = true, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full", explicit = true,
        notes = "overview from 15 m above the beach inland: where the cliff is",
    },
    {
        id = "shadow_inland_high",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 3000, 0 }, yaw = 58, pitch = -22, distance = 3000 },
        hide_player = true, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full", explicit = true,
        notes = "30 m above the beach looking down inland: the ground from 40 to 150 m (cascade splits, the range's end)",
    },
    {
        id = "shadow_inland_far",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 2500, 0 }, yaw = 58, pitch = -9, distance = 1500 },
        hide_player = true, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full", explicit = true,
        notes = "25 m above the beach looking inland along the walk: shadows from 60 to 200 m",
    },
    {
        id = "shadow_inland_a",
        player = { x = -40255, y = 15, z = -6830, yaw = 58 },
        camera = { target = { 0, 170, 0 }, yaw = 58, pitch = -4, distance = 300 },
        hide_player = true, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full", explicit = true,
        notes = "eye height inland from the beach, -20 m along the walk to the cliff",
    },
    {
        id = "shadow_inland_b",
        player = { x = -38559, y = 15, z = -5770, yaw = 58 },
        camera = { target = { 0, 170, 0 }, yaw = 58, pitch = -4, distance = 300 },
        hide_player = true, hide_npcs = 2500,
        time = { 13, 0 }, weather = "Sunny_Full", explicit = true,
        notes = "eye height inland from the beach, +0 m along the walk to the cliff",
    },
}
