# Super Mario Sunshine on Wii U

## SD card setup

The `.wuhb` contains the program and artwork only. Put it in:

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
