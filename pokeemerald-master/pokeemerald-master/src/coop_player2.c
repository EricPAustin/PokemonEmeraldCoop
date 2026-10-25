#include "global.h"
#include "coop.h"
#include "coop_link.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_camera.h"
#include "field_control_avatar.h"
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
#include "constants/map_types.h"

// Linked co-op: Player 2's character in the overworld.
//
// Player 1 is the game's normal player character: it triggers doors, scripts,
// wild encounters and trainers. Player 2 is an extra object event that walks
// around with Player 2's buttons, within COOP_TETHER_X/Y tiles of Player 1.
// While Player 1 bikes, and right after map changes, Player 2 "rides along"
// (hidden on Player 1's tile) and hops off next to Player 1 as soon as
// Player 1 is standing still.
//
// Surfing: either player can start surfing. Whoever starts brings the other
// along: the other player appears on the water next to them, surfing too.
// After that each player surfs and lands on their own.
//
// Waterfall: either player can climb a waterfall, and whoever climbs brings
// the other along to the top. Either player can also surf down one; if that
// leaves the other player too far behind, they come along to the bottom.
//
// Dive: either player can dive and surface, and both players go together.
// Underwater, each player swims on their own.
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
    return (gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE)) != 0;
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

// Can Player 2 be placed on this underwater tile?
static bool8 IsGoodUnderwaterSpotForP2(const struct ObjectEvent *p2, s16 x, s16 y)
{
    if (MapGridGetCollisionAt(x, y) || GetMapBorderIdAt(x, y) == CONNECTION_INVALID)
        return FALSE;
    if (Coop_IsWarpTile(MapGridGetMetatileBehaviorAt(x, y)))
        return FALSE;
    return !IsAnyObjectAt(p2, x, y);
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

// Underwater, Player 2's sprite bobs up and down (like Player 1's, see
// StartUnderwaterSurfBlobBobbing) through this invisible helper sprite.
#define sObjId data[0]
#define sBobY  data[1]
#define sTimer data[2]

static void SpriteCB_P2UnderwaterBob(struct Sprite *sprite)
{
    struct ObjectEvent *p2 = &gObjectEvents[sprite->sObjId];

    if (!p2->active)
    {
        DestroySprite(sprite);
        return;
    }
    if (((sprite->sTimer++) & 3) == 0)
        gSprites[p2->spriteId].y2 += sprite->sBobY;
    if ((sprite->sTimer & 15) == 0)
        sprite->sBobY = -sprite->sBobY;
}

static u8 FindUnderwaterBob(u8 objectEventId)
{
    s32 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        if (gSprites[i].inUse && gSprites[i].callback == SpriteCB_P2UnderwaterBob
         && gSprites[i].sObjId == objectEventId)
            return i;
    }
    return MAX_SPRITES;
}

static void CreateUnderwaterBob(u8 objectEventId)
{
    u8 spriteId = CreateSpriteAtEnd(&gDummySpriteTemplate, 0, 0, -1);

    if (spriteId == MAX_SPRITES)
        return;
    gSprites[spriteId].callback = SpriteCB_P2UnderwaterBob;
    gSprites[spriteId].invisible = TRUE;
    gSprites[spriteId].sObjId = objectEventId;
    gSprites[spriteId].sBobY = 1;
}

#undef sObjId
#undef sBobY
#undef sTimer

static bool8 IsUnderwaterMap(void)
{
    return gMapHeader.mapType == MAP_TYPE_UNDERWATER;
}

static bool8 IsP2Underwater(const struct ObjectEvent *p2)
{
    return p2->graphicsId == GetP2GraphicsId(PLAYER_AVATAR_STATE_UNDERWATER);
}

// Instantly puts Player 2 underwater (sprite and bobbing).
static void StartP2UnderwaterState(struct ObjectEvent *p2)
{
    if (!IsP2Underwater(p2))
    {
        ObjectEventSetGraphicsId(p2, GetP2GraphicsId(PLAYER_AVATAR_STATE_UNDERWATER));
        ObjectEventTurn(p2, p2->facingDirection);
        gSprites[p2->spriteId].y2 = 0;
    }
    if (FindUnderwaterBob(GetObjectId(p2)) == MAX_SPRITES)
        CreateUnderwaterBob(GetObjectId(p2));
}

