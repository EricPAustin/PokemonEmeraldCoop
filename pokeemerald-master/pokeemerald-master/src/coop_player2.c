#include "global.h"
#include "coop.h"
#include "coop_link.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "script.h"
#include "constants/songs.h"
#include "overworld.h"
#include "task.h"
#include "sound.h"
#include "field_screen_effect.h"
#include "field_door.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/flags.h"
#include "constants/field_effects.h"
#include "constants/moves.h"
#include "event_scripts.h"
#include "field_effect.h"
#include "field_effect_helpers.h"
#include "party_menu.h"
#include "pokemon.h"

// Linked co-op: Player 2's character in the overworld.
//
// Player 1 is the game's normal player character: it triggers doors, scripts,
// wild encounters and trainers. Player 2 is an extra object event that walks
// around with Player 2's buttons, within COOP_TETHER_X/Y tiles of Player 1.
// While Player 1 bikes or dives, and right after map changes, Player 2 "rides
// along" (hidden on Player 1's tile) and hops off next to Player 1 as soon as
// Player 1 is standing still.
//
// Surfing: either player can start surfing. Whoever starts brings the other
// along: the other player appears on the water next to them, surfing too.
// After that each player surfs and lands on their own.
//
// Everything here runs inside the shared simulated frame and only reads state
// that is identical on both GBAs, so both games move Player 2 the same way.

#define abs(x) ((x) < 0 ? -(x) : (x))

static struct ObjectEvent *GetPlayer1(void)
{
    return &gObjectEvents[gPlayerAvatar.objectEventId];
}

struct ObjectEvent *CoopP2_GetObject(void)
{
    s32 i;

    if (!gCoopLinkActive)
        return NULL;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        if (gObjectEvents[i].active && gObjectEvents[i].localId == LOCALID_COOP_PLAYER2)
            return &gObjectEvents[i];
    }
    return NULL;
}

bool8 CoopP2_IsObjectP2(const struct ObjectEvent *objectEvent)
{
    return gCoopLinkActive && objectEvent->localId == LOCALID_COOP_PLAYER2;
}

// TRUE while Player 2 walks around on their own (visible, not riding along).
bool8 CoopP2_IsWalking(void)
{
    struct ObjectEvent *p2 = CoopP2_GetObject();
    return p2 != NULL && !p2->invisible;
}

static bool8 MustRideAlong(void)
{
    return (gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_UNDERWATER
                                 | PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE)) != 0;
}

static bool8 IsWithinTether(s16 x, s16 y, s16 otherX, s16 otherY)
{
    return abs(x - otherX) <= COOP_TETHER_X && abs(y - otherY) <= COOP_TETHER_Y;
}

// Called before Player 1 walks onto (x, y).
bool8 CoopP2_IsPlayer1MoveAllowed(s16 x, s16 y)
{
    struct ObjectEvent *p2 = CoopP2_GetObject();

    if (p2 == NULL || p2->invisible)
        return TRUE;
    return IsWithinTether(x, y, p2->currentCoords.x, p2->currentCoords.y);
}

// The area around Player 2 where map objects should exist, in the same format
// as the vanilla area around the camera (see TrySpawnObjectEvents).
void CoopP2_GetViewBounds(s16 *left, s16 *right, s16 *top, s16 *bottom)
{
    struct ObjectEvent *p2 = CoopP2_GetObject();
    s16 x, y;

    if (p2 == NULL || p2->invisible)
    {
        x = gSaveBlock1Ptr->pos.x;
        y = gSaveBlock1Ptr->pos.y;
    }
    else
    {
        x = p2->currentCoords.x - MAP_OFFSET;
        y = p2->currentCoords.y - MAP_OFFSET;
    }
    *left = x - 2;
    *right = x + MAP_OFFSET_W + 2;
    *top = y;
    *bottom = y + MAP_OFFSET_H + 2;
}

static bool8 IsAnyObjectAt(const struct ObjectEvent *self, s16 x, s16 y)
{
    s32 i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *obj = &gObjectEvents[i];
        if (!obj->active || obj == self)
            continue;
        if ((obj->currentCoords.x == x && obj->currentCoords.y == y)
         || (obj->previousCoords.x == x && obj->previousCoords.y == y))
            return TRUE;
    }
    return FALSE;
}

