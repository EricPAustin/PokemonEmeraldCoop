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

// Linked co-op: Player 2's character in the overworld.
//
// Player 1 is the game's normal player character: it triggers doors, scripts,
// wild encounters and trainers. Player 2 is an extra object event that walks
// around with Player 2's buttons, within COOP_TETHER_X/Y tiles of Player 1.
// While Player 1 surfs, bikes or dives, and right after map changes, Player 2
// "rides along" (hidden on Player 1's tile) and hops off next to Player 1 as
// soon as Player 1 is standing still on foot.
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
    return (gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER
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

static void RideAlong(struct ObjectEvent *p2)
{
    struct ObjectEvent *p1 = GetPlayer1();

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
    s32 i;

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
            MoveObjectEventToMapCoords(p2, x, y);
            ObjectEventUpdateElevation(p2);
            p2->invisible = FALSE;
            ObjectEventTurn(p2, facing);
            return;
        }
    }
    RideAlong(p2);
}

static void SpawnP2(void)
{
    struct ObjectEvent *p1 = GetPlayer1();
    u8 graphicsId = GetRivalAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_NORMAL, gSaveBlock2Ptr->playerGender ^ 1);
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
        return FALSE;
    sP2Interacting = TRUE;
    gCoopInputOwner = COOP_PLAYER_2; // Player 2 answers this conversation
    return TRUE;
}

static void TryStartP2Step(struct ObjectEvent *p2)
{
    struct ObjectEvent *p1 = GetPlayer1();
    u16 keys = CoopLink_GetPlayerKeys(COOP_PLAYER_2);
    u8 dir = GetDpadDirection(keys);
    s16 x, y;

    if (dir == DIR_NONE)
        return;

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

    if (FuncIsActiveTask(Task_CoopP2DoorWarp))
        return; // Walking through a door

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
