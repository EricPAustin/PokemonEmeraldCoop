#include "global.h"
#include "coop_link.h"
#include "coop.h"
#include "berry.h"
#include "bike.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "event_object_movement.h"
#include "gpu_regs.h"
#include "menu.h"
#include "overworld.h"
#include "rotating_gate.h"
#include "sprite.h"
#include "text.h"

EWRAM_DATA bool8 gUnusedBikeCameraAheadPanback = FALSE;

struct FieldCameraOffset
{
    u8 xPixelOffset;
    u8 yPixelOffset;
    u8 xTileOffset;
    u8 yTileOffset;
    bool8 copyBGToVRAM;
};

static void RedrawMapSliceNorth(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceSouth(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceEast(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceWest(struct FieldCameraOffset *, const struct MapLayout *);
static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *, s32, s32);
static void DrawWholeMapViewInternal(int, int, const struct MapLayout *);
static void DrawMetatileAt(const struct MapLayout *, u16, int, int);
static void DrawMetatile(s32, const u16 *, u16);
static void CameraPanningCB_PanAhead(void);

static struct FieldCameraOffset sFieldCameraOffset;
static s16 sHorizontalCameraPan;
static s16 sVerticalCameraPan;
static bool8 sBikeCameraPanFlag;
static void (*sFieldCameraPanningCallback)(void);

COMMON_DATA struct CameraObject gFieldCamera = {0};
COMMON_DATA u16 gTotalCameraPixelOffsetY = 0;
COMMON_DATA u16 gTotalCameraPixelOffsetX = 0;

// Linked co-op: on Player 2's GBA the screen follows Player 2.
//
// The game logic (gSaveBlock1Ptr->pos, map connections, which objects exist)
// still follows Player 1's camera on both GBAs. Player 2's GBA only changes what
// is drawn: the BG tilemap ring buffer holds the area around Player 2, and the
// BG scroll and sprite offsets are shifted by the pixel distance between the
// two players. Every map tile always goes to the same buffer cell as in the
// original game, so the two views share the same buffer layout.
static bool8 sCoopViewOn;
static s16 sCoopViewDeltaX;
static s16 sCoopViewDeltaY;
static s16 sCoopDrawnOriginX;
static s16 sCoopDrawnOriginY;

static bool8 IsCoopPlayer2Screen(void)
{
    return COOP_LINKED && gCoopLinkActive && gCoopLocalPlayer == COOP_PLAYER_2;
}

static struct ObjectEvent *GetCoopViewTarget(void)
{
    struct ObjectEvent *p2;
    u8 player1SpriteId;

    if (!IsCoopPlayer2Screen())
        return NULL;
    p2 = CoopP2_GetObject();
    if (p2 == NULL || p2->invisible)
        return NULL;
    // Only while the camera follows Player 1 (cutscenes with their own camera
    // show the shared view).
    player1SpriteId = gObjectEvents[gPlayerAvatar.objectEventId].spriteId;
    if (gFieldCamera.spriteId == 0 || gSprites[gFieldCamera.spriteId].sCamera_FollowSpriteId != player1SpriteId)
        return NULL;
    return p2;
}

// Top-left metatile of the area drawn into the BG buffers.
static void GetCoopDrawOrigin(s16 *x, s16 *y)
{
    struct ObjectEvent *target = GetCoopViewTarget();

    if (target != NULL)
    {
        *x = target->currentCoords.x - MAP_OFFSET;
        *y = target->currentCoords.y - MAP_OFFSET;
    }
    else
    {
        *x = gSaveBlock1Ptr->pos.x;
        *y = gSaveBlock1Ptr->pos.y;
    }
}

// The buffer cell (in tiles, 0-31) that a map coordinate is drawn to.
static u8 GetBufferCell(u8 tileOffset, s32 coord, s32 cameraCoord)
{
    s32 cell = (tileOffset + 2 * (coord - cameraCoord)) % 32;
    if (cell < 0)
        cell += 32;
    return cell;
}

bool8 CoopView_IsLocalViewOffset(void)
{
    return sCoopViewOn;
}

static void ResetCameraOffset(struct FieldCameraOffset *cameraOffset)
{
    cameraOffset->xTileOffset = 0;
    cameraOffset->yTileOffset = 0;
    cameraOffset->xPixelOffset = 0;
    cameraOffset->yPixelOffset = 0;
    cameraOffset->copyBGToVRAM = TRUE;
}

static void AddCameraTileOffset(struct FieldCameraOffset *cameraOffset, u32 xOffset, u32 yOffset)
{
    cameraOffset->xTileOffset += xOffset;
    cameraOffset->xTileOffset %= 32;
    cameraOffset->yTileOffset += yOffset;
    cameraOffset->yTileOffset %= 32;
}

static void AddCameraPixelOffset(struct FieldCameraOffset *cameraOffset, u32 xOffset, u32 yOffset)
{
    cameraOffset->xPixelOffset += xOffset;
    cameraOffset->yPixelOffset += yOffset;
}

void ResetFieldCamera(void)
{
    ResetCameraOffset(&sFieldCameraOffset);
}

void FieldUpdateBgTilemapScroll(void)
{
    u32 r4, r5;
    r5 = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    r4 = sVerticalCameraPan + sFieldCameraOffset.yPixelOffset + 8;

    if (sCoopViewOn) // Linked co-op: Player 2's screen
    {
        r5 += sCoopViewDeltaX;
        r4 += sCoopViewDeltaY;
    }

    SetGpuReg(REG_OFFSET_BG1HOFS, r5);
    SetGpuReg(REG_OFFSET_BG1VOFS, r4);
    SetGpuReg(REG_OFFSET_BG2HOFS, r5);
    SetGpuReg(REG_OFFSET_BG2VOFS, r4);
    SetGpuReg(REG_OFFSET_BG3HOFS, r5);
    SetGpuReg(REG_OFFSET_BG3VOFS, r4);
}

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    *y = sFieldCameraOffset.yPixelOffset + sVerticalCameraPan + 8;
    if (sCoopViewOn)
    {
        *x += sCoopViewDeltaX;
        *y += sCoopViewDeltaY;
    }
}

void DrawWholeMapView(void)
{
    s16 x, y;

    GetCoopDrawOrigin(&x, &y);
    sCoopDrawnOriginX = x;
    sCoopDrawnOriginY = y;
    DrawWholeMapViewInternal(x, y, gMapHeader.mapLayout);
    sFieldCameraOffset.copyBGToVRAM = TRUE;
}

static void DrawWholeMapViewInternal(int x, int y, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 j;
    u32 r6;
    u8 temp;
    // In the original game x/y is always the camera position, so these are
    // just the camera's tile offsets.
    u8 baseX = GetBufferCell(sFieldCameraOffset.xTileOffset, x, gSaveBlock1Ptr->pos.x);
    u8 baseY = GetBufferCell(sFieldCameraOffset.yTileOffset, y, gSaveBlock1Ptr->pos.y);

    for (i = 0; i < 32; i += 2)
    {
        temp = baseY + i;
        if (temp >= 32)
            temp -= 32;
        r6 = temp * 32;
        for (j = 0; j < 32; j += 2)
        {
            temp = baseX + j;
            if (temp >= 32)
                temp -= 32;
            DrawMetatileAt(mapLayout, r6 + temp, x + j / 2, y + i / 2);
        }
    }
}

// Linked co-op: draws one map tile into its buffer cell.
static void CoopDrawMapTile(s32 x, s32 y)
{
    u8 cellX = GetBufferCell(sFieldCameraOffset.xTileOffset, x, gSaveBlock1Ptr->pos.x);
    u8 cellY = GetBufferCell(sFieldCameraOffset.yTileOffset, y, gSaveBlock1Ptr->pos.y);
    DrawMetatileAt(gMapHeader.mapLayout, cellY * 32 + cellX, x, y);
}

// Linked co-op, Player 2's screen: keeps the BG buffer and scroll following
// Player 2. Called every frame after the camera update.
void CoopView_Update(void)
{
    struct ObjectEvent *target;
    s16 x, y, dx, dy;
    s32 i;

    sCoopViewOn = FALSE;
    if (!IsCoopPlayer2Screen())
        return;

    GetCoopDrawOrigin(&x, &y);
    dx = x - sCoopDrawnOriginX;
    dy = y - sCoopDrawnOriginY;
    if (dx != 0 || dy != 0)
    {
        sCoopDrawnOriginX = x;
        sCoopDrawnOriginY = y;
        if ((dx == 0 || dy == 0) && dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
        {
            // Same slices the original camera draws when it moves one tile.
            for (i = 0; i < 16; i++)
            {
                if (dx > 0)
                    CoopDrawMapTile(x + 14, y + i);
                else if (dx < 0)
                    CoopDrawMapTile(x, y + i);
                else if (dy > 0)
                    CoopDrawMapTile(x + i, y + 14);
                else
                    CoopDrawMapTile(x + i, y);
            }
        }
        else
        {
            DrawWholeMapViewInternal(x, y, gMapHeader.mapLayout);
        }
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }

    target = GetCoopViewTarget();
    if (target != NULL)
    {
        struct Sprite *p1Sprite = &gSprites[gObjectEvents[gPlayerAvatar.objectEventId].spriteId];
        struct Sprite *p2Sprite = &gSprites[target->spriteId];
        sCoopViewDeltaX = p2Sprite->x - p1Sprite->x;
        sCoopViewDeltaY = p2Sprite->y - p1Sprite->y;
        sCoopViewOn = TRUE;
        gSpriteCoordOffsetX -= sCoopViewDeltaX;
        gSpriteCoordOffsetY -= sCoopViewDeltaY;
    }
    else
    {
        sCoopViewDeltaX = 0;
        sCoopViewDeltaY = 0;
    }
}

static void RedrawMapSlicesForCameraUpdate(struct FieldCameraOffset *cameraOffset, int x, int y)
{
    const struct MapLayout *mapLayout = gMapHeader.mapLayout;

    if (x > 0)
        RedrawMapSliceWest(cameraOffset, mapLayout);
    if (x < 0)
        RedrawMapSliceEast(cameraOffset, mapLayout);
    if (y > 0)
        RedrawMapSliceNorth(cameraOffset, mapLayout);
    if (y < 0)
        RedrawMapSliceSouth(cameraOffset, mapLayout);
    cameraOffset->copyBGToVRAM = TRUE;
}

static void RedrawMapSliceNorth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r7;

    temp = cameraOffset->yTileOffset + 28;
    if (temp >= 32)
        temp -= 32;
    r7 = temp * 32;
    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->xTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, r7 + temp, gSaveBlock1Ptr->pos.x + i / 2, gSaveBlock1Ptr->pos.y + 14);
    }
}

