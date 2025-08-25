# Texture Dumping & Replacement (DS 3D)

This document explains how to **dump** in‑game Nintendo DS 3D textures to PNG and how to **replace** them with your own images using the texture tools integrated in this build.

> there are two folders `text_replace/dump` and `text_replace/mod`, related to the process. Run the game with dump enabled, copy the PNG you want to change from `dump` to `mod`, edit it in any editor, keep the **filename** intact, and relaunch.

## Supported Renderers
- Software
- OpenGL (Classic)

## Folders

```
text_replace/
├─ dump/   # auto‑generated PNGs from the game
└─ mod/    # your edited PNGs, same file names as in dump/
```

> The emulator creates dump folder automatically.


## Enabling / disabling (how the feature is turned on)

There are checkboxes in main menu's `Config->Texture replace`.

If you are integrating this in your own fork or building from source, these are the functions that toggle the behavior:

```cpp
// Write PNG dumps into text_replace/dump/
TexReplace_SetDump(true);      // or false

// Use replacements from text_replace/mod/
TexReplace_SetReplace(true);   // or false
```

You can enable either feature independently.


## What gets dumped (and file names)

Whenever **dumping** is enabled, each **unique** DS 3D texture seen during rendering is decoded to RGBA and saved as:

```
text_replace/dump/<HASH16>_fmt<FMT>_<W>x<H>.png
```

- `HASH16` – 16 hex digits (uppercase). The hash is computed from the decoded RGBA pixels.
- `FMT`    – DS texture format id (decimal): 1=A3I5, 2=I2, 3=I4, 4=I8, 5=Compressed (4x4), 6=A5I3, 7=Direct (RGB5551).
- `W`/`H`  – **original DS texture** width/height (multiples of 8).

> Each unique **content+format+size** is dumped once per run to avoid duplicates.


### Example

```
text_replace/dump/8A3F09C3C4E11D72_fmt4_64x64.png
```

Means: DS format **I8 (256‑color)**, original size **64×64**.


## How to make a replacement

1. Make sure the texture you want exists in `text_replace/dump/` (enable dumping and visit the scene once).
2. **Copy** the file(s) you want to edit from `dump/` to `text_replace/mod/` **without changing the name**.
3. Open the copied PNG in any editor and modify it.
   - You may **upscale** it to any size (e.g., 64×64 → 256×256). Higher resolution is supported.
   - Keep straight (non‑premultiplied) alpha.
4. Save the edited PNG to `text_replace/mod/` with the **same filename** (the `<HASH16>_fmt<FMT>_<W>x<H>.png` part must stay identical).
5. Run the game with **replacements** enabled. The emulator will load your PNG instead of the original texture.

> The loader looks for `text_replace/mod/<HASH16>_fmt<FMT>_<W>x<H>.png`. The **actual PNG dimensions may differ** from `<W>x<H>`; the shader remaps coordinates correctly. The suffix in the name must still match the original dump.


## Image format & filtering

- Input/Output: **PNG, RGBA8**. Any color space is fine; typical sRGB PNGs work.
- Alpha: **straight** alpha (not premultiplied).
- The renderer respects the DS **wrap/mirror/clamp** modes when sampling your replacement.
- Upscaling: any resolution is accepted. For best results use integer multiples of the original size (e.g., 2×, 4×).


## Tips

- If a game streams or modifies textures at runtime, you may get **several dumps** with different hashes. Replace the exact one(s) used in the scene you care about.
- Transparent edges: export with an alpha channel; feather the edges slightly to avoid hard outlines when the DS does decal/modulate blending.
- Want to revert? Just remove or rename the file from `text_replace/mod/`.


## Troubleshooting

- **Nothing is dumped**  
  Ensure `text_replace/dump/` exists and dumping is enabled. Some polygons are untextured and won’t produce files.
- **My edit doesn’t show up**  
  Check that replacements are enabled and that the edited PNG is in `text_replace/mod/` with the **exact same filename** as the dump. Also verify file permissions.
- **Wrong tile repeated / seams**  
  Make sure you didn’t crop/offset the image. The shader observes DS wrap/mirror/clamp, so an off‑by‑one border in your PNG can become visible when mirrored or repeated.
- **Shimmering when scrolling**  
  Prefer upscales by **integer factors** (2×, 3×, 4× …).


## For contributors (internals, short)

- Dumps are produced from the software‑decoded RGBA (`Decode3DTextureToRGBA`) and written via `stbi_write_png` to `text_replace/dump/<HASH16>_fmt<FMT>_<W>x<H>.png`.
- Replacements are **looked up by filename** derived from the same hash/format/size and loaded with `stb_image` from `text_replace/mod/…` (forced to 4 channels).
- The shader maps DS texel coordinates to the replacement texture using the runtime `ReplSize` uniform; any replacement size works.
- Filenames are case‑sensitive on case‑sensitive filesystems; the `%016llX` formatter produces **uppercase** hex.


## License / attribution

The dumping/replacement feature in this fork relies on `stb_image.h` and `stb_image_write.h` by Sean Barrett (public domain / MIT‑like).

— Have fun modding!