// Surfing on the surface, swimming on an underwater map.
static void StartP2WaterState(struct ObjectEvent *p2)
{
    if (IsUnderwaterMap())
        StartP2UnderwaterState(p2);
    else
        StartP2SurfState(p2);
}

// Instantly takes Player 2 off the water (or out of it).
static void StopP2SurfState(struct ObjectEvent *p2)
{
    u8 blobId = FindSurfBlob(GetObjectId(p2));

    if (blobId != MAX_SPRITES)
        DestroySprite(&gSprites[blobId]);
    blobId = FindUnderwaterBob(GetObjectId(p2));
    if (blobId != MAX_SPRITES)
        DestroySprite(&gSprites[blobId]);
    if (IsP2Surfing(p2) || IsP2Underwater(p2))
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
        StartP2WaterState(p2);
    else
        StopP2SurfState(p2);
    ObjectEventTurn(p2, facing);
}

// Player 1 appears on the water next to Player 2, surfing too (Player 2 started
// surfing while Player 1 was on land, or climbed or surfed down a waterfall).
// The camera jumps with Player 1.
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
    if (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_SURFING)
    {
        // Already surfing: the surf blob follows Player 1 by itself.
        ObjectEventTurn(p1, p2->facingDirection);
        return TRUE;
    }
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

// Player 1 just climbed a waterfall (vanilla Waterfall field effect finished):
// if Player 2 is surfing below, Player 2 comes along to the top.
void CoopP2_OnPlayer1UsedWaterfall(void)
{
    struct ObjectEvent *p1 = GetPlayer1();
    struct ObjectEvent *p2 = CoopP2_GetObject();
    s16 x, y;

    if (p2 == NULL || p2->invisible || !IsP2Surfing(p2) || p2->currentCoords.y <= p1->currentCoords.y)
        return;
    if (FindSurfSpotNear(p2, p1->currentCoords.x, p1->currentCoords.y, p1->facingDirection, FALSE, &x, &y))
        PlaceP2At(p2, x, y, p1->facingDirection, TRUE);
}

// Index of the party Pokémon that uses a field move for Player 2 (Player 2's
// own first), or PARTY_SIZE.
static u8 FindP2MonWithMove(u16 move)
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
            if (MonKnowsMove(mon, move))
                return i;
        }
    }
    return PARTY_SIZE;
}

static u8 FindP2SurfMon(void)
{
    return FindP2MonWithMove(MOVE_SURF);
}

void CoopP2_FindSurfMon(void)
{
    gSpecialVar_Result = FindP2SurfMon();
}

void CoopP2_FindWaterfallMon(void)
{
    gSpecialVar_Result = FindP2MonWithMove(MOVE_WATERFALL);
}

