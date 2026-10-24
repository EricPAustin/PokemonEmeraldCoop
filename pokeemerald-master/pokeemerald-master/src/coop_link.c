#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_main.h"
#include "bg.h"
#include "coop.h"
#include "coop_link.h"
#include "dma3.h"
#include "field_message_box.h"
#include "gpu_regs.h"
#include "link.h"
#include "m4a.h"
#include "main.h"
#include "main_menu.h"
#include "new_game.h"
#include "load_save.h"
#include "malloc.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "pokemon.h"
#include "random.h"
#include "reload_save.h"
#include "rtc.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "text_window.h"
#include "trainer_hill.h"
#include "window.h"
#include "gba/flash_internal.h"
#include "event_data.h"
#include "constants/cable_club.h"
#include "constants/rgb.h"
#include "constants/songs.h"

// Linked co-op core: link-up screen, per-frame lockstep, shared clock, input
// ownership and desync detection. See coop_link.h for the overall design.
//
// The session starts by reloading the save, which wipes EWRAM: the few globals
// below live in IWRAM, and the rest (sCoop) is carried across the reload.

#define LINKCMD_COOP_KEYS   0x6C0B
#define LINKTYPE_COOP       0x7711
#define COOP_SYNC_MAGIC     0x504F4F43 // "COOP"

#define HASH_INTERVAL       32  // Compare game state every this many frames
#define HASH_HISTORY        8
#define INPUT_DELAY         2   // Frames between pressing a button and the game seeing it
#define KEY_FIFO_SIZE       16

struct CoopSyncBlock
{
    u32 magic;
    u32 saveHash;
    u32 rngSeed;
    struct SiiRtcInfo rtc;
    u8 startMode;       // COOP_START_CONTINUE / COOP_START_NEW_GAME
    u8 saveFileStatus;  // New game: Player 1's, so both ask the same "overwrite?" question when saving
    u8 optionsButtonMode;
    u8 optionsTextSpeed;
    u8 optionsWindowFrameType;
    u8 optionsSound;
    u8 optionsBattleStyle;
    u8 optionsBattleSceneOff;
};

bool8 gCoopLinkActive;
u8 gCoopLocalPlayer;
u8 gCoopInputOwner;
u32 gCoopTick;
u32 gCoopDesyncTick;
#if COOP_DEBUG_DESYNC
u32 gCoopDebugFreezeTick;
#endif

// Session state. Kept in EWRAM to save IWRAM (the stack lives right above
// IWRAM's variables); CB2_CoopFirstTick carries it across ReloadSave.
struct CoopLinkState
{
    bool8 tickRan;
    u16 keyFifo[COOP_NUM_PLAYERS][KEY_FIFO_SIZE]; // Each player's buttons, one entry per frame
    u8 keyFifoHead[COOP_NUM_PLAYERS];
    u8 keyFifoCount[COOP_NUM_PLAYERS];
    u32 packetsSent;
    u32 ticksEnded;
    u32 soundSteps;
    u16 playerKeys[COOP_NUM_PLAYERS];
    bool8 inBattlePrompt; // A player is choosing a battle action
    u16 prevPlayerKeys[COOP_NUM_PLAYERS];
    u16 virtualPrevHeld;
    u16 virtualRepeat;
    u32 rngSeed;
    struct SiiRtcInfo baseRtc;
    u16 hashTicks[HASH_HISTORY];
    u32 hashValues[HASH_HISTORY];
    u8 hashPos;
    u16 sendHashTick;
    u32 sendHash;
    bool8 errorShown;
    u8 startMode;
    struct CoopSyncBlock newGameSettings; // Player 1's sync block (options etc. for a new game)
};

static EWRAM_DATA struct CoopLinkState sCoop = {0};

static void Task_CoopLinkup(u8 taskId);
static void LinkCB_CoopSendKeys(void);
static void Task_CoopMessageWaitForA(u8 taskId);
static void CB2_CoopFirstTick(void);
static void ShowCoopErrorScreen(const u8 *text);
static void CheckRemoteHash(u16 tick, u32 hash);

static const u8 sText_Waiting[] = _("Co-op link\nWaiting for the other player...\n\nB: Cancel");
static const u8 sText_Syncing[] = _("Co-op link\nConnected! Syncing...");
static const u8 sText_ModeMismatch[] = _("Both players must choose\nthe same option:\nCONTINUE or NEW GAME.\nA: Return to title");
static const u8 sText_SaveMismatch[] = _("The two save files don't match!\nCopy Player 1's save file over\nPlayer 2's and try again.\nA: Return to title");
static const u8 sText_LinkFailed[] = _("Co-op link failed.\n\n\nA: Return to title");
static const u8 sText_LinkLost[] = _("The co-op link was lost.\n\n\nA: Return to title");
static const u8 sText_Desync[] = _("Co-op desync!\nThe two games got out of step.\nPlease reset both games.\nA: Return to title");

