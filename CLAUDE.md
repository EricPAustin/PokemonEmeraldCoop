# Pokémon Co-op Mod — Project Context

Two-player co-op Pokémon Emerald ROM hack built by Declan and Eric, with Claude writing most of the code. This file is the shared context for every Claude session (Claude Code, claude.ai chats). Read it first; update it when you finish a feature or make a design decision.

For how the co-op works in detail (controls, saving, error messages, testing done), see `LINKED_COOP_NOTES.md`.

## Goal
- Play the whole game together on PC, co-op.
- Every trainer gets a counterpart, and every trainer fight is a 2v2 multi battle (each player controls their own Pokémon).
- Later: new/changed locations, a casino, other additions.

## Foundation (as built)
- **Base:** vanilla pret/pokeemerald (Gen 3 mechanics), not pokeemerald-expansion.
- **Co-op model:** linked lockstep. Two mGBA windows (File → New multiplayer window) connected over the emulated link cable. Every frame both games swap both players' buttons and only advance once both have arrived, so both simulate identical frames (same RNG, NPCs, battles, saves). Each player has their own character and camera; P2 is tethered to P1 (7 tiles across, 6 up/down).
- **Teams:** each player has up to 3 Pokémon, split from the one party. Teams are swapped at any PC via **CO-OP TEAMS**.
- **Player 2's identity:** `SaveBlock2.coopP2Name` (the old unused `filler_90`), `coopP2Gender` and `coopP2Named` (spare option bits), so no save space was used and old saves still load. Older saves have `coopP2Named == FALSE` and fall back to the rival sprite named BRENDAN/MAY. Always read them through `Coop_GetPlayer2Gender()` / `Coop_CopyPlayerName()` in `src/coop.c`.
- **Saves:** both games write the same save. mGBA's second window uses `pokeemerald.sa2`, which must match `pokeemerald.sav`.
- **Config:** `include/coop_link.h`: `COOP_LINKED` (build with/without linked co-op), `COOP_DEBUG_DESYNC` (freeze on desync and keep per-frame state hashes).

## Repo layout
- `pokeemerald-master/pokeemerald-master/`: the game source, with every patch already applied. Build here.
- `pokeemerald-master/agbcc/`: the agbcc compiler source (not in git). Each person clones and builds it once; see Build & play.
- `*.patch` at the root: history of the co-op work as diffs. `coop-full-from-vanilla.patch` is the whole thing against vanilla pret/pokeemerald. These are reference only; do not reapply them to the source folder (that is what produced the `.rej` files that were cleaned up).
- Main co-op code: `src/coop.c`, `src/coop_link.c`, `src/coop_player2.c`, `include/coop_link.h`.

## Roadmap
- **Phase 0 – Setup:** private GitHub repo ✅, clean build running in mGBA, this file in place ✅.
- **Phase 1 – Two players exist:** ✅ P2 character, P2 starter, split teams, shared save, NEW GAME/CONTINUE in co-op, P2 can use doors/warps. P2 picks their own gender and name in Birch's intro and can nickname their starter in Birch's lab.
- **Phase 2 – Co-op battles:** ✅ multi battles, each player picks their own Pokémon's moves ("NAME: What will..."). P2's trainer back sprite slides in and throws P2's Pokémon. To do: wild double battles, edge cases below.
- **Phase 3 – Doubled trainers:** ✅ generated "mirrored partner" for every trainer (same class, different species, same levels, ≤3 Pokémon per side). To do: hand-tune notable fights (gyms, Elite Four, Champion).
- **Phase 4 – Content:** casino (can reuse Mauville Game Corner code), new/changed maps, story additions.

## Known limitations (from the notes)
- P2 can't surf (rides along hidden on P1's tile). Trainers only spot P1.
- P2 can only use doors on P1's current map.
- Cable Club, Union Room, record mixing, link battles/contests are disabled (link cable is busy).
- P1's starter fights the first Zigzagoon alone (scripted single battle).
- If both players pick the same gender, their overworld sprites look identical (P2 uses the rival sprite of their gender).
- In yes/no boxes during dialogue, the other player can still press A/B, so P1 can answer P2's "nickname?" question by mashing A.
- Story cutscenes and dialogue only involve P1 (e.g. Dad on the TV, NPCs address only {PLAYER}). Fixing means editing each event by hand: deferred, low gameplay impact.

