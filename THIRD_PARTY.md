# Third-party software

MeshDeck OS is primarily **MIT**-licensed (see [LICENSE](LICENSE)).  
It also incorporates or links software under other open-source licenses.  
This file lists those components and how they are used.

## MeshCore (MIT)

- **Project:** [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore)
- **License:** MIT
- **Use:** Mesh networking stack. Fetched at build time (not vendored in this repository by default).

## Codec2 (LGPL-2.1)

- **Project:** [drowe67/codec2](https://github.com/drowe67/codec2) (Codec 2 by David Rowe and contributors)
- **License:** [GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- **Full license text in this repo:** [firmware/codec2/COPYING](firmware/codec2/COPYING)
- **Use:** Low-bitrate speech codec for the experimental **voice call** feature.
- **Build flag:** Only linked into **beta** firmware (`MESHDECK_BETA` / `MeshDeck_TDeck_868_beta`). Stable builds do not include Codec2.
- **How it is linked:** Statically compiled into the ESP32 firmware image (normal for PlatformIO / embedded).

### LGPL compliance (how MeshDeck handles it)

1. **Codec2 remains LGPL-2.1.** It is not re-licensed as MIT.  
2. **MeshDeck application code remains MIT.** LGPL does not require the whole firmware to be LGPL.  
3. **Source for Codec2** is provided under `firmware/codec2/` (including `COPYING`) so recipients can obtain the same library version used in the build.  
4. **Recipients can rebuild** the firmware from this repository (see FLASHING.md / build scripts), including substituting or modifying the Codec2 sources under LGPL terms.  
5. **Any modifications to Codec2** made in this tree, if distributed, remain under LGPL-2.1.

If you distribute MeshDeck binaries that include the voice/beta build, you must preserve Codec2 attribution and license notices and ensure the corresponding Codec2 source remains available (as this repository does).

## Other dependencies

| Component | License | Notes |
|-----------|---------|--------|
| [Natural Earth](https://www.naturalearthdata.com/) map data | Public domain | Built-in offline map |
| [qrcodegen](https://github.com/nayuki/QR-Code-generator) | MIT | QR display |
| Arduino / ESP-IDF / PlatformIO libs | Various | Resolved at build time via PlatformIO |

## Questions

For license questions about MeshDeck application code, see [LICENSE](LICENSE).  
For Codec2 itself, see [firmware/codec2/COPYING](firmware/codec2/COPYING) and the upstream project.
