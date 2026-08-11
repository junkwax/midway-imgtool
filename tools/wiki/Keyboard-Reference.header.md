The tables below are generated from the in-app help (`h`), which ships inside
your binary — so this page and the app cannot disagree.

## Context-sensitive keys

Several keys change meaning depending on state. These are the ones that
surprise people:

| Key | Normally | But when… |
|---|---|---|
| `[` `]` | Set palette for marked / for current image | **Pencil or Variant Paint active** → shrink / grow brush radius |
| `H` | Show help | **Floating paste active** → flip horizontally |
| `V` | Variant Paint tool | **Floating paste active** → flip vertically |
| `L` | Lasso tool | **Floating paste active** → drop paste to a layer |
| `Del` | Delete image (image list focused) | **Selection active** → clear the selected pixels |
| `Arrows` | Navigate the image list | **After Paste as New Sprite or a canvas resize** → nudge art inside the canvas |
| `Up`/`Down`/`PgUp`/`PgDn` | Navigate the image list | **Palette list clicked last** → navigate palettes |

Pressing a tool's key a second time toggles it back off.