// ---------------------------------------------------------------------------
// Link-up screen
// ---------------------------------------------------------------------------

static const struct BgTemplate sCoopBgTemplates[] =
{
    {
        .bg = 0,
        .charBaseIndex = 0,
        .mapBaseIndex = 31,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 0,
        .baseTile = 0,
    },
    {
        .bg = 3,
        .charBaseIndex = 0,
        .mapBaseIndex = 30,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 1,
        .baseTile = 0,
    },
};

static const struct WindowTemplate sCoopWindowTemplates[] =
{
    {
        .bg = 0,
        .tilemapLeft = 2,
        .tilemapTop = 5,
        .width = 26,
        .height = 8,
        .paletteNum = 15,
        .baseBlock = 11,
    },
    DUMMY_WIN_TEMPLATE
};

static void CoopScreen_MainCB(void)
{
    RunTasks();
    UpdatePaletteFade();
}

static void CoopScreen_VBlankCB(void)
{
    TransferPlttBuffer();
}

static void CoopScreen_Init(void)
{
    u16 i;

    SetVBlankCallback(NULL);
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0);
    SetGpuReg(REG_OFFSET_BG0HOFS, 0);
    SetGpuReg(REG_OFFSET_BG0VOFS, 0);
    SetGpuReg(REG_OFFSET_BG3HOFS, 0);
    SetGpuReg(REG_OFFSET_BG3VOFS, 0);
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    DmaFill16(3, 0, (void *)VRAM, VRAM_SIZE);
    DmaFill32(3, 0, (void *)OAM, OAM_SIZE);
    DmaFill16(3, 0, (void *)(PLTT + 2), PLTT_SIZE - 2);
    ResetPaletteFade();
    gPlttBufferUnfaded[0] = RGB_WHITE;
    gPlttBufferFaded[0] = RGB_WHITE;
    gPlttBufferUnfaded[1] = RGB(5, 10, 14);
    gPlttBufferFaded[1] = RGB(5, 10, 14);
    for (i = 0; i < 0x10; i++)
        ((u16 *)(VRAM + 0x20))[i] = 0x1111;
    for (i = 0; i < 0x400; i++)
        ((u16 *)(BG_SCREEN_ADDR(30)))[i] = 0x0001;
    ResetTasks();
    ResetSpriteData();
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sCoopBgTemplates, ARRAY_COUNT(sCoopBgTemplates));
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
    ShowBg(0);
    ShowBg(3);
    InitWindows(sCoopWindowTemplates);
    DeactivateAllTextPrinters();
    FillWindowPixelBuffer(0, PIXEL_FILL(1));
    LoadWindowGfx(0, 0, 2, BG_PLTT_ID(14));
    LoadPalette(gStandardMenuPalette, BG_PLTT_ID(15), PLTT_SIZE_4BPP);
    DrawStdFrameWithCustomTileAndPalette(0, FALSE, 2, 14);
    PutWindowTilemap(0);
    CopyWindowToVram(0, COPYWIN_FULL);
    EnableInterrupts(INTR_FLAG_VBLANK);
    SetVBlankCallback(CoopScreen_VBlankCB);
    SetMainCallback2(CoopScreen_MainCB);
}

static void CoopScreen_Print(const u8 *text)
{
    FillWindowPixelBuffer(0, PIXEL_FILL(1));
    AddTextPrinterParameterized(0, FONT_NORMAL, text, 0, 1, 0, NULL);
    CopyWindowToVram(0, COPYWIN_FULL);
}

// Called by the main menu: CONTINUE or NEW GAME.
void CoopLink_StartFromMainMenu(u8 startMode)
{
    sCoop.startMode = startMode;
    gCoopLinkActive = FALSE;
    gCoopDesyncTick = 0;
    sCoop.errorShown = FALSE;
    CoopScreen_Init();
    CreateTask(Task_CoopLinkup, 0);
}