## Known risk areas
- **Save space is almost full** (build output, 2026-10-23): SaveBlock1 15752/15872 B (99.2%, ~120 B free), SaveBlock2 3884/3968 B (97.9%), PokemonStorage 94.5%. Any new saved data (casino, new flags/vars beyond spare ones, P2 stats) must reuse existing space or free some first. The build fails if a block overflows.
- **Desyncs:** anything that reads hardware timers, uninitialised memory, or does long DMA/VRAM clears can break lockstep. Large clears must be chunked so the link interrupt keeps running.
- Battle edge cases where code assumes a single player: Pickup, Pay Day, Exp. Share, money on win/loss, whiteout.
- **Two-PC play:** not yet tested. Needs mGBA link play across two computers on the home network (vs. one PC + Parsec).

## Tools
- GitHub (private repo): source of truth
- mGBA: emulator (multiplayer windows); use save states for testing
- Parsec: fallback if linking across two PCs doesn't work
- Porymap: map editor
- WSL + devkitARM/agbcc: local builds
- Aseprite / LibreSprite / Pixelorama: sprites (16-color palettes)

## Build & play
One-time setup (WSL Ubuntu):
```
sudo apt update && sudo apt install -y build-essential binutils-arm-none-eabi git libpng-dev
cd pokeemerald-master && git clone https://github.com/pret/agbcc && cd agbcc
./build.sh && ./install.sh ../pokeemerald-master
```
Then each time:
1. In WSL, from `pokeemerald-master/pokeemerald-master`: `make -j$(nproc)`
2. mGBA: load `pokeemerald.gba`, then File → New multiplayer window, load the same ROM, one controller per window.
3. Both windows: pick NEW GAME or CONTINUE with A, wait for "Co-op link: waiting for the other player...".

## Workflow rules
- Pull before every session.
- One branch per feature; merge to main only when it builds and passes a playtest.
- Make changes directly in the source folder and commit them. Patch files are no longer needed now that the source is in git.
- Never commit ROMs, saves or built files (see `.gitignore`).
- Bug reports: what you did, what happened, screenshot, save state if possible. For desyncs, say what both players were doing.
- Update this file when a feature lands or a decision changes.

## Decisions log
- **Vanilla pokeemerald over pokeemerald-expansion.** Original plan was expansion (Gen 5+ mechanics). The co-op was built on vanilla and is ~4,000 lines of changes; porting to expansion would mean redoing much of it. Keeping vanilla. Revisit only if newer mechanics become a must-have.
- **Linked lockstep (two windows) over shared input on one screen via Parsec.** Each player gets their own screen and character; already working and soak-tested with no desyncs.
- **3 Pokémon per player** (one party split 3/3) instead of a separate full second party. Avoids the save-space problem entirely.
- GitHub repo over a shared sync folder.

## Status
- Co-op is playable from NEW GAME through at least Route 102/103 per the testing notes.
- 2026-10-23: reviewed repo against the original plan, rewrote this file to match what's built, removed 20 leftover `.rej` files (all hunks verified as already applied or superseded), added `.gitignore`.
- 2026-10-23: clean build on Declan's PC (WSL Ubuntu, agbcc), `pokeemerald.gba` produced.
- 2026-10-23: Player 2 update (playtested OK): P2 gender + name in Birch's intro, P2 nickname for their starter in the lab, P2's name on the starter screen and CO-OP TEAMS PC, P2 overworld sprite follows P2's gender, P2 trainer back sprite in co-op battles (each player throws their own Pokémon, using the multi-battle send-out path).
- 2026-10-23: first playtest of the Player 2 update by Declan + Eric. Fixes from it: P1 always picks the starter first (even if P2 opened the bag) and the starter screen names whose turn it is; battle prompts use player names instead of P1/P2; Pokémon Center nurse no longer prints a garbage "player is waiting in the Union Room" message (co-op skipped clearing that name).
- 2026-10-23: re-tested those fixes: all work.
- Next:
  1. Test linking across two PCs on the home network.
  2. Wild double battles.
  3. Full playthrough test: gyms, Elite Four, Champion.