// Can Player 2 be placed on this tile when hopping off next to Player 1?
static bool8 IsGoodSpotForP2(const struct ObjectEvent *p2, s16 x, s16 y, u8 elevation)
{
    u8 behavior, tileElevation;

    if (MapGridGetCollisionAt(x, y) || GetMapBorderIdAt(x, y) == CONNECTION_INVALID)
        return FALSE;
    tileElevation = MapGridGetElevationAt(x, y);
    if (tileElevation != 0 && tileElevation != 15 && elevation != 0 && elevation != 15 && tileElevation != elevation)
        return FALSE;
    behavior = MapGridGetMetatileBehaviorAt(x, y);
    if (MetatileBehavior_IsSurfableWaterOrUnderwater(behavior)
     || MetatileBehavior_IsDoor(behavior)
     || MetatileBehavior_IsWarpDoor(behavior)
     || MetatileBehavior_IsNonAnimDoor(behavior)
     || MetatileBehavior_IsLadder(behavior)
     || MetatileBehavior_IsEscalator(behavior)
     || Coop_IsWarpTile(behavior))
        return FALSE;
    return !IsAnyObjectAt(p2, x, y);
}

// ---------------------------------------------------------------------------
// Surfing

#define SHOW_MON_CRY_NO_DUCKING (1u << 31) // See field_effect.c

static u8 GetP2GraphicsId(u8 state)
{
    return GetRivalAvatarGraphicsIdByStateIdAndGender(state, Coop_GetPlayer2Gender());
}

static u8 GetObjectId(const struct ObjectEvent *objectEvent)
{
    return objectEvent - gObjectEvents;
}

// Player 2's surfing state is their sprite, so it survives battles and saves.
static bool8 IsP2Surfing(const struct ObjectEvent *p2)
{
    return p2->graphicsId == GetP2GraphicsId(PLAYER_AVATAR_STATE_SURFING);
}

static u8 FindSurfBlob(u8 objectEventId)
{
    s32 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        if (gSprites[i].inUse && gSprites[i].callback == UpdateSurfBlobFieldEffect
         && gSprites[i].data[2] == objectEventId) // sPlayerObjId
            return i;
    }
    return MAX_SPRITES;
}

static u8 CreateSurfBlob(s16 x, s16 y, u8 objectEventId, u8 bobState)
{
    u32 savedArgs[3];
    u8 spriteId;

    // Don't disturb a field effect argument a script may have set up.
    savedArgs[0] = gFieldEffectArguments[0];
    savedArgs[1] = gFieldEffectArguments[1];
    savedArgs[2] = gFieldEffectArguments[2];
    gFieldEffectArguments[0] = x;
    gFieldEffectArguments[1] = y;
    gFieldEffectArguments[2] = objectEventId;
    spriteId = FieldEffectStart(FLDEFF_SURF_BLOB);
    if (spriteId < MAX_SPRITES)
        SetSurfBlob_BobState(spriteId, bobState);
    gFieldEffectArguments[0] = savedArgs[0];
    gFieldEffectArguments[1] = savedArgs[1];
    gFieldEffectArguments[2] = savedArgs[2];
    return spriteId;
}

// Instantly puts Player 2 on a surfing Pokémon (sprite and blob).
static void StartP2SurfState(struct ObjectEvent *p2)
{
    if (!IsP2Surfing(p2))
    {
        ObjectEventSetGraphicsId(p2, GetP2GraphicsId(PLAYER_AVATAR_STATE_SURFING));
        ObjectEventTurn(p2, p2->facingDirection);
    }
    if (FindSurfBlob(GetObjectId(p2)) == MAX_SPRITES)
        CreateSurfBlob(p2->currentCoords.x, p2->currentCoords.y, GetObjectId(p2), BOB_PLAYER_AND_MON);
}

// Instantly takes Player 2 off the water.
static void StopP2SurfState(struct ObjectEvent *p2)
{
    u8 blobId = FindSurfBlob(GetObjectId(p2));

    if (blobId != MAX_SPRITES)
        DestroySprite(&gSprites[blobId]);
    if (IsP2Surfing(p2))
    {
        ObjectEventSetGraphicsId(p2, GetP2GraphicsId(PLAYER_AVATAR_STATE_NORMAL));
        ObjectEventTurn(p2, p2->facingDirection);
        gSprites[p2->spriteId].y2 = 0;
    }
}