void CoopP2_FindDiveMon(void)
{
    gSpecialVar_Result = FindP2MonWithMove(MOVE_DIVE);
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

// Player 2 uses Waterfall (special from EventScript_CoopP2UseWaterfall, followed
// by waitstate): rides up while on waterfall tiles, then Player 1 comes along
// unless Player 1 is already above the waterfall.
#define tStartY data[2]

static void Task_CoopP2Waterfall(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    struct ObjectEvent *p2 = CoopP2_GetObject();

    if (p2 == NULL)
    {
        ScriptContext_Enable();
        DestroyTask(taskId);
        return;
    }

    switch (task->tState)
    {
    case 0:
        task->tStartY = p2->currentCoords.y;
        gFieldEffectArguments[0] = task->tMonId | SHOW_MON_CRY_NO_DUCKING;
        FieldEffectStart(FLDEFF_FIELD_MOVE_SHOW_MON_INIT);
        task->tState++;
        break;
    case 1:
        if (!FieldEffectActiveListContains(FLDEFF_FIELD_MOVE_SHOW_MON))
        {
            UnfreezeObjectEvent(p2);
            ObjectEventClearHeldMovementIfActive(p2);
            task->tState++;
        }
        break;
    case 2:
        ObjectEventSetHeldMovement(p2, GetWalkSlowMovementAction(DIR_NORTH));
        task->tState++;
        break;
    case 3:
        if (ObjectEventClearHeldMovementIfFinished(p2))
        {
            if (MetatileBehavior_IsWaterfall(p2->currentMetatileBehavior))
            {
                task->tState = 2; // Still climbing
                break;
            }
            if (GetPlayer1()->currentCoords.y >= task->tStartY)
                TryTeleportP1ToSurfNextTo(p2);
            ScriptContext_Enable();
            DestroyTask(taskId);
        }
        break;
    }
}

void CoopP2_StartWaterfall(void)
{
    u8 taskId = CreateTask(Task_CoopP2Waterfall, 0xFF);
    gTasks[taskId].tMonId = gFieldEffectArguments[0];
}

#undef tStartY

// Player 2 uses Dive, down or up (special from EventScript_CoopP2UseDive or
// EventScript_CoopP2UseDiveUnderwater, followed by waitstate). Both players
// warp to Player 2's spot on the other map; Player 2 arrives with Player 1.
static void Task_CoopP2Dive(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    struct ObjectEvent *p2 = CoopP2_GetObject();
    struct MapPosition position;

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
            if (p2 != NULL)
            {
                position.x = p2->currentCoords.x;
                position.y = p2->currentCoords.y;
                position.elevation = p2->currentElevation;
                TryDoDiveWarp(&position, MapGridGetMetatileBehaviorAt(position.x, position.y));
            }
            ScriptContext_Enable();
            DestroyTask(taskId);
        }
        break;
    }
}

void CoopP2_StartDive(void)
{
    u8 taskId = CreateTask(Task_CoopP2Dive, 0xFF);
    gTasks[taskId].tMonId = gFieldEffectArguments[0];
}

#undef tState
#undef tMonId

// Player 2 pressed A facing a waterfall while surfing.
static const u8 *GetP2WaterfallScript(const struct ObjectEvent *p2, s16 x, s16 y, u8 dir)
{
    if (!IsP2Surfing(p2) || !MetatileBehavior_IsWaterfall(MapGridGetMetatileBehaviorAt(x, y)))
        return NULL;
    if (!FlagGet(FLAG_BADGE08_GET) || dir != DIR_NORTH)
        return EventScript_CannotUseWaterfall;
    return EventScript_CoopP2UseWaterfall;
}

// Player 2 pressed A while surfing on deep water.
static bool8 CanP2Dive(const struct ObjectEvent *p2)
{
    s16 x = p2->currentCoords.x;
    s16 y = p2->currentCoords.y;

    if (!IsP2Surfing(p2) || !FlagGet(FLAG_BADGE07_GET) || IsUnderwaterMap())
        return FALSE;
    if (!MetatileBehavior_IsDiveable(MapGridGetMetatileBehaviorAt(x, y)))
        return FALSE;
    return SetDiveWarpDive(x - MAP_OFFSET, y - MAP_OFFSET);
}

// Player 2 pressed B underwater.
static bool8 CanP2Surface(const struct ObjectEvent *p2)
{
    s16 x = p2->currentCoords.x;
    s16 y = p2->currentCoords.y;

    if (!IsP2Underwater(p2) || !FlagGet(FLAG_BADGE07_GET) || !IsUnderwaterMap())
        return FALSE;
    if (MetatileBehavior_IsUnableToEmerge(MapGridGetMetatileBehaviorAt(x, y)))
        return FALSE;
    return SetDiveWarpEmerge(x - MAP_OFFSET, y - MAP_OFFSET);
}

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
        if (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_UNDERWATER)
        {
            // Underwater: swim alongside.
            if (IsGoodUnderwaterSpotForP2(p2, x, y))
            {
                PlaceP2At(p2, x, y, facing, TRUE);
                return;
            }
        }
        else if (IsGoodSpotForP2(p2, x, y, p1->currentElevation))
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
static bool8 sP2EncounterStep; // Player 2's current movement is a step (check for wild Pokémon when it ends)
static bool8 sP2Descending;  // Player 2 is surfing down a waterfall