// Hash of both save slots in flash. Both GBAs must start from the same save.
static u32 HashSaveFlash(void)
{
    u32 hash = 2166136261u;
    u8 *buffer = Alloc(SECTOR_SIZE);
    s32 sector, i;

    if (buffer == NULL)
        return 0;
    for (sector = 0; sector < NUM_SECTORS_PER_SLOT * 2; sector++)
    {
        ReadFlash(sector, 0, buffer, SECTOR_SIZE);
        for (i = 0; i < SECTOR_SIZE; i++)
            hash = (hash ^ buffer[i]) * 16777619u;
    }
    Free(buffer);
    return hash;
}

enum {
    LINKUP_OPEN,
    LINKUP_WAIT_PLAYERS,
    LINKUP_CONFIRM,
    LINKUP_EXCHANGE,
    LINKUP_SEND_SYNC,
    LINKUP_WAIT_SYNC,
    LINKUP_STANDBY,
    LINKUP_WAIT_STANDBY,
    LINKUP_START,
};

#define tState data[0]
#define tTimer data[1]

static void LinkupFailed(u8 taskId, const u8 *text)
{
    gCoopLinkActive = FALSE;
    CloseLink();
    CoopScreen_Print(text);
    PlaySE(SE_BOO);
    gTasks[taskId].func = Task_CoopMessageWaitForA;
}

static void Task_CoopLinkup(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    struct CoopSyncBlock *block;
    struct CoopSyncBlock *blocks[COOP_NUM_PLAYERS];
    s32 i;

    if (task->tState >= LINKUP_CONFIRM && HasLinkErrorOccurred())
    {
        LinkupFailed(taskId, sText_LinkFailed);
        return;
    }

    switch (task->tState)
    {
    case LINKUP_OPEN:
        gLinkType = LINKTYPE_COOP;
        OpenLinkTimed();
        ResetLinkPlayerCount();
        ResetLinkPlayers();
        CoopScreen_Print(sText_Waiting);
        task->tTimer = 0;
        task->tState++;
        break;
    case LINKUP_WAIT_PLAYERS:
        if (JOY_NEW(B_BUTTON))
        {
            CloseLink();
            DestroyTask(taskId);
            DoSoftReset();
            return;
        }
        if (++task->tTimer >= 10 && GetLinkPlayerCount_2() >= 2)
        {
            task->tTimer = 0;
            task->tState++;
        }
        break;
    case LINKUP_CONFIRM:
        if (++task->tTimer < 30)
            break;
        if (IsLinkMaster())
        {
            SaveLinkPlayers(GetLinkPlayerCount_2());
            CheckShouldAdvanceLinkState();
        }
        task->tState++;
        break;
    case LINKUP_EXCHANGE:
        switch (GetLinkPlayerDataExchangeStatusTimed(2, 2))
        {
        case EXCHANGE_COMPLETE:
            gCoopLocalPlayer = (GetMultiplayerId() == 0) ? COOP_PLAYER_1 : COOP_PLAYER_2;
            SaveLinkPlayers(GetLinkPlayerCount_2());
            CoopScreen_Print(sText_Syncing);
            task->tState++;
            break;
        case EXCHANGE_DIFF_SELECTIONS:
        case EXCHANGE_PLAYER_NOT_READY:
        case EXCHANGE_PARTNER_NOT_READY:
        case EXCHANGE_WRONG_NUM_PLAYERS:
            LinkupFailed(taskId, sText_LinkFailed);
            break;
        }
        break;
    case LINKUP_SEND_SYNC:
        if (!IsLinkTaskFinished())
            break;
        block = (struct CoopSyncBlock *)gBlockSendBuffer;
        block->magic = COOP_SYNC_MAGIC;
        block->saveHash = HashSaveFlash();
        block->rngSeed = gRngValue;
        RtcGetInfo(&block->rtc);
        block->startMode = sCoop.startMode;
        block->saveFileStatus = gSaveFileStatus;
        block->optionsButtonMode = gSaveBlock2Ptr->optionsButtonMode;
        block->optionsTextSpeed = gSaveBlock2Ptr->optionsTextSpeed;
        block->optionsWindowFrameType = gSaveBlock2Ptr->optionsWindowFrameType;
        block->optionsSound = gSaveBlock2Ptr->optionsSound;
        block->optionsBattleStyle = gSaveBlock2Ptr->optionsBattleStyle;
        block->optionsBattleSceneOff = gSaveBlock2Ptr->optionsBattleSceneOff;
        SendBlock(0, block, sizeof(*block));
        task->tState++;
        break;
    case LINKUP_WAIT_SYNC:
        if (GetBlockReceivedStatus() != GetLinkPlayerCountAsBitFlags())
            break;
        for (i = 0; i < COOP_NUM_PLAYERS; i++)
            blocks[i] = (struct CoopSyncBlock *)gBlockRecvBuffer[i];
        ResetBlockReceivedFlags();
        if (blocks[0]->magic != COOP_SYNC_MAGIC || blocks[1]->magic != COOP_SYNC_MAGIC)
        {
            LinkupFailed(taskId, sText_LinkFailed);
            break;
        }
        if (blocks[0]->startMode != blocks[1]->startMode)
        {
            LinkupFailed(taskId, sText_ModeMismatch);
            break;
        }
        // Continuing needs the same save on both GBAs. A new game starts fresh.
        if (blocks[0]->startMode == COOP_START_CONTINUE
         && (blocks[0]->saveHash != blocks[1]->saveHash || blocks[0]->saveHash == 0))
        {
            LinkupFailed(taskId, sText_SaveMismatch);
            break;
        }
        sCoop.newGameSettings = *blocks[0];
        // Player 1's random seed and clock are used by both games.
        sCoop.rngSeed = blocks[0]->rngSeed;
        sCoop.baseRtc = blocks[0]->rtc;
        task->tState++;
        break;
    case LINKUP_STANDBY:
        SetLinkStandbyCallback();
        task->tState++;
        break;
    case LINKUP_WAIT_STANDBY:
        if (IsLinkTaskFinished())
            task->tState++;
        break;
    case LINKUP_START:
        // From here on, the game only advances when both players' buttons for the
        // next frame have arrived (see CoopLink_BeginTick). The first frame both
        // games simulate reloads the save, so both start from identical state.
        sCoop.ticksEnded = 0;
        sCoop.packetsSent = 0;
        for (i = 0; i < COOP_NUM_PLAYERS; i++)
        {
            sCoop.keyFifoHead[i] = 0;
            sCoop.keyFifoCount[i] = 0;
        }
        sCoop.soundSteps = 0;
        gCoopTick = 0;
        sCoop.playerKeys[0] = sCoop.playerKeys[1] = 0;
        sCoop.prevPlayerKeys[0] = sCoop.prevPlayerKeys[1] = 0;
        sCoop.virtualPrevHeld = 0;
        sCoop.virtualRepeat = 0;
        sCoop.hashPos = 0;
        sCoop.sendHashTick = 0;
        sCoop.sendHash = 0;
        for (i = 0; i < HASH_HISTORY; i++)
            sCoop.hashTicks[i] = 0xFFFF;
        gCoopInputOwner = COOP_PLAYER_1;
        gReceivedRemoteLinkPlayers = FALSE; // Menus shouldn't behave as if in a link room
        gLinkCallback = LinkCB_CoopSendKeys;
        gCoopLinkActive = TRUE;
        DestroyTask(taskId);
        SetMainCallback2(CB2_CoopFirstTick);
        break;
    }
}

