# Linked co-op: whole game

Two emulator windows linked together. Each player has their own character and their own camera,
in the same world, for the whole game. This replaces the Route 101 proof of concept.

## How it works
Both games run the full normal game in lockstep. Every frame they swap both players' buttons over
the link cable, and a frame only runs once both players' buttons for it have arrived. Both games
then simulate exactly the same frame, so they stay identical: same random numbers, same NPCs, same
battles, same saves.

- **Player 1** (first window) is the main character. P1 opens doors, meets trainers that spot
  you and walks into wild Pokémon.
- **Player 2** (second window) walks their own character (the rival sprite of the other gender).
  P2's window follows P2. P2 comes along automatically when P1 uses doors, warps, Fly and so on.
- **Either player can use doors and warps** (doors, exit mats, stairs, ladders, cave entrances).
  Whoever walks in brings the other player along: the other player arrives at the same door.
- **Both players** can press A to talk to people, read signs, pick up items, challenge trainers
  and use PCs. Whoever started the conversation answers its menus.
- **Tether:** the players can't get more than 7 tiles apart sideways or 6 up/down. You can always
  walk back toward each other. If a cutscene moves P1 away, P2 is moved next to P1.
- While P1 surfs, bikes or dives, P2 rides along (hidden on P1's tile) and hops off next to P1
  once P1 is standing still on foot again.

### Who controls what
| Situation | Who presses buttons |
|---|---|
| Walking around | Each player moves their own character |
| START menu | Whoever pressed START (P1 or P2) controls it until it closes |
| Dialogue and battle text | Either player can press A or B |
| Battle choices (FIGHT/BAG/POKéMON/RUN, moves, targets, switching) | The owner of that Pokémon: "P1: What will..." / "P2: What will..." |
| Everything else (cursor in yes/no boxes, naming, shops, PC...) | Whoever started the conversation (usually Player 1) |

Teams, the 3-Pokémon limit and co-op double battles work as in milestone 1.

### Starters
When Professor Birch's bag opens, Player 1 picks a starter, then the bag opens again for Player 2
("Player 2, choose your POKéMON and help out too!"). Each player can pick any of the three, even the
same one. Player 1's starter fights the Zigzagoon alone (that first battle is scripted as a single
battle); co-op double battles start from the next battle. In linked co-op, Player 2's buttons pick
Player 2's starter.


### Party screen
The party screen outside battle now looks like the one in co-op battles: Player 1's lead (blue)
top left, Player 2's lead (gold) below it, then the rest of Player 1's team and the rest of
Player 2's team on the right. SWITCH works as before: moving a Pokémon onto a slot of the other
color hands it to that player. If only one player has Pokémon, the normal layout is used.

### Trainer battles: a mirrored partner (goal 2)
Every trainer brings a partner: another trainer of the same class (Youngster Calvin brings another
Youngster, Roxanne brings another Gym Leader, Sidney another Elite Four member).
- **Team sizes:** each side fields up to 3 Pokémon, the same as each player. A trainer with more than 3
  keeps their 3 strongest (their ace always stays). The partner brings the same number.
- **The partner's Pokémon** are different species at the same levels: first from the partner's own
  team, then from other trainers of that class. The partner never repeats a species already in the battle.
- **Evolution stage fits the level:** each partner Pokémon is moved to the stage a Pokémon of that level
  could have, so early fights never get fully evolved Pokémon (the Route 103 rival's partner used to bring
  a level 5 Gardevoir from Wally's team; now it's a Ralts). It goes down a stage while the level is below
  the evolution level, and up while it's at or above it. Evolutions without a set level count from:
  stones 25, friendship 30, Feebas/Milotic 30, trades 32 (close to the earliest such Pokémon trainers
  use in the original game); they're only added going up 10 levels later than that. Baby Pokémon
  (Pichu, Azurill...) are never used. The trainer's own team is unchanged.
  `mirror_audit.py` (in the project files, `tools/coop-link-harness/`) lists every trainer's partner from `pokeemerald.elf`
  and flags any that are too evolved (before this fix: 188 of 1,634 partner Pokémon; now 0).
- The partner is always the same for a given trainer, and is chosen the same way on both GBAs.
- Classes with only one person: Archie brings an Aqua Admin, Maxie and Tabitha bring Magma members.
  The Champion brings himself with the rest of his team (Wallace & Wallace).
- Beating the partner doesn't count as beating them where they really are, so you can still fight them there.
- If only one player has a Pokémon that can fight, battles are the original singles with no partner.

### Changing teams at the PC (goal 3)
Every PC now has a **CO-OP TEAMS** option. Pick a Pokémon to send it to the other player's team.
If that team already has 3, pick one of theirs to trade places with. Either player can do this,
including for the other player's team.

## How to play
1. Build (in WSL, from `pokeemerald-master/pokeemerald-master`): `make -j$(nproc)`
   (All patches are already applied in your folder.)
2. In mGBA: load `pokeemerald.gba`, then **File → New multiplayer window** and load the same ROM.
   Give each window its own controller.
3. In **both** windows, on the main menu, choose one of these with **A** (this ROM is co-op only,
   so both always link up; there is no single-player mode):
   - **NEW GAME**: start a fresh adventure together. The save files don't matter. Player 1 goes
     through Professor Birch's introduction (name, boy/girl) and Player 1's option settings are used.
   - **CONTINUE**: continue a save together. Both windows must load the **same save**: mGBA's first
     window uses `pokeemerald.sav` and the second uses `pokeemerald.sa2`, so copy `.sav` over `.sa2`.
   "Co-op link: waiting for the other player..." appears, then both games start together (B on that
   screen cancels back to the title). If one window picked NEW GAME and the other CONTINUE, you'll be
   asked to pick the same option.