static void RedrawMapSliceSouth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r7 = cameraOffset->yTileOffset * 32;

    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->xTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, r7 + temp, gSaveBlock1Ptr->pos.x + i / 2, gSaveBlock1Ptr->pos.y);
    }
}

static void RedrawMapSliceEast(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r6 = cameraOffset->xTileOffset;

    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, temp * 32 + r6, gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y + i / 2);
    }
}

static void RedrawMapSliceWest(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u8 r5 = cameraOffset->xTileOffset + 28;

    if (r5 >= 32)
        r5 -= 32;
    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, temp * 32 + r5, gSaveBlock1Ptr->pos.x + 14, gSaveBlock1Ptr->pos.y + i / 2);
    }
}

void CurrentMapDrawMetatileAt(int x, int y)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        DrawMetatileAt(gMapHeader.mapLayout, offset, x, y);
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

void DrawDoorMetatileAt(int x, int y, u16 *tiles)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        DrawMetatile(METATILE_LAYER_TYPE_COVERED, tiles, offset);
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

static void DrawMetatileAt(const struct MapLayout *mapLayout, u16 offset, int x, int y)
{
    u16 metatileId = IsCoopPlayer2Screen() ? MapGridGetMetatileIdAtForDrawing(x, y) : MapGridGetMetatileIdAt(x, y);
    const u16 *metatiles;

    if (metatileId > NUM_METATILES_TOTAL)
        metatileId = 0;
    if (metatileId < NUM_METATILES_IN_PRIMARY)
    {
        metatiles = mapLayout->primaryTileset->metatiles;
    }
    else
    {
        metatiles = mapLayout->secondaryTileset->metatiles;
        metatileId -= NUM_METATILES_IN_PRIMARY;
    }
    DrawMetatile(MapGridGetMetatileLayerTypeAt(x, y), metatiles + metatileId * NUM_TILES_PER_METATILE, offset);
}

