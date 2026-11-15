# PresetToolKit — Fallout 4 ⇄ Cyberpunk 2077 faces

PresetToolKit moves a character's face between Fallout 4 and Cyberpunk 2077, and can export the
result as a 3D model.

| Tab / direction | Input | Output |
|---|---|---|
| Faces: Fallout 4 → Cyberpunk 2077 | LooksMenu preset (`.json`) or Fallout 4 save (`.fos`) | V head replacement `.archive` (+ mod manager `.zip`) |
| Faces: Cyberpunk 2077 → Fallout 4 | Cyberpunk 2077 save (`sav.dat`) | LooksMenu preset `.json` (+ optionally a copy of a Fallout 4 save with the face) |
| Export model | either of the above | `.glb` / `.obj` of the face on V's or Fallout 4's head |

## Quick start (Windows)

1. Build (see [Build](#build)) or use a release; keep the `data` folder next to the programs.
2. Run **`PresetToolKit.exe`**.
3. Set the **Fallout 4 folder**, the **Cyberpunk 2077 folder** and **WolvenKit.CLI.exe**. Nothing
   has to be extracted by hand: Fallout 4's files are read from its loose files and `.ba2`
   archives, and V's head is pulled from Cyberpunk's archives with WolvenKit (once per gender,
   then cached).
4. Pick the direction and the input, choose the destination and press **Convert**.

- Cyberpunk mods go to `Cyberpunk 2077\archive\pc\mod`.
- Presets go to `Fallout 4\Data\F4SE\Plugins\F4EE\Presets`.

The log panel shows progress and a report. Settings are remembered in `PresetToolKit.ini`. Working
files go to `%LOCALAPPDATA%\PresetToolKit\work` (*Open work folder*).

`PresetToolKitCLI.exe` is the command-line version (`--help` lists every option);
`run_PresetToolKit.bat` is an editable launcher for it.

## Fallout 4 → Cyberpunk 2077

**Input.** The preset's (or save's) face sculpts and face presets / sliders are read. Fallout 4
saves are read directly: no LooksMenu export is needed, and
`PresetToolKitCLI --export-preset <out.json> --preset <save.fos>` writes a save's face as a preset.

**How it works.** Fallout 4's own head is sculpted the way the game does it: its face bones and
skin weights, with the exact ranges from the game's sculpt region file. The face bones are
calibrated against V's head at run time, and the movement of the face is carried over to V's head.
Without the Fallout 4 folder an approximate method (bones placed on V's head, from built-in data)
is used.

**Output.** Only V's head `.morphtarget` is replaced; the new vertex positions are written straight
into the game file. Skinning, the face rig and everything else stay exactly as in the game. The
result is a plain `.archive` that overrides the vanilla head: no ArchiveXL, and V simply has the
new face. Writing the file uses the game's Oodle library, so the Cyberpunk 2077 folder must be set.

**Character creator shapes** (on by default; `--keep-cc-shapes` keeps them):
- Every V has one eyes / nose / mouth / jaw / ear shape applied on top of the head, moving the face
  by 4-13 mm. These are switched off, so the face in game is exactly the converted one.
- The same applies to the other face parts (eyeballs, brows, lashes, teeth, makeup decals). They
  also follow the sculpt, so eyes stay in their sockets and decals stay on the skin
  (`--no-face-parts` leaves them alone).

Replacing the vanilla head conflicts with other mods that replace the same files. Only the chosen
gender's head is changed.

## Cyberpunk 2077 → Fallout 4

**What is read.** From the save: V's body gender, the character creator face choices (eyes, nose,
mouth, jaw, ears) and the look. V's face is rebuilt from the game files.

**The fit.** Fallout 4's sculpt sliders (position, rotation and scale of every region) and face
presets / sliders are fitted on Fallout 4's own head, so that its face lands on V's. The report
shows the mean distance between the two faces before and after. Without the Fallout 4 folder an
approximate fit is used.

**Approximate customization** (on by default; `--no-customization` writes the face shape only)
carries V's look over to the nearest vanilla Fallout 4 options:

| Cyberpunk 2077 | Fallout 4 |
|---|---|
| hair colour | nearest of the 32 hair colours |
| eye colour | eye head part |
| skin tone | skin tone tint, by lightness |
| eyebrow style | brow tint of the same number |
| freckles, pimples / blemishes | freckles, cheek blemishes |
| lip / eye / cheek makeup | lipstick, eyeshadow / eyeliner, blush (nearest palette colour) |

The table is `data/fo4_chargen.json`, generated from Fallout4.esm by `tools/make_fo4_chargen.py`.

**Hairstyle and body weight** are not in a Cyberpunk save. To keep yours, use `--base-preset`
(command line) to take head parts, other tints and body from an existing preset, or the body
weight from a Fallout 4 save.

**Face into a Fallout 4 save** (GUI: *Also put face in FO4 save*; `--fo4-save`, `--fo4-save-out`):
- A **copy** of the chosen save (plus its `.f4se` co-save) gets the face, eyes, hair colour and tints.
- Its hairstyle, body weight and other tints are kept, and the original save is not touched.
- Its character must be the same sex as V and must have had its face edited at least once (a save
  in the pre-war mirror works too).

## Extended Facial Sculpting (Fallout 4 mods)

Two checkboxes (only one at a time) add the extra sculpt sliders of these Fallout 4 mods. Their
region files are bundled in `data`:

- **Extended Facial Sculpting** (`--efs`, `data/efs`)
- **Extended Facial Sculpting REDUX** (`--efs-redux`, `data/efs_redux`): the "Normal Sculpts" files,
  which also work with its "Single-Side" install option

Cyberpunk 2077 → Fallout 4: the face is fitted with the extra sliders too, and the preset then needs
the mod. Mean distance from V's face on test saves:

| Sculpt regions | Female V | Male V |
|---|---|---|
| Vanilla | 0.73 mm | 0.81 mm |
| Extended Facial Sculpting | 0.58 mm | 0.56 mm |
| Extended Facial Sculpting REDUX | 0.52 mm | 0.51 mm |

Fallout 4 → Cyberpunk 2077: presets and saves that use either mod's sliders are detected
automatically. REDUX's male sliders use their own IDs.

## Export model

The **Export model** tab (`--export-model <file | folder>`, `--model-head cp|fo4`) saves the
generated face as a 3D model instead of a mod or preset.

- **Face from:** a Fallout 4 preset / save, or a Cyberpunk 2077 save.
- **Head mesh:**
  - **Cyberpunk 2077:** V's head with the face exactly as in the mod or preset.
  - **Fallout 4:** Fallout 4's own head sculpted as the game does it. This needs the Fallout 4 folder.
- **Format:** `.glb` or `.obj`, in metres with Y up, with UVs and smooth normals. Both heads are placed
  where V's head is, so exports from the two games line up.

V's head is needed for both head meshes, because the faces are lined up with it.

## Command line

```bash
# Fallout 4 -> Cyberpunk 2077
PresetToolKitCLI --preset <preset.json | save.fos> --fo4-dir <Fallout 4> --cp-game <Cyberpunk 2077> \
    --wolvenkit <WolvenKit.CLI.exe> --archive-dest <Cyberpunk 2077>/archive/pc/mod

# Cyberpunk 2077 -> Fallout 4
PresetToolKitCLI --cp-save <sav.dat> --fo4-dir <Fallout 4> --cp-game <Cyberpunk 2077> \
    --wolvenkit <WolvenKit.CLI.exe> --preset-out <Fallout 4>/Data/F4SE/Plugins/F4EE/Presets

# Export model
PresetToolKitCLI --preset <preset.json> --fo4-dir <Fallout 4> --cp-game <Cyberpunk 2077> \
    --wolvenkit <WolvenKit.CLI.exe> --export-model <face.glb> --model-head fo4
```

Every run writes `report.txt` and `log.txt` to the work folder (`--out`). Without WolvenKit.CLI a
Fallout 4 → Cyberpunk run still sculpts the head and writes a WolvenKit project with
`MANUAL_STEPS.txt`.

**V's head from a WolvenKit project** (instead of the Cyberpunk folder):
- Export V's head `.morphtarget` to GLB with WolvenKit 8.9.1 or newer, and pass it with `--head`.
- Pass the vanilla `.morphtarget` with `--template-morphtarget`.
- In the GUI, picking the GLB from a project's `source\raw` folder fills in the rest.

## Build

C++17 and CMake 3.16 or newer; no external dependencies (nlohmann/json and miniz are included).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Windows, open the folder in Visual Studio as a CMake project, or run the commands above in a
Developer PowerShell. The GUI is built on Windows only. The `data` folder is copied next to the
programs.

## Configuration

`data/default_mapping.json` holds the settings that need no rebuild:

- the head files to replace;
- the WolvenKit.CLI command lines;
- the built-in face bone calibration;
- the rules of the approximate method (regions, gains, ID and slider rules).

The bundled `HumanRaceFacialBoneRegions*.txt` files are used when no Fallout 4 folder is set;
`--fo4-regions` overrides them.

`tools/calibrate.py` recreates the built-in calibration from a Fallout 4 face-bones head and V's
head GLB (for modded heads).

## Testing without the games

`tests/` has generators for a synthetic head GLB, a `.ba2` archive and a Cyberpunk save, a sample
preset and a stand-in for WolvenKit.CLI. A quick run that sculpts the synthetic head and writes a
WolvenKit project:

```bash
python3 tests/make_test_head.py /tmp/head.glb
./build/PresetToolKitCLI --preset tests/sample_preset.json --head /tmp/head.glb --out /tmp/out
```

## Source layout

```
src/gui_win32.cpp       Windows GUI (PresetToolKit.exe)
src/main.cpp            command line (PresetToolKitCLI.exe)
src/Converter.*         both directions and the model export, shared by the GUI and CLI
src/Fo4FaceFit.*        Fallout 4's face sculpting on its own head; the fit to V's face
src/Calibrator.*        Fallout 4 face bones -> V's head calibration
src/HeadDeformer.*      approximate sculpting on V's head, head frame detection
src/PresetFit.*         approximate fit, V's character creator shapes
src/GameSaves.*         Fallout 4 and Cyberpunk 2077 saves (read; Fallout 4 face write)
src/Fo4Look.*           Cyberpunk look -> Fallout 4 tints, eyes, hair colour
src/LooksMenuPreset.*   LooksMenu preset read / write
src/Fo4Data.*           Fallout 4 loose files + .ba2 archives
src/Nif.*, src/Tri.*    Fallout 4 .nif and .tri readers
src/Cr2w.*              Cyberpunk CR2W files: Oodle, .morphtarget patching
src/ArchivePackager.*   WolvenKit project, WolvenKit.CLI import / pack, zip
src/ModelExport.*       .glb / .obj writer
src/Gltf.*, src/Mapping.*, src/Zip.*, src/Util.*   support code
data/                   mapping, region files (vanilla, EFS, EFS REDUX), fo4_chargen.json
tools/                  calibrate.py, make_fo4_chargen.py, nif.py
tests/                  test data generators and a WolvenKit.CLI stand-in
third_party/            nlohmann/json, miniz (MIT)
```