**Saving:** either player can save from the START menu. Both games write the same save, so
`pokeemerald.sav` and `pokeemerald.sa2` stay identical and next session links right away.
If they ever differ, you get "The two save files don't match!": copy `.sav` over `.sa2` again.

## Messages you might see
- **The co-op link was lost.** One window was closed or the link broke. Reset both games and link again
  from your last save.
- **Co-op desync!** The two games got out of step. This shouldn't happen. Reset both games and tell
  me what you were doing when it appeared.

## Not available during co-op
- The Cable Club, Wireless Club, Union Room, record mixing, link contests and link battles
  (the attendants just say "please visit again"). The link cable is busy with co-op.
- Player 2 can't surf. Trainers only spot Player 1.
- Player 2 can only use doors on the map Player 1 is on (not doors just across a route border).
- Fast-forwarding one window doesn't help: each game waits for the other.

## Testing done
Two linked mGBA cores, scripted inputs, comparing both games' state every 32 frames:
- Walking in and out of a Pokémon Center, across the Route 102 → Oldale connection, onto Route 103.
- A trainer battle (Calvin): each player only controls their own Pokémon's choices; either can advance text.
- Saving from the START menu: link stays up, both save files identical afterwards.
- P2 opening the START menu and party screen; P1's buttons are ignored while P2 has the menu.
- Two 45-cycle random-walk soak tests on Route 102 with wild battles, menus and NPC talk
  (about 29,000 frames each): no desync. Repeated after goals 2 and 3: still no desync.
- Calvin + mirrored partner (Youngster Ben with a Gulpin): "YOUNGSTER CALVIN and YOUNGSTER BEN want
  to battle!", battle completes, Calvin is marked as beaten and Ben isn't.
- Player 2 walking into the Oldale Pokémon Center door (P1 stayed outside): door opens, P2 walks in,
  both arrive inside. Then P2 walks out through the exit mat and both arrive outside.
- Player 2 walking to the Pokémon Center PC, opening CO-OP TEAMS and swapping Mudkip (P1) with
  Torchic (P2); P1's buttons were ignored meanwhile.

## Starting from NEW GAME
- Both games wipe to a blank save with Player 1's options, then run Birch's introduction together.
- Fixed on the way: the trainer ID was seeded from a hardware timer when you finish naming yourself,
  which differs between the GBAs; it now uses the shared random numbers. The main menu's video
  memory clears are chunked like the others.
- Tested: Player 1 with a save and Player 2 with none both pick NEW GAME. The introduction, naming,
  the truck, arriving in Littleroot and walking into the house stay in sync (about 15,000 frames).
  Player 2 appears next to Player 1 once the cutscenes let go.

## Always linked
- A on CONTINUE or NEW GAME always starts the co-op link (SELECT still works too). OPTION and
  Mystery Gift work as before.
- Tested: both windows pressing A on NEW GAME (introduction, naming, truck, Littleroot) and on
  CONTINUE (a trainer battle on Route 102): linked, no desync.

## Bugs fixed while adding starters
- **Linked co-op broke at Birch's bag** ("Communication error"). The starter screen cleared all of
  video memory in one go, which blocked the link cable interrupt. It's now cleared in chunks.
- **Desync after picking a starter.** A new Pokémon's nickname kept leftover bytes after the name,
  and those bytes differed between the two GBAs (they'd also end up in the save, so the next link
  would say the saves don't match). Nicknames are now padded properly.
- For future debugging: set `COOP_DEBUG_DESYNC` to `TRUE` in `include/coop_link.h`. A desync then
  freezes both games at the same frame and keeps per-frame state hashes for comparing.

## What changed since the proof of concept
- Lockstep is strict: a frame runs only with both players' buttons for that frame (2-frame input buffer),
  so long loads on one side never pile up lag.
- Link-killing code is disabled during co-op: the Pokémon Center's wireless search, the cable club
  linkups, link open/close calls.
- Large VRAM clears are done in chunks so the link interrupt keeps running during map changes.
- Graphics copies are finished at the start of every frame, so code waiting on them behaves the same on both GBAs.
- Frame counters and the second random number generator are synced at session start.
- Player 2 no longer becomes the main character after a battle or menu.
- Talking to Player 2 does nothing (it used to run a random script).

## How to undo it
```
patch -R -p1 < "../../coop-always-linked.patch" # goes back to SELECT to link (A plays solo)
patch -R -p1 < "../../coop-new-game.patch"     # removes starting from NEW GAME (after the line above)
patch -R -p1 < "../../coop-starters.patch"     # removes the starter changes (after the line above)
patch -R -p1 < "../../coop-p2-warps.patch"    # removes Player 2 using doors (after the line above)
patch -R -p1 < "../../coop-party-layout.patch" # removes the new party screen (after the line above)
patch -R -p1 < "../../coop-goals-2-3.patch"   # removes goals 2 and 3 (after the line above)
patch -R -p1 < "../../coop-linked.patch"      # removes everything since the proof of concept
```
(`coop-linked.patch` contains everything, so use it on its own, not after the others.) `coop-full-from-vanilla.patch` is the whole co-op work
(milestone 1 + linked co-op) as one diff against the original pret/pokeemerald.
Set `COOP_LINKED` to `FALSE` in `include/coop_link.h` to build without linked co-op.

## Next steps
1. Player 2's trainer sprite in battle (cosmetic: right now only Player 1's back sprite slides in).
2. Wild battles against two wild Pokémon (wild battles are still 2 players vs 1 Pokémon).
3. Play-test a real run: gyms, Elite Four and Champion, and report anything odd.
