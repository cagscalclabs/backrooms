# BAKROOMS (Codex Prototype)

NOTE: I got it in my head to just tell CODEX: here's what I'm looking for, but you create the progression and win conditions... and see what happens.

Backrooms-inspired first-person CE game with a fixed-point raycaster.

## Controls
- `Up` / `Down`: move forward/back
- `Left` / `Right`: turn
- `Y=` / `WINDOW`: strafe left/right
- `ALPHA` (hold): run / fast strafe modifier
- `2nd`: interact
- `CLEAR`: quit

## Notes
- Rendering uses integer fixed-point math in the main engine path.
- Raycasting uses adaptive columns (`46` normal / `44` fast while turning or running).
- Renderer uses precomputed ray tables, capped DDA depth, and adaptive entity stride for better CE performance.
- Walls use lightweight per-level textures (sampled in fixed-point) rather than flat fills.
- Sanity starts at `100%`, decays slowly, drops on entity contact, and can be restored by tonic items.
- Visual corruption ramps at `75%`, `50%`, and `25%` sanity; at `25%` creatures become hostile.
- At `0%` sanity, the run ends.
- One shard per level reveals that level's warp/exit and de-corrupts that level's visuals.
- Shards now map to sanity components with persistent effects:
  sight (hallucinations), movement (control inversion pulses), time (occasional rewind), mind (faster decay).
- Sanity and shard-effect progress persist between sessions via save data.
- Playable levels now use `24x24` layouts (larger exploration footprint per zone).
- World is split into multiple themed zones.
- Contains a hidden escape condition.

## Build
```sh
cd /Users/acagliano/CEdev/source/backrooms_codex
make
```

Output:
- `bin/BAKROOMS.8xp`
