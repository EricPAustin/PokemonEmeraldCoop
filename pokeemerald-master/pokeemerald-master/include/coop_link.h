#ifndef GUARD_COOP_LINK_H
#define GUARD_COOP_LINK_H

#include "siirtc.h"

// Linked co-op: two GBAs (e.g. two mGBA multiplayer windows) play the same game.
//
// Both GBAs run the normal single-player game in lockstep. Every frame they swap
// both players' buttons over the link cable, then simulate that frame with
// exactly the same input on both sides, so the two games stay identical.
// Player 1 (first window) is the normal player character. Player 2 walks a
// second character, and Player 2's window draws the map around Player 2.
//
// The game is always played linked: choosing CONTINUE (both games must have
// the same save file) or NEW GAME on the main menu of both GBAs links them up.
//
// Set to FALSE to remove the feature entirely.
#define COOP_LINKED TRUE

// Debugging aid: when TRUE, a desync doesn't show an error. Instead both games
// stop 64 frames after it and keep per-frame state hashes (gCoopDebugHistory)
// that can be compared to find what diverged first.
#define COOP_DEBUG_DESYNC FALSE

// Max distance (in tiles) between the two players.
#define COOP_TETHER_X 7
#define COOP_TETHER_Y 6

#define LOCALID_COOP_PLAYER2 250

extern bool8 gCoopLinkActive;   // Lockstep session running
extern u8 gCoopLocalPlayer;     // Which player this GBA belongs to (COOP_PLAYER_1/2)
extern u8 gCoopInputOwner;      // Whose buttons drive menus, scripts and battle prompts
extern u32 gCoopTick;           // Frames simulated since the session started
extern u32 gCoopDesyncTick;     // Non-zero once a desync was detected

// Main menu
#define COOP_START_CONTINUE 0
#define COOP_START_NEW_GAME 1
void CoopLink_StartFromMainMenu(u8 startMode);
void CoopLink_StartNewGameIntro(void); // main_menu.c

// Main loop / interrupts
bool8 CoopLink_BeginTick(void);
void CoopLink_EndTick(void);
bool8 CoopLink_TickRanThisFrame(void);
bool8 CoopLink_ShouldStepSound(void);

// Cable club features are off during linked co-op
void CoopLink_RefuseLinkup(void);

// Shared clock
bool8 CoopLink_GetRtcInfo(struct SiiRtcInfo *rtc);

// Input ownership
void CoopLink_SetInputOwnerForBattler(u8 battler);
void CoopLink_EndBattlePrompt(void);
u16 CoopLink_GetPlayerKeys(u8 player);
u16 CoopLink_GetPlayerNewKeys(u8 player);

// Player 2's character (coop_player2.c)
void CoopP2_Update(void);
void CoopP2_OnMapLoad(void);
void CoopP2_EnsureSpawned(void);
struct ObjectEvent *CoopP2_GetObject(void);
bool8 CoopP2_IsWalking(void);
bool8 CoopP2_IsPlayer1MoveAllowed(s16 x, s16 y);
void CoopP2_GetViewBounds(s16 *left, s16 *right, s16 *top, s16 *bottom);
bool8 CoopP2_IsObjectP2(const struct ObjectEvent *objectEvent);
u8 CoopP2_GetInteractingFacing(void);
void CoopP2_OnPlayer1StartedSurfing(void);
void CoopP2_OnPlayer1UsedWaterfall(void);
bool8 CoopP2_GetEncounterPos(s16 *x, s16 *y, bool8 *surfing);
void CoopP2_ClearEncounterPos(void);
bool8 Coop_TryWildEncounterAt(s16 x, s16 y); // field_control_avatar.c
bool8 Coop_TryStartInteractionAt(s16 x, s16 y, u8 elevation, u8 direction); // field_control_avatar.c
bool8 Coop_TrySetupDoorWarpAt(s16 x, s16 y, u8 elevation, u8 direction);
bool8 Coop_TryStepWarpAt(s16 x, s16 y, u8 elevation, u8 direction, bool8 arrow);
bool8 Coop_IsWarpTile(u8 metatileBehavior);

// Player 2's camera (field_camera.c)
bool8 CoopView_IsLocalViewOffset(void);
void CoopView_Update(void);

#endif // GUARD_COOP_LINK_H