static void DrawMetatile(s32 metatileLayerType, const u16 *tiles, u16 offset)
{
    switch (metatileLayerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        // Draw metatile's bottom layer to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg3[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = tiles[3];

        // Draw transparent tiles to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 1] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = 0;

        // Draw metatile's top layer to the top background layer.
        gOverworldTilemapBuffer_Bg1[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg1[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = tiles[7];
        break;
    case METATILE_LAYER_TYPE_COVERED:
        // Draw metatile's bottom layer to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg3[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = tiles[3];

        // Draw metatile's top layer to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg2[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = tiles[7];

        // Draw transparent tiles to the top background layer.
        gOverworldTilemapBuffer_Bg1[offset] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 1] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = 0;
        break;
    case METATILE_LAYER_TYPE_NORMAL:
        // Draw garbage to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 1] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = 0x3014;

        // Draw metatile's bottom layer to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg2[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = tiles[3];

        // Draw metatile's top layer to the top background layer, which covers object event sprites.
        gOverworldTilemapBuffer_Bg1[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg1[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = tiles[7];
        break;
    }
    ScheduleBgCopyTilemapToVram(1);
    ScheduleBgCopyTilemapToVram(2);
    ScheduleBgCopyTilemapToVram(3);
}

static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *cameraOffset, s32 x, s32 y)
{
    if (IsCoopPlayer2Screen())
    {
        // Linked co-op: only tiles in the area currently drawn around Player 2.
        if (x < sCoopDrawnOriginX || x >= sCoopDrawnOriginX + 16
         || y < sCoopDrawnOriginY || y >= sCoopDrawnOriginY + 16)
            return -1;
        return GetBufferCell(cameraOffset->yTileOffset, y, gSaveBlock1Ptr->pos.y) * 32
             + GetBufferCell(cameraOffset->xTileOffset, x, gSaveBlock1Ptr->pos.x);
    }

    x -= gSaveBlock1Ptr->pos.x;
    x *= 2;
    if (x >= 32 || x < 0)
        return -1;
    x = x + cameraOffset->xTileOffset;
    if (x >= 32)
        x -= 32;

    y = (y - gSaveBlock1Ptr->pos.y) * 2;
    if (y >= 32 || y < 0)
        return -1;
    y = y + cameraOffset->yTileOffset;
    if (y >= 32)
        y -= 32;

    return y * 32 + x;
}

static void CameraUpdateCallback(struct CameraObject *fieldCamera)
{
    if (fieldCamera->spriteId != 0)
    {
        fieldCamera->movementSpeedX = gSprites[fieldCamera->spriteId].sCamera_MoveX;
        fieldCamera->movementSpeedY = gSprites[fieldCamera->spriteId].sCamera_MoveY;
    }
}

void ResetCameraUpdateInfo(void)
{
    gFieldCamera.movementSpeedX = 0;
    gFieldCamera.movementSpeedY = 0;
    gFieldCamera.x = 0;
    gFieldCamera.y = 0;
    gFieldCamera.spriteId = 0;
    gFieldCamera.callback = NULL;
}

u32 InitCameraUpdateCallback(u8 trackedSpriteId)
{
    if (gFieldCamera.spriteId != 0)
        DestroySprite(&gSprites[gFieldCamera.spriteId]);
    gFieldCamera.spriteId = AddCameraObject(trackedSpriteId);
    gFieldCamera.callback = CameraUpdateCallback;
    return 0;
}

void CameraUpdate(void)
{
    int deltaX;
    int deltaY;
    int curMovementOffsetY;
    int curMovementOffsetX;
    int movementSpeedX;
    int movementSpeedY;

    if (gFieldCamera.callback != NULL)
        gFieldCamera.callback(&gFieldCamera);
    movementSpeedX = gFieldCamera.movementSpeedX;
    movementSpeedY = gFieldCamera.movementSpeedY;
    deltaX = 0;
    deltaY = 0;
    curMovementOffsetX = gFieldCamera.x;
    curMovementOffsetY = gFieldCamera.y;


    if (curMovementOffsetX == 0 && movementSpeedX != 0)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY == 0 && movementSpeedY != 0)
    {
        if (movementSpeedY > 0)
            deltaY = 1;
        else
            deltaY = -1;
    }
    if (curMovementOffsetX != 0 && curMovementOffsetX == -movementSpeedX)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY != 0 && curMovementOffsetY == -movementSpeedY)
    {
        if (movementSpeedY > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }

    gFieldCamera.x += movementSpeedX;
    gFieldCamera.x %= 16;
    gFieldCamera.y += movementSpeedY;
    gFieldCamera.y %= 16;

    if (deltaX != 0 || deltaY != 0)
    {
        CameraMove(deltaX, deltaY);
        UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
        RotatingGatePuzzleCameraUpdate(deltaX, deltaY);
        SetBerryTreesSeen();
        AddCameraTileOffset(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        if (IsCoopPlayer2Screen())
        {
            // Linked co-op: CoopView_Update draws Player 2's area instead. When the
            // camera crossed into a connected map, all coordinates were shifted.
            if (gCamera.active)
            {
                sCoopDrawnOriginX -= gCamera.x;
                sCoopDrawnOriginY -= gCamera.y;
            }
        }
        else
        {
            RedrawMapSlicesForCameraUpdate(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        }
    }

    AddCameraPixelOffset(&sFieldCameraOffset, movementSpeedX, movementSpeedY);
    gTotalCameraPixelOffsetX -= movementSpeedX;
    gTotalCameraPixelOffsetY -= movementSpeedY;
}

void MoveCameraAndRedrawMap(int deltaX, int deltaY) // Linked co-op: used when Player 1 joins Player 2 surfing
{
    CameraMove(deltaX, deltaY);
    UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
    DrawWholeMapView();
    gTotalCameraPixelOffsetX -= deltaX * 16;
    gTotalCameraPixelOffsetY -= deltaY * 16;
}

void SetCameraPanningCallback(void (*callback)(void))
{
    sFieldCameraPanningCallback = callback;
}

void SetCameraPanning(s16 horizontal, s16 vertical)
{
    sHorizontalCameraPan = horizontal;
    sVerticalCameraPan = vertical + 32;
}

void InstallCameraPanAheadCallback(void)
{
    sFieldCameraPanningCallback = CameraPanningCB_PanAhead;
    sBikeCameraPanFlag = FALSE;
    sHorizontalCameraPan = 0;
    sVerticalCameraPan = 32;
}

void UpdateCameraPanning(void)
{
    if (sFieldCameraPanningCallback != NULL)
        sFieldCameraPanningCallback();
    //Update sprite offset of overworld objects
    gSpriteCoordOffsetX = gTotalCameraPixelOffsetX - sHorizontalCameraPan;
    gSpriteCoordOffsetY = gTotalCameraPixelOffsetY - sVerticalCameraPan - 8;
    CoopView_Update(); // Linked co-op: Player 2's screen follows Player 2
}

static void CameraPanningCB_PanAhead(void)
{
    u8 var;

    if (gUnusedBikeCameraAheadPanback == FALSE)
    {
        InstallCameraPanAheadCallback();
    }
    else
    {
        // this code is never reached
        if (gPlayerAvatar.tileTransitionState == T_TILE_TRANSITION)
        {
            sBikeCameraPanFlag ^= 1;
            if (sBikeCameraPanFlag == FALSE)
                return;
        }
        else
        {
            sBikeCameraPanFlag = FALSE;
        }

        var = GetPlayerMovementDirection();
        if (var == 2)
        {
            if (sVerticalCameraPan > -8)
                sVerticalCameraPan -= 2;
        }
        else if (var == 1)
        {
            if (sVerticalCameraPan < 72)
                sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan < 32)
        {
            sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan > 32)
        {
            sVerticalCameraPan -= 2;
        }
    }
}
