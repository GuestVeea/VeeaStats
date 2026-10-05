# Fonts for the HUD skins

| Header                           | Font                          | Used by            | Source (github.com/google/fonts) | License                      |
|----------------------------------|-------------------------------|--------------------|----------------------------------|------------------------------|
| `orbitron_semibold.h`            | Orbitron, weight 600          | JARV               | `ofl/orbitron`                   | `OFL-Orbitron.txt`           |
| `rajdhani_semibold.h`            | Rajdhani SemiBold             | JARV               | `ofl/rajdhani`                   | `OFL-Rajdhani.txt`           |
| `michroma_regular.h`             | Michroma                      | Galactic Conflict  | `ofl/michroma`                   | `OFL-Michroma.txt`           |
| `saira_semicondensed_medium.h`   | Saira Semi Condensed Medium   | Galactic Conflict  | `ofl/sairasemicondensed`         | `OFL-SairaSemiCondensed.txt` |
| `pressstart2p_regular.h`         | Press Start 2P                | Federation Gunship | `ofl/pressstart2p`               | `OFL-PressStart2P.txt`       |
| `tiny5_regular.h`                | Tiny5                         | Federation Gunship | `ofl/tiny5`                      | `OFL-Tiny5.txt`              |
| `hennypenny_regular.h`           | Henny Penny                   | Halloween          | `ofl/hennypenny`                 | `OFL-HennyPenny.txt`         |
| `fredoka_semibold.h`             | Fredoka, weight 600           | Halloween          | `ofl/fredoka`                    | `OFL-Fredoka.txt`            |

All are embedded in the program so nothing needs installing. To keep them small
they were trimmed to Western European characters, then converted to C headers.

The SIL Open Font License treats a trimmed font as a modified version, which may
not keep a Reserved Font Name. So the fonts added for Galactic Conflict,
Federation Gunship and Halloween were also given their own family names inside
the file ("VeeaStats Wide", "VeeaStats Condensed", "VeeaStats Pixel", "VeeaStats
Pixel Text", "VeeaStats Spooky", "VeeaStats Rounded") with `rename.py` below. (Orbitron, added earlier, also has a Reserved
Font Name and should get the same treatment when it is next regenerated.)

```sh
pip install fonttools
fonttools varLib.instancer "Orbitron[wght].ttf" wght=600 -o Orbitron-SemiBold.ttf   # Orbitron ships as a variable font
U="U+0020-007E,U+00A0-00FF,U+2013-2014,U+2018-201D,U+2022,U+2026,U+00B0"
pyftsubset Orbitron-SemiBold.ttf --unicodes="$U" --layout-features=kern --no-hinting --desubroutinize --output-file=Orbitron.sub.ttf
pyftsubset Rajdhani-SemiBold.ttf --unicodes="$U" --layout-features=kern --no-hinting --desubroutinize --output-file=Rajdhani.sub.ttf

g++ -O2 -o binary_to_compressed_c ../imgui/misc/fonts/binary_to_compressed_c.cpp
./binary_to_compressed_c -base85 Orbitron.sub.ttf OrbitronSemiBold > orbitron_semibold.h
./binary_to_compressed_c -base85 Rajdhani.sub.ttf RajdhaniSemiBold > rajdhani_semibold.h

# Galactic Conflict, Federation Gunship and Halloween: trim, rename, convert.
fonttools varLib.instancer "Fredoka[wdth,wght].ttf" wght=600 wdth=100 -o Fredoka-SemiBold.ttf   # also a variable font
for item in "Michroma-Regular.ttf|MichromaRegular|michroma_regular|VeeaStats Wide" \
            "SairaSemiCondensed-Medium.ttf|SairaSemiCondensedMedium|saira_semicondensed_medium|VeeaStats Condensed" \
            "PressStart2P-Regular.ttf|PressStart2PRegular|pressstart2p_regular|VeeaStats Pixel" \
            "Tiny5-Regular.ttf|Tiny5Regular|tiny5_regular|VeeaStats Pixel Text" \
            "HennyPenny-Regular.ttf|HennyPennyRegular|hennypenny_regular|VeeaStats Spooky" \
            "Fredoka-SemiBold.ttf|FredokaSemiBold|fredoka_semibold|VeeaStats Rounded"; do
    IFS='|' read ttf symbol header family <<< "$item"
    pyftsubset "$ttf" --unicodes="$U" --layout-features=kern --no-hinting --desubroutinize --output-file=sub.ttf
    python3 rename.py sub.ttf "$family"
    ./binary_to_compressed_c -base85 sub.ttf "$symbol" > "$header.h"
done
```

`rename.py`:

```python
import sys
from fontTools.ttLib import TTFont
path, family = sys.argv[1], sys.argv[2]
font = TTFont(path)
postscript = family.replace(" ", "") + "-Regular"
for record in font["name"].names:
    if record.nameID in (1, 16):  record.string = family
    elif record.nameID == 4:      record.string = family + " Regular"
    elif record.nameID == 6:      record.string = postscript
    elif record.nameID == 3:      record.string = postscript + ";VeeaStats subset"
    elif record.nameID in (17, 18, 21, 22, 25): record.string = "Regular"
font.save(path)
```

Press Start 2P and Tiny5 are pixel fonts: they are only sharp at multiples of
8 px, which is why Federation Gunship uses them at 8, 16 and 32 px.