static bool8 IsSurfableAt(s16 x, s16 y)
{
    u8 behavior = MapGridGetMetatileBehaviorAt(x, y);
    return MetatileBehavior_IsSurfableWaterOrUnderwater(behavior) && !MetatileBehavior_IsWaterfall(behavior);
}

// Can a surfing player be placed on this tile?
static bool8 IsGoodSurfSpot(const struct ObjectEvent *self, s16 x, s16 y, bool8 currentMapOnly)
{
    s32 borderId = GetMapBorderIdAt(x, y);

    if (borderId == CONNECTION_INVALID || (currentMapOnly && borderId != CONNECTION_NONE))
        return FALSE;
    if (MapGridGetCollisionAt(x, y) || !IsSurfableAt(x, y))
        return FALSE;
    return !IsAnyObjectAt(self, x, y);
}

// A free water tile next to (x, y): beside first, then ahead, behind, diagonal.
static bool8 FindSurfSpotNear(const struct ObjectEvent *self, s16 x, s16 y, u8 facing, bool8 currentMapOnly, s16 *outX, s16 *outY)
{
    // [0]: facing north/south (beside = west/east), [1]: facing west/east
    static const s8 sOffsets[2][8][2] = {
        {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}},
        {{0, -1}, {0, 1}, {-1, 0}, {1, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}},
    };
    u8 set = (facing == DIR_NORTH || facing == DIR_SOUTH) ? 0 : 1;
    s32 i;

    for (i = 0; i < 8; i++)
    {
        s16 spotX = x + sOffsets[set][i][0];
        s16 spotY = y + sOffsets[set][i][1];
        if (IsGoodSurfSpot(self, spotX, spotY, currentMapOnly))
        {
            *outX = spotX;
            *outY = spotY;
            return TRUE;
        }
    }
    return FALSE;
}

static void PlaceP2At(struct ObjectEvent *p2, s16 x, s16 y, u8 facing, bool8 surfing)
{
    ObjectEventClearHeldMovementIfActive(p2);
    MoveObjectEventToMapCoords(p2, x, y);
    ObjectEventUpdateElevation(p2);
    p2->invisible = FALSE;
    if (surfing)
        StartP2SurfState(p2);
    else
        StopP2SurfState(p2);
    ObjectEventTurn(p2, facing);
}

// Player 2 started surfing while Player 1 is on land: Player 1 appears on the
// water next to Player 2, surfing too. The camera jumps with Player 1.
static bool8 TryTeleportP1ToSurfNextTo(const struct ObjectEvent *p2)
{
    struct ObjectEvent *p1 = GetPlayer1();
    s16 x, y;

    if (gFieldCamera.x != 0 || gFieldCamera.y != 0)
        return FALSE; // Camera mid-scroll (shouldn't happen: everyone is standing still)
    // Only within Player 1's current map, so the camera doesn't cross a map connection.
    if (!FindSurfSpotNear(p1, p2->currentCoords.x, p2->currentCoords.y, p2->facingDirection, TRUE, &x, &y))
        return FALSE;

    MoveCameraAndRedrawMap(x - p1->currentCoords.x, y - p1->currentCoords.y);
    MoveObjectEventToMapCoords(p1, x, y); // Also resets the camera object
    ObjectEventUpdateElevation(p1);
    ObjectEventSetGraphicsId(p1, GetPlayerAvatarGraphicsIdByStateId(PLAYER_AVATAR_STATE_SURFING));
    ObjectEventTurn(p1, p2->facingDirection);
    SetPlayerAvatarStateMask(PLAYER_AVATAR_FLAG_SURFING);
    p1->fieldEffectSpriteId = CreateSurfBlob(x, y, gPlayerAvatar.objectEventId, BOB_PLAYER_AND_MON);
    Overworld_ClearSavedMusic();
    Overworld_ChangeMusicTo(MUS_SURF);
    return TRUE;
}

// Player 1 just started surfing (vanilla Surf field effect finished): Player 2
// appears on the water next to Player 1, surfing too.
void CoopP2_OnPlayer1StartedSurfing(void)
{
    struct ObjectEvent *p1 = GetPlayer1();
    struct ObjectEvent *p2 = CoopP2_GetObject();
    s16 x, y;

    if (p2 == NULL || p2->invisible || IsP2Surfing(p2))
        return;
    if (FindSurfSpotNear(p2, p1->currentCoords.x, p1->currentCoords.y, p1->facingDirection, FALSE, &x, &y))
        PlaceP2At(p2, x, y, p1->facingDirection, TRUE);
}

