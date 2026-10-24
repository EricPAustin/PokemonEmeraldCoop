#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_message.h"
#include "coop.h"
#include "coop_link.h"
#include "event_data.h"
#include "strings.h"
#include "battle_setup.h"
#include "data.h"
#include "string_util.h"
#include "main.h"
#include "malloc.h"
#include "pokemon.h"
#include "util.h"
#include "constants/battle.h"
#include "constants/characters.h"
#include "constants/species.h"
#include "constants/opponents.h"
#include "constants/trainers.h"
#include "constants/items.h"

// Battle types that keep their vanilla behavior (no co-op double battle).
#define COOP_INELIGIBLE_BATTLE_TYPES (BATTLE_TYPE_LINK              \
                                    | BATTLE_TYPE_LINK_IN_BATTLE    \
                                    | BATTLE_TYPE_RECORDED          \
                                    | BATTLE_TYPE_RECORDED_LINK     \
                                    | BATTLE_TYPE_FRONTIER          \
                                    | BATTLE_TYPE_MULTI             \
                                    | BATTLE_TYPE_INGAME_PARTNER    \
                                    | BATTLE_TYPE_TOWER_LINK_MULTI  \
                                    | BATTLE_TYPE_SAFARI            \
                                    | BATTLE_TYPE_WALLY_TUTORIAL    \
                                    | BATTLE_TYPE_FIRST_BATTLE      \
                                    | BATTLE_TYPE_TRAINER_HILL      \
                                    | BATTLE_TYPE_EREADER_TRAINER)

EWRAM_DATA bool8 gCoopBattle = FALSE;
EWRAM_DATA bool8 gCoopChoosingPlayer2Starter = FALSE; // The starter screen is open for Player 2
EWRAM_DATA bool8 gCoopMirrorBattle = FALSE; // The second opponent was added by co-op (see AddMirrorOpponent)

static const u8 sText_CoopP1WhatWillPkmnDo[] = _("P1: What will\n{B_ACTIVE_NAME_WITH_PREFIX} do?");
static const u8 sText_CoopP2WhatWillPkmnDo[] = _("P2: What will\n{B_ACTIVE_NAME_WITH_PREFIX} do?");
const u8 gText_CoopNotYourMon[] = _("{STR_VAR_1} belongs to\nthe other player!");

// ---------------------------------------------------------------------------
// Ownership
// ---------------------------------------------------------------------------

u8 GetBoxMonCoopOwner(struct BoxPokemon *boxMon)
{
    return boxMon->coopOwner;
}

void SetBoxMonCoopOwner(struct BoxPokemon *boxMon, u8 owner)
{
    boxMon->coopOwner = (owner == COOP_PLAYER_2) ? COOP_PLAYER_2 : COOP_PLAYER_1;
}

u8 GetMonCoopOwner(struct Pokemon *mon)
{
    return GetBoxMonCoopOwner(&mon->box);
}

void SetMonCoopOwner(struct Pokemon *mon, u8 owner)
{
    SetBoxMonCoopOwner(&mon->box, owner);
}

void Coop_SwapOwners(struct Pokemon *mon1, struct Pokemon *mon2)
{
    u8 owner1 = GetMonCoopOwner(mon1);

    SetMonCoopOwner(mon1, GetMonCoopOwner(mon2));
    SetMonCoopOwner(mon2, owner1);
}

static bool8 IsPartySlotFilled(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_SPECIES, NULL) != SPECIES_NONE;
}

static bool8 IsMonAbleToBattle(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_SPECIES_OR_EGG, NULL) != SPECIES_NONE
        && GetMonData(mon, MON_DATA_SPECIES_OR_EGG, NULL) != SPECIES_EGG
        && !GetMonData(mon, MON_DATA_IS_EGG, NULL)
        && GetMonData(mon, MON_DATA_HP, NULL) != 0;
}

u8 Coop_CountTeam(u8 owner)
{
    s32 i;
    u8 count = 0;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (IsPartySlotFilled(&gPlayerParty[i]) && GetMonCoopOwner(&gPlayerParty[i]) == owner)
            count++;
    }
    return count;
}

// Makes sure no player has more than COOP_TEAM_SIZE Pokémon in the party.
// Extra Pokémon are handed to the other player. Never reorders the party.
static void FixTeamOverflow(void)
{
    s32 i;
    u8 counts[COOP_NUM_PLAYERS];

    counts[COOP_PLAYER_1] = 0;
    counts[COOP_PLAYER_2] = 0;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        u8 owner;

        if (!IsPartySlotFilled(&gPlayerParty[i]))
            continue;

        owner = GetMonCoopOwner(&gPlayerParty[i]);
        if (counts[owner] >= COOP_TEAM_SIZE)
        {
            owner ^= 1;
            SetMonCoopOwner(&gPlayerParty[i], owner);
        }
        counts[owner]++;
    }
}

