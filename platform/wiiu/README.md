# Super Mario Sunshine on Wii U

## Easiest: Wii U Pack Helper (Windows)

Download `WiiU-Pack-Helper.exe` from the release page and run it:

1. Choose **Super Mario Sunshine**, then choose your SD card's top folder.
2. Choose your own North American (GMSE01) disc image (`.iso`, `.gcm` or
   `.ciso`; convert RVZ to ISO in Dolphin first). Pick 16:9 or 4:3.
3. Optional: tick **HD textures** and choose `GMS_1080.zip` from the
   [Super Mario Sunshine UHD Texture Pack 2.1.1](https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack/releases/tag/2.1.1).
   The helper converts it into the Wii U pack (about 900 MB); `GMS.7z` is the
   full-size pack and is not used.
4. Click **Start** and wait for **Done**.

The helper writes the game data and the texture pack in the layout below.
Copy `sms.wuhb` to `sd:/wiiu/apps/` yourself, or put it beside the helper
and tick **Copy sms.wuhb**. Neither the game nor the texture pack is
included in the release; you supply both.

## SD card setup

To set the card up by hand instead: the `.wuhb` contains the program and
artwork only. Put it in:

```text
sd:/wiiu/apps/sms.wuhb
```

Keep the game data beside it in the app folder:

```text
sd:/wiiu/apps/sms/
├── sys/
│   ├── boot.bin
│   └── fst.bin
├── files/
│   └── ...                 # the extracted GameCube game files
└── textures/
    └── GMSE01.gx2pack     # optional Wii U texture pack
```

The `files/` tree must preserve the disc paths: a disc file such as
`/data/scene.szs` belongs at `files/data/scene.szs`. Do not put the game data
inside the `.wuhb`.

With Aroma, copy the `.wuhb` to `sd:/wiiu/apps/` and the data tree to
`sd:/wiiu/apps/sms/`, insert the SD card, and launch **SMS** from the Wii U
Menu.

## Controls

The controls below use the game's GameCube actions.

| Controller | Mapping |
| --- | --- |
| GamePad | Left stick: move; right stick: camera; A: jump; B: dive-attack; X/Y: X/Y actions; R: FLUDD full press; ZR: FLUDD half press; L: full L; ZL: half L/camera reset; either stick click: Z; Plus: pause. |
| Wii Remote (sideways, no extension) | D-pad on the left: move (Up: left, Right: up, Down: right, Left: down); 2: jump; 1: dive/talk; B: full R/FLUDD; A: X/nozzle switch; Minus: full L/camera reset; Plus: pause. |
| Wii Remote + Nunchuk | Nunchuk stick: move; A: jump; B trigger: full R/FLUDD; Minus: B/dive; C: L/camera reset; Z: Z; D-pad: camera and menu cursor; 1/2: X/Y; Plus: pause. |
| Pro Controller | Same layout as the GamePad. |
| Classic Controller | Same sticks and face-button layout; analog L/R are GameCube L/R; ZL/ZR are Z. |

A Wii Remote without an extension is used sideways; its D-pad supplies the
movement stick. Nunchuk and other controller layouts remain as listed above.

## Widescreen

The game runs in 16:9 widescreen by default. For the original 4:3 picture,
create an empty file named `4x3` in the app folder:

```text
sd:/wiiu/apps/sms/4x3
```
