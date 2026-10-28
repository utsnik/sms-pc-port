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
| Wii Remote + Nunchuk | Nunchuk stick: move; A: jump; B trigger: full R/FLUDD; Minus: B/dive; C: L/camera reset; Z: Z; D-pad: camera and menu cursor; 1/2: X/Y; Plus: pause. |
| Pro Controller | Same layout as the GamePad. |
| Classic Controller | Same sticks and face-button layout; analog L/R are GameCube L/R; ZL/ZR are Z. |

A Wii Remote without a Nunchuk has no movement-stick mapping; its D-pad, A,
and Plus operate menus.

## Widescreen

Widescreen is opt-in. Create an empty file named `widescreen` in the app
folder:

```text
sd:/wiiu/apps/sms/widescreen
```