// Puts the party in co-op order, the same layout as the party screen in a
// co-op battle: Player 1's lead, Player 2's lead, then the rest of Player 1's
// team, then the rest of Player 2's. Each team keeps its own order.
static void SortPartyByOwner(void)
{
    struct Pokemon *sorted = AllocZeroed(sizeof(struct Pokemon) * PARTY_SIZE);
    u8 team[COOP_NUM_PLAYERS][PARTY_SIZE];
    u8 count[COOP_NUM_PLAYERS] = {0, 0};
    s32 i, n = 0;
    u8 owner;

    if (sorted == NULL)
        return;

    CalculatePlayerPartyCount();
    for (i = 0; i < gPlayerPartyCount; i++)
    {
        owner = GetMonCoopOwner(&gPlayerParty[i]);
        team[owner][count[owner]++] = i;
    }

    for (owner = 0; owner < COOP_NUM_PLAYERS; owner++)
    {
        if (count[owner] != 0)
            sorted[n++] = gPlayerParty[team[owner][0]];
    }
    for (owner = 0; owner < COOP_NUM_PLAYERS; owner++)
    {
        for (i = 1; i < count[owner]; i++)
            sorted[n++] = gPlayerParty[team[owner][i]];
    }
    for (i = 0; i < n; i++)
        gPlayerParty[i] = sorted[i];
    Free(sorted);
}

// TRUE if both players have a Pokémon in the party: the party screen then
// uses the two-column co-op layout.
bool8 Coop_BothTeamsInParty(void)
{
    return COOP_ENABLED && Coop_CountTeam(COOP_PLAYER_1) != 0 && Coop_CountTeam(COOP_PLAYER_2) != 0;
}

// Must not be called during a battle, since it can reorder the party.
void Coop_NormalizeParty(void)
{
    if (!COOP_ENABLED)
        return;

    FixTeamOverflow();
    SortPartyByOwner();
}

// Returns the player who should receive a newly obtained Pokémon: whoever has
// fewer Pokémon in the party (Player 1 on a tie), so both teams fill up evenly.
// Does not reorder the party, so it is safe to call during battle.
u8 Coop_GetOwnerForNewMon(void)
{
    u8 count1, count2;

    FixTeamOverflow();
    count1 = Coop_CountTeam(COOP_PLAYER_1);
    count2 = Coop_CountTeam(COOP_PLAYER_2);

    if (count1 >= COOP_TEAM_SIZE)
        return COOP_PLAYER_2;
    if (count2 >= COOP_TEAM_SIZE)
        return COOP_PLAYER_1;
    if (count2 < count1)
        return COOP_PLAYER_2;
    return COOP_PLAYER_1;
}

// ---------------------------------------------------------------------------
// Player 2's name and gender
//
// Stored in SaveBlock2 (coopP2Name, coopP2Gender, coopP2Named) in space that
// was unused, so older saves still load. Those have coopP2Named == FALSE and
// keep the old look: the rival of the other gender, named BRENDAN or MAY.
// ---------------------------------------------------------------------------

u8 Coop_GetPlayer2Gender(void)
{
    if (!gSaveBlock2Ptr->coopP2Named)
        return gSaveBlock2Ptr->playerGender ^ 1;
    return gSaveBlock2Ptr->coopP2Gender;
}

void Coop_CopyPlayerName(u8 *dest, u8 owner)
{
    if (owner != COOP_PLAYER_2)
        StringCopy(dest, gSaveBlock2Ptr->playerName);
    else if (gSaveBlock2Ptr->coopP2Named)
        StringCopy(dest, gSaveBlock2Ptr->coopP2Name);
    else
        StringCopy(dest, Coop_GetPlayer2Gender() == MALE ? gText_ExpandedPlaceholder_Brendan : gText_ExpandedPlaceholder_May);
}

// Called once Player 2 confirmed their name in Birch's introduction.
void Coop_SetPlayer2Named(u8 gender)
{
    gSaveBlock2Ptr->coopP2Gender = gender;
    gSaveBlock2Ptr->coopP2Named = TRUE;
    gSaveBlock2Ptr->coopP2Name[PLAYER_NAME_LENGTH] = EOS;
}

