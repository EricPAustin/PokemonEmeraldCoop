#ifndef GUARD_COOP_H
#define GUARD_COOP_H

// Two-player co-op support.
//
// The party (6 slots) is shared between two players. Every Pokémon carries an
// owner bit (see struct BoxPokemon::coopOwner), and each player may have at most
// COOP_TEAM_SIZE Pokémon in the party.
//
// In battle, when both players have a Pokémon that can fight, the battle is
// turned into a double battle: Player 1 controls the left Pokémon and Player 2
// controls the right one. Each battler can only switch to Pokémon owned by its
// player. If the opponent only has one Pokémon (wild battles, trainers with a
// single Pokémon), the opponent's right slot starts the battle empty.

// Set to FALSE to build the vanilla single-player game.
#define COOP_ENABLED TRUE

#define COOP_PLAYER_1   0
#define COOP_PLAYER_2   1
#define COOP_NUM_PLAYERS 2
#define COOP_TEAM_SIZE  (PARTY_SIZE / COOP_NUM_PLAYERS)

extern bool8 gCoopBattle;
extern bool8 gCoopChoosingPlayer2Starter;
extern const u8 gText_CoopNotYourMon[];

// Ownership
u8 GetMonCoopOwner(struct Pokemon *mon);
void SetMonCoopOwner(struct Pokemon *mon, u8 owner);
u8 GetBoxMonCoopOwner(struct BoxPokemon *boxMon);
void SetBoxMonCoopOwner(struct BoxPokemon *boxMon, u8 owner);
u8 Coop_CountTeam(u8 owner);
u8 Coop_GetOwnerForNewMon(void);
void Coop_NormalizeParty(void);
bool8 Coop_BothTeamsInParty(void);
void Coop_SwapOwners(struct Pokemon *mon1, struct Pokemon *mon2);

// Player 2's name and gender (picked in Birch's introduction)
u8 Coop_GetPlayer2Gender(void);
void Coop_CopyPlayerName(u8 *dest, u8 owner);
void Coop_SetPlayer2Named(u8 gender);
void Coop_BufferPlayer2Starter(void);
void Coop_GiveInputToPlayer2(void);
void Coop_GiveInputToPlayer1(void);

// Battle
void Coop_ResetBattle(void);
bool8 Coop_IsMirrorBattle(void);
void Coop_InitBattle(void);
bool8 IsCoopBattle(void);
u8 Coop_GetBattlerOwner(u8 battler);
bool8 Coop_IsMonUsableByBattler(u8 battler, struct Pokemon *mon);
bool8 Coop_SetBattlePartyIds(void);
void Coop_MarkAbsentBattlersAtStart(void);
bool8 Coop_IsBattlerAbsent(u8 battler);
bool8 Coop_IsPartnerAbsent(u8 battler);
u8 Coop_GetOpposingBattler(u8 battler);
const u8 *Coop_GetWhatWillPkmnDoText(u8 battler);

// PC
u16 Coop_TryMoveMonToOtherTeam(void);
u16 Coop_TrySwapTeamMons(void);

#endif // GUARD_COOP_H