// Where the wild Pokémon was found, when Player 2 found it (for the battle
// background and transition). Cleared when the battle starts.
static EWRAM_DATA bool8 sEncounterPosSet = FALSE;
static EWRAM_DATA s16 sEncounterX = 0;
static EWRAM_DATA s16 sEncounterY = 0;
static EWRAM_DATA bool8 sEncounterSurfing = FALSE;

bool8 CoopP2_GetEncounterPos(s16 *x, s16 *y, bool8 *surfing)
{
    if (!sEncounterPosSet)
        return FALSE;
    *x = sEncounterX;
    *y = sEncounterY;
    *surfing = sEncounterSurfing;
    return TRUE;
}

void CoopP2_ClearEncounterPos(void)
{
    sEncounterPosSet = FALSE;
}

// Player 2 just finished a step: maybe a wild Pokémon appears.
static bool8 TryP2WildEncounter(struct ObjectEvent *p2)
{
    sEncounterPosSet = TRUE;
    sEncounterX = p2->currentCoords.x;
    sEncounterY = p2->currentCoords.y;
    sEncounterSurfing = IsP2Surfing(p2);
    if (Coop_TryWildEncounterAt(p2->currentCoords.x, p2->currentCoords.y))
        return TRUE;
    sEncounterPosSet = FALSE;
    return FALSE;
}

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
        const u8 *script = GetP2WaterfallScript(p2, x, y, dir);

        if (script != NULL)
            ScriptContext_SetupScript(script);
        else if (CanP2StartSurfing(p2, x, y, dir))
            ScriptContext_SetupScript(EventScript_CoopP2UseSurf);
        else if (CanP2Dive(p2))
            ScriptContext_SetupScript(EventScript_CoopP2UseDive);
        else
            return FALSE;
    }
    sP2Interacting = TRUE;
    gCoopInputOwner = COOP_PLAYER_2; // Player 2 answers this conversation
    return TRUE;
}

// Player 2 pressed B underwater: surface (both players go up).
static bool8 TryP2Surface(struct ObjectEvent *p2)
{
    if (!CanP2Surface(p2))
        return FALSE;
    ScriptContext_SetupScript(EventScript_CoopP2UseDiveUnderwater);
    sP2Interacting = TRUE;
    gCoopInputOwner = COOP_PLAYER_2;
    return TRUE;
}

static void TryStartP2UnderwaterStep(struct ObjectEvent *p2, u8 dir)
{
    struct ObjectEvent *p1 = GetPlayer1();
    s16 x = p2->currentCoords.x;
    s16 y = p2->currentCoords.y;

    MoveCoords(dir, &x, &y);
    if (IsWithinTether(x, y, p1->currentCoords.x, p1->currentCoords.y)
     && GetCollisionAtCoords(p2, x, y, dir) == COLLISION_NONE)
    {
        ObjectEventSetHeldMovement(p2, GetWalkNormalMovementAction(dir));
        sP2Stepped = TRUE;
        sP2EncounterStep = TRUE;
        SpawnObjectsAroundP2();
        return;
    }
    if (p2->facingDirection != dir)
        ObjectEventSetHeldMovement(p2, GetFaceDirectionMovementAction(dir));
}