// Index of the party Pokémon that uses Surf for Player 2 (Player 2's own first),
// or PARTY_SIZE.
static u8 FindP2SurfMon(void)
{
    s32 pass, i;

    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < PARTY_SIZE; i++)
        {
            struct Pokemon *mon = &gPlayerParty[i];
            if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(mon, MON_DATA_IS_EGG))
                continue;
            if ((pass == 0) != (GetMonCoopOwner(mon) == COOP_PLAYER_2))
                continue;
            if (MonKnowsMove(mon, MOVE_SURF))
                return i;
        }
    }
    return PARTY_SIZE;
}

void CoopP2_FindSurfMon(void)
{
    gSpecialVar_Result = FindP2SurfMon();
}

// Player 2 pressed A facing (x, y): can they surf onto it?
static bool8 CanP2StartSurfing(struct ObjectEvent *p2, s16 x, s16 y, u8 dir)
{
    struct ObjectEvent *p1 = GetPlayer1();

    if (IsP2Surfing(p2) || !FlagGet(FLAG_BADGE05_GET))
        return FALSE;
    if (p2->previousElevation != 3
     || GetCollisionAtCoords(p2, x, y, dir) != COLLISION_ELEVATION_MISMATCH
     || !MetatileBehavior_IsSurfableFishableWater(MapGridGetMetatileBehaviorAt(x, y))
     || !IsWithinTether(x, y, p1->currentCoords.x, p1->currentCoords.y)
     || IsAnyObjectAt(p2, x, y))
        return FALSE;
    return FindP2SurfMon() != PARTY_SIZE;
}

// Player 2 uses Surf (special from EventScript_CoopP2UseSurf, followed by waitstate).
#define tState data[0]
#define tMonId data[1]

static void Task_CoopP2Surf(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    struct ObjectEvent *p2 = CoopP2_GetObject();
    s16 x, y;

    if (p2 == NULL)
    {
        ScriptContext_Enable();
        DestroyTask(taskId);
        return;
    }

    switch (task->tState)
    {
    case 0:
        gFieldEffectArguments[0] = task->tMonId | SHOW_MON_CRY_NO_DUCKING;
        FieldEffectStart(FLDEFF_FIELD_MOVE_SHOW_MON_INIT);
        task->tState++;
        break;
    case 1:
        if (!FieldEffectActiveListContains(FLDEFF_FIELD_MOVE_SHOW_MON))
        {
            x = p2->currentCoords.x;
            y = p2->currentCoords.y;
            MoveCoords(p2->facingDirection, &x, &y);
            UnfreezeObjectEvent(p2);
            ObjectEventClearHeldMovementIfActive(p2);
            ObjectEventSetGraphicsId(p2, GetP2GraphicsId(PLAYER_AVATAR_STATE_SURFING));
            ObjectEventSetHeldMovement(p2, GetJumpSpecialMovementAction(p2->facingDirection));
            CreateSurfBlob(x, y, GetObjectId(p2), BOB_NONE);
            task->tState++;
        }
        break;
    case 2:
        if (ObjectEventClearHeldMovementIfFinished(p2))
        {
            u8 blobId = FindSurfBlob(GetObjectId(p2));
            if (blobId != MAX_SPRITES)
                SetSurfBlob_BobState(blobId, BOB_PLAYER_AND_MON);
            ObjectEventUpdateElevation(p2);
            ObjectEventTurn(p2, p2->facingDirection);
            if (!(gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER)))
                TryTeleportP1ToSurfNextTo(p2);
            ScriptContext_Enable();
            DestroyTask(taskId);
        }
        break;
    }
}

void CoopP2_StartSurfing(void)
{
    u8 taskId = CreateTask(Task_CoopP2Surf, 0xFF);
    gTasks[taskId].tMonId = gFieldEffectArguments[0];
}

#undef tState
#undef tMonId

// ---------------------------------------------------------------------------

static void RideAlong(struct ObjectEvent *p2)
{
    struct ObjectEvent *p1 = GetPlayer1();

    StopP2SurfState(p2);
    if (p2->currentCoords.x != p1->currentCoords.x || p2->currentCoords.y != p1->currentCoords.y)
        MoveObjectEventToMapCoords(p2, p1->currentCoords.x, p1->currentCoords.y);
    p2->currentElevation = p1->currentElevation;
    p2->previousElevation = p1->previousElevation;
    p2->invisible = TRUE;
}