// Special for Birch's lab: VAR_RESULT = TRUE if Player 2 has a Pokémon to
// nickname. Sets VAR_0x8004 to its party slot, STR_VAR_1 to its species
// and STR_VAR_2 to Player 2's name.
void Coop_BufferPlayer2Starter(void)
{
    s32 i;

    gSpecialVar_Result = FALSE;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (IsPartySlotFilled(&gPlayerParty[i]) && GetMonCoopOwner(&gPlayerParty[i]) == COOP_PLAYER_2)
        {
            gSpecialVar_0x8004 = i;
            StringCopy(gStringVar1, gSpeciesNames[GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL)]);
            Coop_CopyPlayerName(gStringVar2, COOP_PLAYER_2);
            gSpecialVar_Result = TRUE;
            return;
        }
    }
}

// Specials: who answers the rest of the running script (menus, naming).
void Coop_GiveInputToPlayer2(void)
{
    gCoopInputOwner = COOP_PLAYER_2;
}

void Coop_GiveInputToPlayer1(void)
{
    gCoopInputOwner = COOP_PLAYER_1;
}

// ---------------------------------------------------------------------------
// Battle
// ---------------------------------------------------------------------------

static bool8 CanTeamFight(u8 owner)
{
    s32 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (IsMonAbleToBattle(&gPlayerParty[i]) && GetMonCoopOwner(&gPlayerParty[i]) == owner)
            return TRUE;
    }
    return FALSE;
}

void Coop_ResetBattle(void)
{
    gCoopBattle = FALSE;
    gCoopMirrorBattle = FALSE;
}

// ---------------------------------------------------------------------------
// Mirror opponent (goal 2)
//
// In a co-op trainer battle, the trainer gets a partner: another trainer of
// the same class, with as many Pokémon as the trainer has (each side fields
// up to COOP_TEAM_SIZE, like each player). The partner's Pokémon are different
// species at the same levels. Everything is picked from the trainer's ID, so
// a trainer always brings the same partner, and both linked GBAs agree.
// ---------------------------------------------------------------------------

static u32 HashU32(u32 x)
{
    x ^= x >> 16;
    x *= 0x7FEB352D;
    x ^= x >> 15;
    x *= 0x846CA68B;
    x ^= x >> 16;
    return x;
}

static u16 GetTrainerPartyMonSpecies(u16 trainerId, u8 i, u16 *heldItem)
{
    const struct Trainer *trainer = &gTrainers[trainerId];

    *heldItem = ITEM_NONE;
    switch (trainer->partyFlags & (F_TRAINER_PARTY_CUSTOM_MOVESET | F_TRAINER_PARTY_HELD_ITEM))
    {
    case 0:
        return trainer->party.NoItemDefaultMoves[i].species;
    case F_TRAINER_PARTY_CUSTOM_MOVESET:
        return trainer->party.NoItemCustomMoves[i].species;
    case F_TRAINER_PARTY_HELD_ITEM:
        *heldItem = trainer->party.ItemDefaultMoves[i].heldItem;
        return trainer->party.ItemDefaultMoves[i].species;
    default:
        *heldItem = trainer->party.ItemCustomMoves[i].heldItem;
        return trainer->party.ItemCustomMoves[i].species;
    }
}

// Classes with only one person get a partner from a related class.
static const u8 sMirrorRelatedClasses[][2] =
{
    {TRAINER_CLASS_AQUA_LEADER,   TRAINER_CLASS_AQUA_ADMIN},
    {TRAINER_CLASS_MAGMA_LEADER,  TRAINER_CLASS_MAGMA_ADMIN},
    {TRAINER_CLASS_MAGMA_ADMIN,   TRAINER_CLASS_TEAM_MAGMA},
    {TRAINER_CLASS_COOLTRAINER_2, TRAINER_CLASS_COOLTRAINER},
};

// Same person (rematches and story variants share the trainer's name). Team
// grunts are all called GRUNT but are different people.
static bool8 IsSamePerson(u16 trainerId, u16 otherId)
{
    u8 trainerClass = gTrainers[trainerId].trainerClass;

    if (StringCompare(gTrainers[trainerId].trainerName, gTrainers[otherId].trainerName) != 0)
        return FALSE;
    if (trainerClass == gTrainers[otherId].trainerClass
     && (trainerClass == TRAINER_CLASS_TEAM_AQUA || trainerClass == TRAINER_CLASS_TEAM_MAGMA))
        return trainerId == otherId;
    return TRUE;
}