static void TryStartP2SurfStep(struct ObjectEvent *p2, u8 dir)
{
    struct ObjectEvent *p1 = GetPlayer1();
    s16 x = p2->currentCoords.x;
    s16 y = p2->currentCoords.y;
    u8 collision;

    MoveCoords(dir, &x, &y);
    // Surfing down a waterfall (Player 1 comes along if left too far behind).
    if (dir == DIR_SOUTH && MetatileBehavior_IsWaterfall(MapGridGetMetatileBehaviorAt(x, y))
     && GetCollisionAtCoords(p2, x, y, dir) == COLLISION_NONE)
    {
        ObjectEventSetHeldMovement(p2, GetWalkFastMovementAction(dir));
        sP2Descending = TRUE;
        SpawnObjectsAroundP2();
        return;
    }
    if (IsWithinTether(x, y, p1->currentCoords.x, p1->currentCoords.y))
    {
        collision = GetCollisionAtCoords(p2, x, y, dir);
        if (collision == COLLISION_NONE && IsSurfableAt(x, y))
        {
            // Surfing is as fast as running.
            ObjectEventSetHeldMovement(p2, GetWalkFastMovementAction(dir));
            sP2EncounterStep = TRUE;
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
    if (IsP2Underwater(p2))
    {
        TryStartP2UnderwaterStep(p2, dir);
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
            sP2EncounterStep = TRUE;
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
        sP2EncounterStep = TRUE;
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

    if (FuncIsActiveTask(Task_CoopP2DoorWarp) || FuncIsActiveTask(Task_CoopP2Surf)
     || FuncIsActiveTask(Task_CoopP2Waterfall) || FuncIsActiveTask(Task_CoopP2Dive))
        return; // Walking through a door, jumping onto the water, climbing a waterfall, diving

    if (MustRideAlong())
    {
        sP2Stepped = FALSE;
        sP2EncounterStep = FALSE;
        sP2Descending = FALSE;
        RideAlong(p2);
        return;
    }

    if (p2->invisible)
    {
        sP2Stepped = FALSE;
        sP2EncounterStep = FALSE;
        sP2Descending = FALSE;
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

    // Surfing down a waterfall: keep going until the bottom, then bring
    // Player 1 along if they're too far away now.
    if (IsP2Surfing(p2) && MetatileBehavior_IsWaterfall(MapGridGetMetatileBehaviorAt(p2->currentCoords.x, p2->currentCoords.y)))
        sP2Descending = TRUE;
    if (sP2Descending)
    {
        if (IsP2Surfing(p2) && MetatileBehavior_IsWaterfall(MapGridGetMetatileBehaviorAt(p2->currentCoords.x, p2->currentCoords.y)))
        {
            ObjectEventSetHeldMovement(p2, GetWalkFastMovementAction(DIR_SOUTH));
            SpawnObjectsAroundP2();
            return;
        }
        if (!IsWithinTether(p2->currentCoords.x, p2->currentCoords.y, p1->currentCoords.x, p1->currentCoords.y))
        {
            // Wait for Player 1 to stand still first.
            if (gPlayerAvatar.tileTransitionState != T_NOT_MOVING || gFieldCamera.x != 0 || gFieldCamera.y != 0
             || ArePlayerFieldControlsLocked())
                return;
            TryTeleportP1ToSurfNextTo(p2);
        }
        sP2Descending = FALSE;
    }

    // Surfing follows the tile Player 2 is on: hopping off onto land ends it,
    // and the surf blob comes back after battles and menus. The same goes for
    // swimming underwater.
    if (IsP2Surfing(p2))
    {
        if (!IsSurfableAt(p2->currentCoords.x, p2->currentCoords.y))
            StopP2SurfState(p2);
        else
            StartP2SurfState(p2);
    }
    else if (IsP2Underwater(p2))
    {
        StartP2UnderwaterState(p2);
    }

    // Player 1 got moved too far away (cutscene, forced movement...): catch up.
    if (!IsWithinTether(p2->currentCoords.x, p2->currentCoords.y, p1->currentCoords.x, p1->currentCoords.y))
    {
        PlaceP2NearP1(p2);
        return;
    }

    if (ArePlayerFieldControlsLocked())
    {
        sP2EncounterStep = FALSE;
        return; // Menus and cutscenes: Player 2 waits
    }

    // Just stepped onto stairs, a ladder, a cave entrance...: both players warp.
    if (sP2Stepped)
    {
        sP2Stepped = FALSE;
        if (Coop_TryStepWarpAt(p2->currentCoords.x, p2->currentCoords.y, p2->currentElevation, p2->facingDirection, FALSE))
            return;
    }

    // Wild Pokémon (land or water, like Player 1's steps).
    if (sP2EncounterStep)
    {
        sP2EncounterStep = FALSE;
        if (TryP2WildEncounter(p2))
            return;
    }

    sP2Interacting = FALSE;
    if ((CoopLink_GetPlayerNewKeys(COOP_PLAYER_2) & B_BUTTON) && TryP2Surface(p2))
        return;
    if ((CoopLink_GetPlayerNewKeys(COOP_PLAYER_2) & A_BUTTON) && TryP2Interaction(p2))
        return;
    TryStartP2Step(p2);
}