static u8 OppositeDirection(u8 dir)
{
    switch (dir)
    {
    case DIR_NORTH: return DIR_SOUTH;
    case DIR_SOUTH: return DIR_NORTH;
    case DIR_WEST:  return DIR_EAST;
    default:        return DIR_WEST;
    }
}

// Puts Player 2 on a free tile next to Player 1 (behind first), or keeps them
// riding along if there's no room.
static void PlaceP2NearP1(struct ObjectEvent *p2)
{
    struct ObjectEvent *p1 = GetPlayer1();
    u8 facing = p1->facingDirection;
    u8 dirs[4];
    s16 surfX, surfY;
    s32 i;

    // Player 1 is surfing: surf alongside.
    if ((gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_SURFING)
     && FindSurfSpotNear(p2, p1->currentCoords.x, p1->currentCoords.y, facing, FALSE, &surfX, &surfY))
    {
        PlaceP2At(p2, surfX, surfY, facing, TRUE);
        return;
    }

    dirs[0] = OppositeDirection(facing);
    dirs[1] = (facing == DIR_NORTH || facing == DIR_SOUTH) ? DIR_WEST : DIR_NORTH;
    dirs[2] = OppositeDirection(dirs[1]);
    dirs[3] = facing;

    for (i = 0; i < 4; i++)
    {
        s16 x = p1->currentCoords.x;
        s16 y = p1->currentCoords.y;
        MoveCoords(dirs[i], &x, &y);
        if (IsGoodSpotForP2(p2, x, y, p1->currentElevation))
        {
            PlaceP2At(p2, x, y, facing, FALSE);
            return;
        }
    }
    RideAlong(p2);
}

static void SpawnP2(void)
{
    struct ObjectEvent *p1 = GetPlayer1();
    u8 graphicsId = GetP2GraphicsId(PLAYER_AVATAR_STATE_NORMAL);
    u8 objEventId = SpawnSpecialObjectEventParameterized(graphicsId, MOVEMENT_TYPE_PLAYER, LOCALID_COOP_PLAYER2,
                                                          p1->currentCoords.x, p1->currentCoords.y, p1->currentElevation);

    if (objEventId < OBJECT_EVENTS_COUNT)
        RideAlong(&gObjectEvents[objEventId]); // Hops off once Player 1 stands still
}

// After a warp/map load: Player 2 arrives with Player 1.
void CoopP2_OnMapLoad(void)
{
    if (gCoopLinkActive && CoopP2_GetObject() == NULL)
        SpawnP2();
}

// After returning to the field (continue, end of battle, closing a menu).
void CoopP2_EnsureSpawned(void)
{
    if (gCoopLinkActive && CoopP2_GetObject() == NULL)
        SpawnP2();
}

static u8 GetDpadDirection(u16 keys)
{
    if (keys & DPAD_UP)
        return DIR_NORTH;
    if (keys & DPAD_DOWN)
        return DIR_SOUTH;
    if (keys & DPAD_LEFT)
        return DIR_WEST;
    if (keys & DPAD_RIGHT)
        return DIR_EAST;
    return DIR_NONE;
}

static void SpawnObjectsAroundP2(void)
{
    // Objects are positioned relative to the camera, so only do this while the
    // camera isn't in the middle of scrolling. Player 1's camera movement also
    // spawns objects around Player 2 (the spawn area covers both players).
    if (gFieldCamera.x == 0 && gFieldCamera.y == 0)
    {
        TrySpawnObjectEvents(0, 0);
        RemoveObjectEventsOutsideView();
    }
}

static bool8 sP2Interacting; // The running script was started by Player 2
static bool8 sP2Stepped;     // Player 2's current movement is a step (check for warps when it ends)

// Player 2 walks through a door: open it, step in, close it, then both players warp.
#define tState  data[0]
#define tDoorTask data[1]
#define tDoorX  data[2]
#define tDoorY  data[3]