static bool8 IsMirrorCandidate(u16 trainerId, u16 trainerA, u8 trainerClass)
{
    return gTrainers[trainerId].trainerClass == trainerClass
        && gTrainers[trainerId].partySize != 0
        && !IsSamePerson(trainerId, trainerA);
}

static u16 PickMirrorTrainerOfClass(u16 trainerA, u8 trainerClass)
{
    u16 i, count = 0, pick;

    for (i = 1; i < TRAINERS_COUNT; i++)
    {
        if (IsMirrorCandidate(i, trainerA, trainerClass))
            count++;
    }
    if (count == 0)
        return TRAINER_NONE;

    pick = HashU32(trainerA) % count;
    for (i = 1; i < TRAINERS_COUNT; i++)
    {
        if (IsMirrorCandidate(i, trainerA, trainerClass) && pick-- == 0)
            return i;
    }
    return TRAINER_NONE;
}

// Another trainer of the same class (or a related one), or the same trainer
// if there is none (e.g. the Champion, who then brings the rest of his team).
static u16 PickMirrorTrainer(u16 trainerA)
{
    u8 trainerClass = gTrainers[trainerA].trainerClass;
    u16 trainerB = PickMirrorTrainerOfClass(trainerA, trainerClass);
    u8 i;

    for (i = 0; trainerB == TRAINER_NONE && i < ARRAY_COUNT(sMirrorRelatedClasses); i++)
    {
        if (sMirrorRelatedClasses[i][0] == trainerClass)
            trainerB = PickMirrorTrainerOfClass(trainerA, sMirrorRelatedClasses[i][1]);
    }
    return (trainerB == TRAINER_NONE) ? trainerA : trainerB;
}

static bool8 IsSpeciesUsed(u16 species, const u16 *used, u8 count)
{
    u8 i;

    for (i = 0; i < count; i++)
    {
        if (used[i] == species)
            return TRUE;
    }
    return FALSE;
}

// Finds the next species for the partner: first from the partner's own team,
// then from other trainers of the partner's class, then from any trainer. Never a
// species already in this battle.
static u16 PickMirrorSpecies(u16 trainerB, u16 trainerA, u32 seed, const u16 *used, u8 usedCount, u16 *heldItem)
{
    u16 pass, n, t, i, start, species;

    for (pass = 0; pass < 3; pass++)
    {
        start = (pass == 0) ? trainerB : 1 + seed % (TRAINERS_COUNT - 1);
        for (n = 0; n < TRAINERS_COUNT; n++)
        {
            t = start + n;
            if (t >= TRAINERS_COUNT)
                t -= TRAINERS_COUNT - 1; // Trainer IDs start at 1
            if (pass == 0 && t != trainerB)
                break;
            if (pass == 1 && gTrainers[t].trainerClass != gTrainers[trainerB].trainerClass)
                continue;
            for (i = 0; i < gTrainers[t].partySize; i++)
            {
                u8 slot = (i + seed) % gTrainers[t].partySize;
                species = GetTrainerPartyMonSpecies(t, slot, heldItem);
                if (species == SPECIES_NONE || species == SPECIES_EGG || IsSpeciesUsed(species, used, usedCount))
                    continue;
                if (pass != 0)
                    *heldItem = ITEM_NONE; // Only the partner's own Pokémon keep their items
                return species;
            }
        }
    }
    *heldItem = ITEM_NONE;
    return SPECIES_ZIGZAGOON; // Not reached in practice
}

