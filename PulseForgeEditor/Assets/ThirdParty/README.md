# PulseForge Editor visual-asset notices

These editor-only assets are bundled beside `PulseForgeEditor.exe` under `EditorAssets/`. The CMake post-build
step copies both the assets and these notices for Vulkan and OpenGL editor builds. The binary font files are
unmodified upstream releases; Font Awesome's selected glyphs are limited at atlas-build time and no font subset is
generated.

## Inter 4.1

- Upstream: [rsms/inter release v4.1](https://github.com/rsms/inter/releases/tag/v4.1)
- Files: `Fonts/Inter-Regular.ttf`, `Fonts/Inter-SemiBold.ttf` (from `extras/ttf/` in `Inter-4.1.zip`)
- License: SIL Open Font License 1.1; copyright 2016 The Inter Project Authors. The complete upstream license is
  included in `Inter/OFL.txt`.
- The OFL permits commercial use and redistribution with software subject to its terms, including retaining the
  license and not selling the font by itself. The files are unmodified.
- SHA-256: Regular `40D692FCE188E4471E2B3CBA937BE967878F631AD3EBBBDCD587687C7EBE0C82`; SemiBold
  `78A843FADE9D4612A5567302FB595B56976EB5FCEBF4FEA5A5912D638BAFCDE3`.

## JetBrains Mono 2.304

- Upstream: [JetBrains/JetBrainsMono release v2.304](https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304)
- File: `Fonts/JetBrainsMono-Regular.ttf` (from `fonts/ttf/` in the official v2.304 archive)
- License: SIL Open Font License 1.1; copyright 2020 The JetBrains Mono Project Authors. The complete upstream
  license is included in `JetBrainsMono/OFL.txt`.
- The OFL permits commercial use and redistribution with software subject to its terms, including retaining the
  license and not selling the font by itself. The file is unmodified.
- SHA-256: `A0BF60EF0F83C5ED4D7A75D45838548B1F6873372DFAC88F71804491898D138F`.

## Font Awesome Free 7.3.0

- Upstream: [FortAwesome/Font-Awesome release 7.3.0](https://github.com/FortAwesome/Font-Awesome/releases/tag/7.3.0)
- File: `Icons/Font Awesome 7 Free-Solid-900.otf` (the Free Solid desktop font from the official desktop archive)
- License: SIL Open Font License 1.1 for the desktop font; copyright 2026 Fonticons, Inc.; Reserved Font Name
  "Font Awesome". The full upstream `LICENSE.txt`, including its license split and attribution guidance, is included
  in `FontAwesome/LICENSE.txt` and copied with the editor package.
- Font Awesome's license permits commercial bundling/redistribution with software when the copyright notice and
  license are included; the font may not be sold by itself. The upstream license states that its font files already
  contain sufficient embedded attribution. PulseForge retains the license and uses only generic Free Solid glyphs;
  no Pro or brand assets are included.
- SHA-256: `E5CED4E03C367C82FE265CC54DB9EBB3BB3577DE66F0955266FEB7EF1FDE3B61`.

## FreeType 2.14.2

- Upstream source: the repository's existing pinned copy of [FreeType](https://gitlab.freedesktop.org/freetype/freetype)
  at commit `67c52a0b68eaeb7ae1f2248202924883c4a232d0` (version 2.14.2).
- Purpose: rasterizes Font Awesome's CFF-outline OTF without modifying or converting it. It is linked only into the
  editor target; the engine and game runtime do not depend on it.
- License: FreeType Project License. The license text from the pinned source is included in `FreeType/FTL.txt` and
  copied with the editor package.