#undef tState
#undef tTimer

static void Task_CoopMessageWaitForA(u8 taskId)
{
    if (JOY_NEW(A_BUTTON | B_BUTTON))
    {
        DestroyTask(taskId);
        DoSoftReset();
    }
}

static void ShowCoopErrorScreen(const u8 *text)
{
    gCoopLinkActive = FALSE;
    CloseLink();
    m4aMPlayAllStop();
    CoopScreen_Init();
    CoopScreen_Print(text);
    CreateTask(Task_CoopMessageWaitForA, 0);
}

// First frame simulated in lockstep, run by both games on the same frame.
// New game: both games start from a blank save with Player 1's options, then
// Professor Birch's introduction begins (Player 1 answers it).
static void StartCoopNewGame(void)
{
    u16 imeBackup = REG_IME;
    const struct CoopSyncBlock *settings = &sCoop.newGameSettings;
    u16 linkType = gLinkType;
    struct CoopLinkState state = sCoop;

    REG_IME = 0;
    RegisterRamReset(RESET_EWRAM);
    ClearGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_FORCED_BLANK);
    REG_IME = imeBackup;
    sCoop = state;
    gLinkType = linkType;
    CoopLink_RestoreLinkOpen();

    gMain.inBattle = FALSE;
    SetSaveBlocksPointers(GetSaveBlocksPointersBaseOffset());
    ResetMenuAndMonGlobals();
    Save_ResetSaveCounters();
    Sav2_ClearSetDefault();
    gSaveBlock2Ptr->optionsButtonMode = settings->optionsButtonMode;
    gSaveBlock2Ptr->optionsTextSpeed = settings->optionsTextSpeed;
    gSaveBlock2Ptr->optionsWindowFrameType = settings->optionsWindowFrameType;
    gSaveBlock2Ptr->optionsSound = settings->optionsSound;
    gSaveBlock2Ptr->optionsBattleStyle = settings->optionsBattleStyle;
    gSaveBlock2Ptr->optionsBattleSceneOff = settings->optionsBattleSceneOff;
    gSaveFileStatus = settings->saveFileStatus;
    SetPokemonCryStereo(gSaveBlock2Ptr->optionsSound);
    InitHeap(gHeap, HEAP_SIZE);
    CoopLink_StartNewGameIntro();
}