static void Task_CoopP2DoorWarp(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    struct ObjectEvent *p2 = CoopP2_GetObject();

    switch (task->tState)
    {
    case 0:
        FreezeObjectEvents();
        PlaySE(GetDoorSoundEffect(task->tDoorX, task->tDoorY));
        task->tDoorTask = FieldAnimateDoorOpen(task->tDoorX, task->tDoorY);
        task->tState++;
        break;
    case 1:
        if (task->tDoorTask < 0 || !gTasks[task->tDoorTask].isActive)
        {
            if (p2 != NULL)
            {
                ObjectEventClearHeldMovementIfActive(p2);
                ObjectEventSetHeldMovement(p2, MOVEMENT_ACTION_WALK_NORMAL_UP);
            }
            task->tState++;
        }
        break;
    case 2:
        if (p2 == NULL || ObjectEventClearHeldMovementIfFinished(p2))
        {
            if (p2 != NULL)
                p2->invisible = TRUE;
            task->tDoorTask = FieldAnimateDoorClose(task->tDoorX, task->tDoorY);
            task->tState++;
        }
        break;
    case 3:
        if (task->tDoorTask < 0 || !gTasks[task->tDoorTask].isActive)
        {
            DoWarp(); // Fades out and loads the destination with Player 1 at the door
            DestroyTask(taskId);
        }
        break;
    }
}

#undef tState
#undef tDoorTask
#undef tDoorX
#undef tDoorY

// Facing direction of the player who started the running script.
u8 CoopP2_GetInteractingFacing(void)
{
    struct ObjectEvent *p2 = CoopP2_GetObject();

    if (sP2Interacting && p2 != NULL)
        return p2->facingDirection;
    return GetPlayerFacingDirection();
}

// Player 2 pressed A: talk to whoever / use whatever is in front of them.
static bool8 TryP2Interaction(struct ObjectEvent *p2)
{
    s16 x = p2->currentCoords.x;
    s16 y = p2->currentCoords.y;
    u8 dir = p2->facingDirection;

    MoveCoords(dir, &x, &y);
    if (!Coop_TryStartInteractionAt(x, y, p2->currentElevation, dir))
    {
        if (!CanP2StartSurfing(p2, x, y, dir))
            return FALSE;
        ScriptContext_SetupScript(EventScript_CoopP2UseSurf);
    }
    sP2Interacting = TRUE;
    gCoopInputOwner = COOP_PLAYER_2; // Player 2 answers this conversation
    return TRUE;
}

static void TryStartP2SurfStep(struct ObjectEvent *p2, u8 dir)
{
    struct ObjectEvent *p1 = GetPlayer1();
    s16 x = p2->currentCoords.x;
    s16 y = p2->currentCoords.y;
    u8 collision;

    MoveCoords(dir, &x, &y);
    if (IsWithinTether(x, y, p1->currentCoords.x, p1->currentCoords.y))
    {
        collision = GetCollisionAtCoords(p2, x, y, dir);
        if (collision == COLLISION_NONE && IsSurfableAt(x, y))
        {
            // Surfing is as fast as running.
            ObjectEventSetHeldMovement(p2, GetWalkFastMovementAction(dir));
            SpawnObjectsAroundP2();
            return;
        }
        if (collision == COLLISION_ELEVATION_MISMATCH && !IsSurfableAt(x, y)
         && MapGridGetElevationAt(x, y) == 3 && !IsAnyObjectAt(p2, x, y))
        {
            // Hop off onto land (the surfing sprite is swapped once the jump ends).
            u8 blobId = FindSurfBlob(GetObjectId(p2));
            if (blobId != MAX_SPRITES)
                SetSurfBlob_BobState(blobId, BOB_JUST_MON);
            ObjectEventSetHeldMovement(p2, GetJumpSpecialMovementAction(dir));
            SpawnObjectsAroundP2();
            return;
        }
    }

    if (p2->facingDirection != dir)
        ObjectEventSetHeldMovement(p2, GetFaceDirectionMovementAction(dir));
}