// Keeps the trainer's strongest COOP_TEAM_SIZE Pokémon (in their original
// order) in slots 0-2, and gives the partner the same number in slots 3-5.
static void AddMirrorOpponent(void)
{
    struct Pokemon *kept = AllocZeroed(sizeof(struct Pokemon) * COOP_TEAM_SIZE);
    u16 used[PARTY_SIZE];
    u8 levels[COOP_TEAM_SIZE];
    u8 count, keptCount, i, j;
    u16 trainerA = gTrainerBattleOpponent_A;
    u16 trainerB;

    if (kept == NULL)
        return;

    count = CalculateEnemyPartyCount();
    if (count == 0)
    {
        Free(kept);
        return;
    }

    // Strongest Pokémon first (later ones win ties: trainers lead up to their ace).
    keptCount = 0;
    for (i = 0; i < count; i++)
    {
        u8 rank = 0;
        u8 level = GetMonData(&gEnemyParty[i], MON_DATA_LEVEL, NULL);

        for (j = 0; j < count; j++)
        {
            u8 other = GetMonData(&gEnemyParty[j], MON_DATA_LEVEL, NULL);
            if (other > level || (other == level && j > i))
                rank++;
        }
        if (rank < COOP_TEAM_SIZE)
            kept[keptCount++] = gEnemyParty[i];
    }

    ZeroEnemyPartyMons();
    for (i = 0; i < keptCount; i++)
    {
        gEnemyParty[i] = kept[i];
        used[i] = GetMonData(&kept[i], MON_DATA_SPECIES, NULL);
        levels[i] = GetMonData(&kept[i], MON_DATA_LEVEL, NULL);
    }
    Free(kept);

    trainerB = PickMirrorTrainer(trainerA);
    for (i = 0; i < keptCount; i++)
    {
        struct Pokemon *mon = &gEnemyParty[COOP_TEAM_SIZE + i];
        u32 seed = HashU32(trainerA * 8 + i);
        u16 heldItem;
        u16 species = PickMirrorSpecies(trainerB, trainerA, seed, used, keptCount + i, &heldItem);
        u8 fixedIV = GetMonData(&gEnemyParty[i], MON_DATA_HP_IV, NULL);

        used[keptCount + i] = species;
        CreateMon(mon, species, levels[i], fixedIV, TRUE, seed, OT_ID_RANDOM_NO_SHINY, 0);
        SetMonData(mon, MON_DATA_HELD_ITEM, &heldItem);
    }

    gTrainerBattleOpponent_B = trainerB;
    gBattleTypeFlags |= BATTLE_TYPE_TWO_OPPONENTS;
    gCoopMirrorBattle = TRUE;
}

bool8 Coop_IsMirrorBattle(void)
{
    return gCoopMirrorBattle;
}

// Called from CB2_InitBattleInternal once the enemy party has been created.
void Coop_InitBattle(void)
{
    gCoopBattle = FALSE;

    if (!COOP_ENABLED)
        return;
    if (gBattleTypeFlags & COOP_INELIGIBLE_BATTLE_TYPES)
        return;

    Coop_NormalizeParty();

    // If only one player can fight, the battle plays out as in the original game.
    if (!CanTeamFight(COOP_PLAYER_1) || !CanTeamFight(COOP_PLAYER_2))
        return;

    gCoopBattle = TRUE;
    gBattleTypeFlags |= BATTLE_TYPE_DOUBLE;

    // Trainers get a partner with a mirrored team.
    if ((gBattleTypeFlags & BATTLE_TYPE_TRAINER)
     && !(gBattleTypeFlags & (BATTLE_TYPE_TWO_OPPONENTS | BATTLE_TYPE_SECRET_BASE)))
        AddMirrorOpponent();
}

bool8 IsCoopBattle(void)
{
    return gCoopBattle && gMain.inBattle;
}

u8 Coop_GetBattlerOwner(u8 battler)
{
    if (GetBattlerPosition(battler) & BIT_FLANK)
        return COOP_PLAYER_2;
    else
        return COOP_PLAYER_1;
}

// Whether the given battler is allowed to switch to / be replaced by this Pokémon.
// Always TRUE outside co-op battles and for the opponent's side.
bool8 Coop_IsMonUsableByBattler(u8 battler, struct Pokemon *mon)
{
    if (!IsCoopBattle() || GetBattlerSide(battler) != B_SIDE_PLAYER)
        return TRUE;

    return GetMonCoopOwner(mon) == Coop_GetBattlerOwner(battler);
}

static u8 GetFirstAbleMon(struct Pokemon *party, s32 exclude, s32 owner)
{
    s32 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (i == exclude)
            continue;
        if (owner >= 0 && GetMonCoopOwner(&party[i]) != owner)
            continue;
        if (IsMonAbleToBattle(&party[i]))
            return i;
    }
    return PARTY_SIZE;
}