static void CB2_CoopFirstTick(void)
{
    u16 linkType = gLinkType;
    struct CoopLinkState state = sCoop;

    if (sCoop.startMode == COOP_START_NEW_GAME)
    {
        gRngValue = sCoop.rngSeed;
        gRng2Value = sCoop.rngSeed ^ 0x5A5A5A5A;
        gMain.vblankCounter1 = 0;
        gMain.vblankCounter2 = 0;
        StartCoopNewGame();
        return;
    }

    // Everything the game logic reads must start out the same on both GBAs.
    gRngValue = sCoop.rngSeed;
    gRng2Value = sCoop.rngSeed ^ 0x5A5A5A5A;
    gMain.vblankCounter1 = 0;
    gMain.vblankCounter2 = 0;
    ReloadSave(); // Wipes EWRAM, reloads the save, then continues the game
    sCoop = state;
    gLinkType = linkType;
    CoopLink_RestoreLinkOpen();
}

// ---------------------------------------------------------------------------
// Lockstep
// ---------------------------------------------------------------------------

// Keeps other link callbacks from taking over. Packets are sent by SendKeys.
static void LinkCB_CoopSendKeys(void)
{
}

// Sends this GBA's buttons for one future frame.
static void SendKeys(u16 keys)
{
    u16 cmd[CMD_LENGTH] = {0};

    cmd[0] = LINKCMD_COOP_KEYS;
    cmd[1] = keys;
    cmd[2] = sCoop.sendHashTick;
    cmd[3] = sCoop.sendHash & 0xFFFF;
    cmd[4] = sCoop.sendHash >> 16;
    CoopLink_SendPacket(cmd);
    sCoop.packetsSent++;
}

// Files a received packet's buttons under each player. Both GBAs receive the
// same packets, so both build the same lists.
static bool8 ReceivePacket(u16 (*recv)[CMD_LENGTH])
{
    s32 i;

    for (i = 0; i < COOP_NUM_PLAYERS; i++)
    {
        if (recv[i][0] != LINKCMD_COOP_KEYS)
            continue;
        if (sCoop.keyFifoCount[i] >= KEY_FIFO_SIZE)
            return FALSE;
        sCoop.keyFifo[i][(sCoop.keyFifoHead[i] + sCoop.keyFifoCount[i]) % KEY_FIFO_SIZE] = recv[i][1] & KEYS_MASK;
        sCoop.keyFifoCount[i]++;
        if (i != GetMultiplayerId())
            CheckRemoteHash(recv[i][2], recv[i][3] | (recv[i][4] << 16));
    }
    return TRUE;
}

static u16 PopKeys(u8 player)
{
    u16 keys = sCoop.keyFifo[player][sCoop.keyFifoHead[player]];
    sCoop.keyFifoHead[player] = (sCoop.keyFifoHead[player] + 1) % KEY_FIFO_SIZE;
    sCoop.keyFifoCount[player]--;
    return keys;
}

static bool8 IsFreeRoam(void)
{
    return gMain.callback2 == CB2_Overworld
        && gMain.callback1 == CB1_Overworld
        && !ArePlayerFieldControlsLocked();
}

