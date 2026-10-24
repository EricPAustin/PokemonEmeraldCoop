EventScript_PC::
	lockall
	setvar VAR_0x8004, PC_LOCATION_OTHER
	special DoPCTurnOnEffect
	playse SE_PC_ON
	msgbox Text_BootUpPC, MSGBOX_DEFAULT
	goto EventScript_PCMainMenu
	end

EventScript_PCMainMenu::
	message gText_WhichPCShouldBeAccessed
	waitmessage
	special ScriptMenu_CreatePCMultichoice
	goto EventScript_AccessPC
	end

EventScript_AccessPC::
	switch VAR_RESULT
	case 0, EventScript_AccessPokemonStorage
	case 1, EventScript_AccessPlayersPC
	case 2, EventScript_AccessCoopTeams
	case 3, EventScript_AccessHallOfFame
	case 4, EventScript_TurnOffPC
	case MULTI_B_PRESSED, EventScript_TurnOffPC
	end

@ Co-op: move Pokémon between Player 1's and Player 2's teams.
EventScript_AccessCoopTeams::
	playse SE_PC_LOGIN
	msgbox CoopText_TeamsIntro, MSGBOX_DEFAULT
EventScript_CoopTeamsChoose::
	special ChoosePartyMon
	goto_if_eq VAR_0x8004, PARTY_NOTHING_CHOSEN, EventScript_PCMainMenu
	specialvar VAR_RESULT, Coop_TryMoveMonToOtherTeam
	goto_if_eq VAR_RESULT, 1, EventScript_CoopTeamsSwap
	msgbox CoopText_MonJoinedTeam, MSGBOX_DEFAULT
	goto EventScript_CoopTeamsChoose
	end

EventScript_CoopTeamsSwap::
	msgbox CoopText_TeamFullChooseSwap, MSGBOX_DEFAULT
	copyvar VAR_0x8005, VAR_0x8004
	special ChoosePartyMon
	goto_if_eq VAR_0x8004, PARTY_NOTHING_CHOSEN, EventScript_CoopTeamsChoose
	specialvar VAR_RESULT, Coop_TrySwapTeamMons
	goto_if_eq VAR_RESULT, 1, EventScript_CoopTeamsSameTeam
	msgbox CoopText_MonsSwitchedTeams, MSGBOX_DEFAULT
	goto EventScript_CoopTeamsChoose
	end

EventScript_CoopTeamsSameTeam::
	msgbox CoopText_ChooseFromOtherTeam, MSGBOX_DEFAULT
	goto EventScript_CoopTeamsChoose
	end

EventScript_AccessPlayersPC::
	playse SE_PC_LOGIN
	msgbox gText_AccessedPlayersPC, MSGBOX_DEFAULT
	special PlayerPC
	goto EventScript_PCMainMenu
	end

EventScript_AccessPokemonStorage::
	playse SE_PC_LOGIN
	call_if_unset FLAG_SYS_PC_LANETTE, EventScript_AccessSomeonesPC
	call_if_set FLAG_SYS_PC_LANETTE, EventScript_AccessLanettesPC
	msgbox gText_StorageSystemOpened, MSGBOX_DEFAULT
	special ShowPokemonStorageSystemPC
	goto EventScript_PCMainMenu
	end

EventScript_AccessSomeonesPC::
	msgbox gText_AccessedSomeonesPC, MSGBOX_DEFAULT
	return

EventScript_AccessLanettesPC::
	msgbox gText_AccessedLanettesPC, MSGBOX_DEFAULT
	return

EventScript_TurnOffPC::
	setvar VAR_0x8004, PC_LOCATION_OTHER
	playse SE_PC_OFF
	special DoPCTurnOffEffect
	releaseall
	end

EventScript_AccessHallOfFame::
	goto_if_unset FLAG_SYS_GAME_CLEAR, EventScript_TurnOffPC
	playse SE_PC_LOGIN
	special AccessHallOfFamePC
	goto EventScript_AccessPC
	end