// Picks the Pokémon each battler starts with. Returns FALSE if this is not a co-op battle,
// in which case the vanilla logic is used.
bool8 Coop_SetBattlePartyIds(void)
{
    u8 battler, left, right;
    s32 i, id;

    if (!IsCoopBattle())
        return FALSE;

    // Player side: left is Player 1, right is Player 2.
    for (battler = 0; battler < gBattlersCount; battler++)
    {
        if (GetBattlerSide(battler) != B_SIDE_PLAYER)
            continue;

        id = GetFirstAbleMon(gPlayerParty, -1, Coop_GetBattlerOwner(battler));
        gBattlerPartyIndexes[battler] = (id == PARTY_SIZE) ? 0 : id;
    }

    // Opponent side
    left = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
    right = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
    if (gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS)
    {
        gBattlerPartyIndexes[left] = 0;
        gBattlerPartyIndexes[right] = PARTY_SIZE / 2;
        return TRUE;
    }

    id = GetFirstAbleMon(gEnemyParty, -1, -1);
    gBattlerPartyIndexes[left] = (id == PARTY_SIZE) ? 0 : id;

    id = GetFirstAbleMon(gEnemyParty, gBattlerPartyIndexes[left], -1);
    if (id == PARTY_SIZE)
    {
        // The opponent only has one Pokémon. Point the right battler at a slot
        // that can't fight (preferably an empty one); it starts the battle absent.
        for (i = 0; i < PARTY_SIZE; i++)
        {
            if (i != gBattlerPartyIndexes[left] && !IsPartySlotFilled(&gEnemyParty[i]))
                break;
        }
        if (i == PARTY_SIZE)
        {
            for (i = 0; i < PARTY_SIZE; i++)
            {
                if (i != gBattlerPartyIndexes[left] && GetMonData(&gEnemyParty[i], MON_DATA_HP, NULL) == 0)
                    break;
            }
        }
        id = (i == PARTY_SIZE) ? gBattlerPartyIndexes[left] : i;
    }
    gBattlerPartyIndexes[right] = id;
    return TRUE;
}

// Called at the start of the battle intro, right after the absent flags are cleared.
void Coop_MarkAbsentBattlersAtStart(void)
{
    u8 left, right;

    if (!IsCoopBattle() || (gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS))
        return;

    left = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
    right = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
    if (gBattlerPartyIndexes[right] == gBattlerPartyIndexes[left]
     || !IsMonAbleToBattle(&gEnemyParty[gBattlerPartyIndexes[right]]))
        gAbsentBattlerFlags |= gBitTable[right];
}

bool8 Coop_IsBattlerAbsent(u8 battler)
{
    return IsCoopBattle() && (gAbsentBattlerFlags & gBitTable[battler]);
}

bool8 Coop_IsPartnerAbsent(u8 battler)
{
    return Coop_IsBattlerAbsent(BATTLE_PARTNER(battler));
}

// The battler directly across the field, or the other opposing battler if that slot is empty.
u8 Coop_GetOpposingBattler(u8 battler)
{
    u8 opposing = BATTLE_OPPOSITE(battler);

    if (IsCoopBattle() && (gAbsentBattlerFlags & gBitTable[opposing]))
        opposing = BATTLE_PARTNER(opposing);
    return opposing;
}

const u8 *Coop_GetWhatWillPkmnDoText(u8 battler)
{
    if (!IsCoopBattle() || GetBattlerSide(battler) != B_SIDE_PLAYER)
        return gText_WhatWillPkmnDo;
    if (Coop_GetBattlerOwner(battler) == COOP_PLAYER_2)
        return sText_CoopP2WhatWillPkmnDo;
    return sText_CoopP1WhatWillPkmnDo;
}

// ---------------------------------------------------------------------------
// PC: CO-OP TEAMS (goal 3)
// ---------------------------------------------------------------------------

// gSpecialVar_0x8004: party slot. Returns 1 if the other team is already full.
u16 Coop_TryMoveMonToOtherTeam(void)
{
    struct Pokemon *mon = &gPlayerParty[gSpecialVar_0x8004];
    u8 other = GetMonCoopOwner(mon) ^ 1;

    GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    Coop_CopyPlayerName(gStringVar2, other);
    if (Coop_CountTeam(other) >= COOP_TEAM_SIZE)
        return 1;
    SetMonCoopOwner(mon, other);
    Coop_NormalizeParty();
    return 0;
}

// gSpecialVar_0x8005 and gSpecialVar_0x8004: party slots of two Pokémon on
// different teams. Returns 1 if they're on the same team.
u16 Coop_TrySwapTeamMons(void)
{
    struct Pokemon *mon1 = &gPlayerParty[gSpecialVar_0x8005];
    struct Pokemon *mon2 = &gPlayerParty[gSpecialVar_0x8004];

    if (GetMonCoopOwner(mon1) == GetMonCoopOwner(mon2))
        return 1;
    GetMonData(mon1, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    GetMonData(mon2, MON_DATA_NICKNAME, gStringVar3);
    StringGet_Nickname(gStringVar3);
    Coop_SwapOwners(mon1, mon2);
    Coop_NormalizeParty();
    return 0;
}