static void TryStartP2Step(struct ObjectEvent *p2)
{
    struct ObjectEvent *p1 = GetPlayer1();
    u16 keys = CoopLink_GetPlayerKeys(COOP_PLAYER_2);
    u8 dir = GetDpadDirection(keys);
    s16 x, y;

    if (dir == DIR_NONE)
        return;

    if (IsP2Surfing(p2))
    {
        TryStartP2SurfStep(p2, dir);
        return;
    }

    // Exit mats and other arrow warps: pushing toward them takes both players out.
    if (Coop_TryStepWarpAt(p2->currentCoords.x, p2->currentCoords.y, p2->currentElevation, dir, TRUE))
        return;

    x = p2->currentCoords.x;
    y = p2->currentCoords.y;
    MoveCoords(dir, &x, &y);

    // Doors: Player 2 walks in and Player 1 comes along.
    if (Coop_TrySetupDoorWarpAt(x, y, p2->currentElevation, dir))
    {
        u8 taskId = CreateTask(Task_CoopP2DoorWarp, 10);
        gTasks[taskId].data[2] = x;
        gTasks[taskId].data[3] = y;
        LockPlayerFieldControls();
        gFieldCallback = FieldCB_DefaultWarpExit;
        return;
    }

    if (GetLedgeJumpDirection(x, y, dir) != DIR_NONE)
    {
        s16 landX = x, landY = y;
        MoveCoords(dir, &landX, &landY);
        if (IsWithinTether(landX, landY, p1->currentCoords.x, p1->currentCoords.y) && !IsAnyObjectAt(p2, landX, landY))
        {
            ObjectEventSetHeldMovement(p2, GetJump2MovementAction(dir));
            sP2Stepped = TRUE;
            SpawnObjectsAroundP2();
            return;
        }
    }
    else if (GetCollisionAtCoords(p2, x, y, dir) == COLLISION_NONE
          && IsWithinTether(x, y, p1->currentCoords.x, p1->currentCoords.y)
          && !MetatileBehavior_IsSurfableWaterOrUnderwater(MapGridGetMetatileBehaviorAt(x, y)))
    {
        if ((keys & B_BUTTON) && FlagGet(FLAG_SYS_B_DASH))
            ObjectEventSetHeldMovement(p2, GetWalkFastMovementAction(dir));
        else
            ObjectEventSetHeldMovement(p2, GetWalkNormalMovementAction(dir));
        sP2Stepped = TRUE;
        SpawnObjectsAroundP2();
        return;
    }

    // Blocked: just turn to face that way.
    if (p2->facingDirection != dir)
        ObjectEventSetHeldMovement(p2, GetFaceDirectionMovementAction(dir));
}

// Runs once per simulated frame in the overworld (CB1_Overworld).
void CoopP2_Update(void)
{
    struct ObjectEvent *p1 = GetPlayer1();
    struct ObjectEvent *p2;

    if (!gCoopLinkActive)
        return;

    p2 = CoopP2_GetObject();
    if (p2 == NULL)
    {
        CoopP2_EnsureSpawned();
        return;
    }

    if (FuncIsActiveTask(Task_CoopP2DoorWarp) || FuncIsActiveTask(Task_CoopP2Surf))
        return; // Walking through a door, jumping onto the water

    if (MustRideAlong())
    {
        sP2Stepped = FALSE;
        RideAlong(p2);
        return;
    }

    if (p2->invisible)
    {
        sP2Stepped = FALSE;
        // Riding along: hop off once Player 1 stands still with free controls.
        if (!ArePlayerFieldControlsLocked() && gPlayerAvatar.tileTransitionState == T_NOT_MOVING)
            PlaceP2NearP1(p2);
        else
            RideAlong(p2);
        return;
    }

    // Still in the middle of a step?
    if (ObjectEventIsHeldMovementActive(p2) && !ObjectEventClearHeldMovementIfFinished(p2))
        return;

    // Surfing follows the tile Player 2 is on: hopping off onto land ends it,
    // and the surf blob comes back after battles and menus.
    if (IsP2Surfing(p2))
    {
        if (!IsSurfableAt(p2->currentCoords.x, p2->currentCoords.y))
            StopP2SurfState(p2);
        else
            StartP2SurfState(p2);
    }

    // Player 1 got moved too far away (cutscene, forced movement...): catch up.
    if (!IsWithinTether(p2->currentCoords.x, p2->currentCoords.y, p1->currentCoords.x, p1->currentCoords.y))
    {
        PlaceP2NearP1(p2);
        return;
    }

    if (ArePlayerFieldControlsLocked())
        return; // Menus and cutscenes: Player 2 waits

    // Just stepped onto stairs, a ladder, a cave entrance...: both players warp.
    if (sP2Stepped)
    {
        sP2Stepped = FALSE;
        if (Coop_TryStepWarpAt(p2->currentCoords.x, p2->currentCoords.y, p2->currentElevation, p2->facingDirection, FALSE))
            return;
    }

    sP2Interacting = FALSE;
    if ((CoopLink_GetPlayerNewKeys(COOP_PLAYER_2) & A_BUTTON) && TryP2Interaction(p2))
        return;
    TryStartP2Step(p2);
}