static void ApplyVirtualKeys(u16 held)
{
    held &= KEYS_MASK;
    gMain.newKeysRaw = held & ~sCoop.virtualPrevHeld;
    gMain.newKeys = gMain.newKeysRaw;
    gMain.newAndRepeatedKeys = gMain.newKeysRaw;
    if (held != 0 && held == sCoop.virtualPrevHeld)
    {
        if (--sCoop.virtualRepeat == 0)
        {
            gMain.newAndRepeatedKeys = held;
            sCoop.virtualRepeat = gKeyRepeatContinueDelay;
        }
    }
    else
    {
        sCoop.virtualRepeat = gKeyRepeatStartDelay;
    }
    gMain.keyRepeatCounter = sCoop.virtualRepeat;
    gMain.heldKeysRaw = held;
    gMain.heldKeys = held;
    if (gSaveBlock2Ptr->optionsButtonMode == OPTIONS_BUTTON_MODE_L_EQUALS_A)
    {
        if (JOY_NEW(L_BUTTON))
            gMain.newKeys |= A_BUTTON;
        if (JOY_HELD(L_BUTTON))
            gMain.heldKeys |= A_BUTTON;
    }
    if (JOY_NEW(gMain.watchedKeysMask))
        gMain.watchedKeysPressed = TRUE;
    sCoop.virtualPrevHeld = held;
}

// Decides whose buttons drive the game this frame. Both GBAs compute the same
// answer, since it only depends on the shared game state and both players' keys.
//
// - Walking around: Player 1's buttons move the main character (Player 2's
//   character is moved by coop_player2.c). Player 2 can open the START menu,
//   and then controls it until it closes.
// - Battle choices (FIGHT/BAG/POKéMON/RUN, moves, targets, switching): the
//   player who owns that Pokémon.
// - Text (battle messages, dialogue boxes): either player can press A or B.
// - Everything else: the last player in control (usually Player 1).
static void UpdateVirtualInput(void)
{
    u16 new1 = sCoop.playerKeys[COOP_PLAYER_1] & ~sCoop.prevPlayerKeys[COOP_PLAYER_1];
    u16 new2 = sCoop.playerKeys[COOP_PLAYER_2] & ~sCoop.prevPlayerKeys[COOP_PLAYER_2];
    bool8 shared;

    if (!gMain.inBattle)
        sCoop.inBattlePrompt = FALSE;

    if (IsFreeRoam())
    {
        gCoopInputOwner = COOP_PLAYER_1;
        if ((new2 & START_BUTTON) && !(new1 & (START_BUTTON | SELECT_BUTTON | A_BUTTON)))
        {
            gCoopInputOwner = COOP_PLAYER_2;
            ApplyVirtualKeys(START_BUTTON);
            return;
        }
        ApplyVirtualKeys(sCoop.playerKeys[COOP_PLAYER_1]);
        return;
    }

    if (gMain.inBattle)
        shared = !sCoop.inBattlePrompt && gMain.callback2 == BattleMainCB2;
    else
        shared = gMain.callback2 == CB2_Overworld && !IsFieldMessageBoxHidden();

    if (shared)
        ApplyVirtualKeys(sCoop.playerKeys[gCoopInputOwner] | (sCoop.playerKeys[gCoopInputOwner ^ 1] & (A_BUTTON | B_BUTTON)));
    else
        ApplyVirtualKeys(sCoop.playerKeys[gCoopInputOwner]);
}

static void CheckRemoteHash(u16 tick, u32 hash)
{
    s32 i;

    if (gCoopDesyncTick != 0 || tick == 0)
        return;
    for (i = 0; i < HASH_HISTORY; i++)
    {
        if (sCoop.hashTicks[i] == tick)
        {
            if (sCoop.hashValues[i] != hash)
            {
                gCoopDesyncTick = gCoopTick;
#if COOP_DEBUG_DESYNC
                gCoopDebugFreezeTick = tick + 64;
#endif
            }
            return;
        }
    }
}

