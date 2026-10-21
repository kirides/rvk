-- The spots of the D3D9-vs-Vulkan reference comparison (docs/research/reference-compare.md section 5). Data only; main.lua walks them in order.
--
--   id           file name stem: <tag>_<id>_b<k>.png
--   player       anchor { x, y, z, yaw } in engine units (y up); snapped to the nearest walkable point (risen.world.nav_point) unless snap = false
--   camera       fixed camera: { target = { dx, dy, dz } relative to the anchor (abs = true: absolute), yaw, pitch, distance, fov? }
--                yaw/pitch = the view direction (Entity:pose convention: yaw 0 looks along +z, 90 along +x; pitch < 0 looks down)
--   hide_player  the player is not rendered (no idle animation, no shadow); hide_npcs = radius: NPCs and monsters around the player are not rendered
--   time         { hour, minute }, held every tick until the freeze
--   weather      a state name or { State = weight, ... } (forced, mode 2, thunder off); interiors use the weights their weather zone gives (calibration)
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
}
