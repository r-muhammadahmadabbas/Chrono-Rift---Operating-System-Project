# Assets (Sprites)

Put your sprite sheets in this folder.

This project’s SFML renderer (Arbiter) looks for:

- `assets/hero_sheet.png`

For now, `hero_sheet.png` is used for BOTH heroes and enemies.
Later, you can switch to one sheet per character/enemy (or add `enemy_sheet.png`).

## Expected layout (simple grid)

The sprite-sheet code assumes a uniform grid of frames.

Default assumptions (change in `arbiter/arbiter.cpp` if your sheet differs):

### Hero sheet (`assets/hero_sheet.png`)

- A single row with **6 attack frames**.
- Example: **168x37** image => each frame is **28x37**.
- Idle/heal currently use frame 0 (you can extend later).

### Enemy sheet (`assets/enemy_sheet.png`)

- Defaults are still 32x32 with idle/attack rows (adjust when you add your sheet).

If your sheet has a different layout (48x48, different rows/cols, etc.), adjust the constants near the top of `render_loop()` in `arbiter/arbiter.cpp`.

## Licensing note

Avoid using copyrighted game rips for your final submission/report unless you have permission.
Use CC0/royalty-free packs when possible.