bool8 CoopLink_BeginTick(void)
{
    u16 recv[MAX_LINK_PLAYERS][CMD_LENGTH];
    u16 localKeys = gMain.heldKeysRaw; // This GBA's real buttons, read this frame
    s32 i;

    sCoop.tickRan = FALSE;
    if (gLinkErrorOccurred)
    {
        // The link broke. Leave lockstep so the link error screen can run.
        gCoopLinkActive = FALSE;
        return TRUE;
    }
    if (!CoopLink_IsLinkOpen() || !(gLinkStatus & LINK_STAT_CONN_ESTABLISHED))
    {
        ShowCoopErrorScreen(sText_LinkLost);
        return TRUE;
    }
    gLinkCallback = LinkCB_CoopSendKeys; // Nothing else may take over the link

    // Collect everything that arrived since last frame.
    if (!(gLinkStatus & LINK_STAT_RECEIVED_NOTHING) && !ReceivePacket(gRecvCmds))
    {
        ShowCoopErrorScreen(sText_LinkLost);
        return TRUE;
    }
    while (CoopLink_DequeueRecvPacket(recv))
    {
        if (!ReceivePacket(recv))
        {
            ShowCoopErrorScreen(sText_LinkLost);
            return TRUE;
        }
    }

    // Simulate a frame once both players' buttons for it are here. Otherwise
    // wait (the other GBA is behind, e.g. still loading a map).
#if COOP_DEBUG_DESYNC
    if (gCoopDebugFreezeTick != 0 && gCoopTick >= gCoopDebugFreezeTick)
        return FALSE; // Both games stop at the same frame so their memory can be compared
#endif
    if (sCoop.keyFifoCount[COOP_PLAYER_1] != 0 && sCoop.keyFifoCount[COOP_PLAYER_2] != 0)
    {
        // Graphics copies queued last frame are done by now on both GBAs.
        FlushDma3Requests();
        for (i = 0; i < COOP_NUM_PLAYERS; i++)
        {
            sCoop.prevPlayerKeys[i] = sCoop.playerKeys[i];
            sCoop.playerKeys[i] = PopKeys(i);
        }
        UpdateVirtualInput();

        // Things the original game advances once per screen refresh now advance
        // once per simulated frame, so both games see the same values.
        gMain.vblankCounter1++;
        gMain.vblankCounter2++;
        if (gTrainerHillVBlankCounter && *gTrainerHillVBlankCounter < 0xFFFFFFFF)
            (*gTrainerHillVBlankCounter)++;
        Random();
        gCoopTick++;
        sCoop.tickRan = TRUE;
    }

    // Stay INPUT_DELAY frames ahead: one packet per simulated frame.
    if (sCoop.packetsSent < gCoopTick + INPUT_DELAY)
        SendKeys(localKeys);

    return sCoop.tickRan;
}

