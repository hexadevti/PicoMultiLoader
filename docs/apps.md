# Apps on the Home screen

Home lists:

- every top-level folder on the SD card that holds an `app.ini` or a `.uf2`, and
- every loose `.uf2` file in the card's root.

Favourites (F2) and recently run apps are kept in `/pml/favourites.txt` and `/pml/recents.txt`, one path per line. You can edit these files by hand.

## app.ini

Put `app.ini` in the app's folder. Every key is optional, and ` ;` starts a comment.

```ini
[app]
name     = NES
author   = picocalc community
version  = 1.4.0
category = Emulators          ; Emulators | Tools | Dev | Media
icon     = icon.bmp           ; the default
binary   = picocalc-nes.uf2   ; binary.rp2040 / binary.rp2350 win over it
board    = rp2350             ; p4 | rp2040 | rp2350 | any (a list is fine)
desc     = NES emulator with save states.
```

| Key | What happens when it is missing |
|---|---|
| `name` | The folder name is shown. |
| `binary` | The folder's only `.uf2` runs. If the folder has several, Enter opens the folder in Files. |
| `board` | The app is shown on every board. If it is set, other boards hide the app. |
| `category` | The app appears only under All, Recent and Favourites. |

## Icons

The icon is a **32 × 32, 4-bit (16-colour), uncompressed BMP**. Each pixel's value is a **palette role**, not a fixed colour, so the icon follows the user's theme.

| Index | Role | Typical use |
|---|---|---|
| 0 | *(clear)* | The cell behind shows through. |
| 1 | PANEL | Dark fill |
| 2 | FIELD | Raised surfaces |
| 3 | LINE | Outlines, frames |
| 4 | DIM | Shadows, secondary detail |
| 5 | FG | Normal text colour |
| 6 | BRIGHT | Highlights |
| 7 | ACCENT | Main theme colour |
| 8 | ACCDK | Dark accent |
| 9 | OK | Green |
| 10 | WARN | Yellow / amber |
| 11 | ERR | Red |
| 12 | ACC2 | Second accent |
| 13–15 | cycled | Rotate between the accent colours a few times a second (animated). |

Index 0 is drawn as transparent, so there is no BG role in icons.

Any image editor that saves 4-bit indexed BMPs will work. Pick the palette entries in the order above. The colours stored in the BMP are ignored; only the index of each pixel matters. Top-down and bottom-up BMPs are both accepted.

An app with no valid icon gets a generic one: a folder, or a window showing the app's first letter.