static u32 HashBytes(u32 hash, const void *data, u32 size)
{
    const u8 *bytes = data;
    u32 i;

    for (i = 0; i < size; i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static u32 HashU32(u32 hash, u32 value)
{
    return HashBytes(hash, &value, sizeof(value));
}

// Hash of the game state that must be identical on both GBAs.
static u32 ComputeStateHash(void)
{
    u32 hash = 2166136261u;
    s32 i;

    hash = HashU32(hash, gRngValue);
    hash = HashU32(hash, (u32)gMain.callback1);
    hash = HashU32(hash, (u32)gMain.callback2);
    hash = HashBytes(hash, &gSaveBlock1Ptr->location, sizeof(gSaveBlock1Ptr->location));
    hash = HashBytes(hash, &gSaveBlock1Ptr->pos, sizeof(gSaveBlock1Ptr->pos));
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        struct ObjectEvent *obj = &gObjectEvents[i];
        if (!obj->active)
            continue;
        hash = HashU32(hash, i | (obj->localId << 8) | (obj->graphicsId << 16) | (obj->facingDirection << 24));
        hash = HashBytes(hash, &obj->currentCoords, sizeof(obj->currentCoords));
    }
    hash = HashBytes(hash, gPlayerParty, sizeof(gPlayerParty));
    if (gMain.inBattle)
        hash = HashBytes(hash, gBattleMons, sizeof(gBattleMons));
    return hash;
}


#if COOP_DEBUG_DESYNC
// Desync debugging (see COOP_DEBUG_DESYNC): a hash of each part of the game
// state for the last 128 frames. Compare gCoopDebugHistory between the two
// GBAs after both stop at gCoopDebugFreezeTick to see what diverged first.
EWRAM_DATA u32 gCoopDebugHistory[128][8] = {0};
u32 CoopDebug_GetScriptInfo(void); // script.c

static void RecordDebugHistory(void)
{
    u32 *h = gCoopDebugHistory[gCoopTick % 128];
    u32 x;
    s32 i;

    h[0] = gCoopTick;
    h[1] = gRngValue;
    h[2] = (u32)gMain.callback2;
    x = 2166136261u;
    for (i = 0; i < NUM_TASKS; i++)
        x = HashU32(x, gTasks[i].isActive ? (u32)gTasks[i].func : 0);
    h[3] = x;
    x = 2166136261u;
    for (i = 0; i < NUM_TASKS; i++)
    {
        if (gTasks[i].isActive)
            x = HashBytes(x, gTasks[i].data, sizeof(gTasks[i].data));
    }
    h[4] = x;
    x = 2166136261u;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
        x = HashBytes(x, &gObjectEvents[i].currentCoords, sizeof(gObjectEvents[i].currentCoords));
    h[5] = x;
    h[6] = HashBytes(2166136261u, gPlayerParty, sizeof(gPlayerParty));
    h[7] = CoopDebug_GetScriptInfo();
}
#endif

void CoopLink_EndTick(void)
{
#if COOP_DEBUG_DESYNC
    RecordDebugHistory();
#endif
    sCoop.ticksEnded++;
    if (gCoopTick % HASH_INTERVAL == 0)
    {
        u32 hash = ComputeStateHash();
        sCoop.hashTicks[sCoop.hashPos] = gCoopTick & 0xFFFF;
        sCoop.hashValues[sCoop.hashPos] = hash;
        sCoop.hashPos = (sCoop.hashPos + 1) % HASH_HISTORY;
        sCoop.sendHashTick = gCoopTick & 0xFFFF;
        sCoop.sendHash = hash;
    }
    if (gCoopDesyncTick != 0 && !sCoop.errorShown && !COOP_DEBUG_DESYNC)
    {
        sCoop.errorShown = TRUE;
        ShowCoopErrorScreen(sText_Desync);
    }
}

// A cable club feature tried to start its own link session (see CreateLinkupTask).
void CoopLink_RefuseLinkup(void)
{
    gLinkType = LINKTYPE_COOP;
    gSpecialVar_Result = LINKUP_FAILED;
    ScriptContext_Enable();
}

bool8 CoopLink_TickRanThisFrame(void)
{
    return sCoop.tickRan;
}

// The sound engine advances exactly once between simulated frames, so scripts
// that wait for a sound effect or music to finish behave the same on both GBAs.
bool8 CoopLink_ShouldStepSound(void)
{
    if (sCoop.soundSteps < sCoop.ticksEnded)
    {
        sCoop.soundSteps = sCoop.ticksEnded;
        return TRUE;
    }
    return FALSE;
}

u16 CoopLink_GetPlayerKeys(u8 player)
{
    return sCoop.playerKeys[player];
}

u16 CoopLink_GetPlayerNewKeys(u8 player)
{
    return sCoop.playerKeys[player] & ~sCoop.prevPlayerKeys[player];
}

void CoopLink_EndBattlePrompt(void)
{
    sCoop.inBattlePrompt = FALSE;
}

void CoopLink_SetInputOwnerForBattler(u8 battler)
{
    if (!gCoopLinkActive)
        return;
    sCoop.inBattlePrompt = TRUE;
    if (GetBattlerSide(battler) != B_SIDE_PLAYER)
        return;
    gCoopInputOwner = GetMonCoopOwner(&gPlayerParty[gBattlerPartyIndexes[battler]]);
}

// ---------------------------------------------------------------------------
// Shared clock: Player 1's clock at link-up, advanced by simulated frames.
// ---------------------------------------------------------------------------

static u8 ToBcd(u32 value)
{
    return ((value / 10) << 4) | (value % 10);
}

static u8 DaysInMonth(u32 year, u32 month)
{
    static const u8 sDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && IsLeapYear(year))
        return 29;
    return sDays[month - 1];
}

bool8 CoopLink_GetRtcInfo(struct SiiRtcInfo *rtc)
{
    u32 seconds, days;
    u32 year, month, day;

    if (!gCoopLinkActive)
        return FALSE;

    *rtc = sCoop.baseRtc;
    seconds = ConvertBcdToBinary(sCoop.baseRtc.hour) * 3600
            + ConvertBcdToBinary(sCoop.baseRtc.minute) * 60
            + ConvertBcdToBinary(sCoop.baseRtc.second)
            + gCoopTick / 60;
    days = seconds / 86400;
    seconds %= 86400;
    rtc->hour = ToBcd(seconds / 3600);
    rtc->minute = ToBcd((seconds / 60) % 60);
    rtc->second = ToBcd(seconds % 60);

    if (days != 0)
    {
        year = ConvertBcdToBinary(sCoop.baseRtc.year);
        month = ConvertBcdToBinary(sCoop.baseRtc.month);
        day = ConvertBcdToBinary(sCoop.baseRtc.day);
        rtc->dayOfWeek = (sCoop.baseRtc.dayOfWeek + days) % 7;
        if (month < 1 || month > 12)
            month = 1;
        while (days != 0)
        {
            days--;
            if (++day > DaysInMonth(year, month))
            {
                day = 1;
                if (++month > 12)
                {
                    month = 1;
                    year = (year + 1) % 100;
                }
            }
        }
        rtc->year = ToBcd(year);
        rtc->month = ToBcd(month);
        rtc->day = ToBcd(day);
    }
    return TRUE;
}
