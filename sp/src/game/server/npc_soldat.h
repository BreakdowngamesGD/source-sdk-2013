//=============================================================================//
// Purpose: npc_soldat - Russian Ground Forces soldier for Entropy : Zero 2.
//
//   A C++ port of the useful parts of the V.A.C. GMod soldier brain
//   (sv_brain / sv_mind / sv_squadmind), RU side only:
//     * roles (leader, rifleman, autorifle, grenadier, medic, marksman, rto,
//       at, sniper, hsniper, breacher) each with its own weapon from the
//       weapon_css_* set AND its own combat behaviour
//     * per-man persona (skill, nerve, fear, discipline) and a Russian voice
//     * suppression, morale, panic (flee or cower)
//     * "engage first" on close contacts, startle on gunfire while at ease
//     * leader: orders squadmates to advance on a hiding enemy, rallies shaken
//       men; squad falls apart when he dies (unless the RTO takes command)
//     * Russian deliberate assault at squad level (see UpdateAssaultPhase): the
//       leader runs support-by-fire -> probe -> hold -> exploit phases. The
//       support element (autorifle, marksman, snipers, Suppressor) fixes the
//       enemy; the assault element (riflemen, grenadiers, AT, breacher) advances
//       in small bounding teams of up to 3 on the leader's call, goes to ground
//       if the probe stalls, and floods through a breach with a grenade first;
//       medic / RTO stay back as security. If the leader dies (or the squad has
//       none) command passes down s_CommandSuccession in the .cpp, so the
//       assault keeps running. Soldats auto-squad with nearby soldats
//       (npc_soldat_auto_squad, keyvalue soldatautosquad).
//     * fire and manoeuvre: movers push forward while a base of fire pins
//     * idle chatter: relaxed soldiers talk amongst themselves
//     * sidearm swap when the primary runs dry with the enemy close
//   Not ported (no content / no vehicles): vehicles, ropes, smoke, carry,
//   wounded/drag/surrender, doors, fire teams bounding.
//=============================================================================//

#ifndef NPC_SOLDAT_H
#define NPC_SOLDAT_H
#ifdef _WIN32
#pragma once
#endif

#include "npc_combines.h"
#include "npc_combine_hla.h"

// Hammer keyvalue "soldatrole" (choices) - order matches s_Roles in the .cpp.
// FGD: add   10 : "Breacher (shotgun)"
enum SoldatRole_t
{
	SOLDAT_ROLE_RIFLEMAN = 0,
	SOLDAT_ROLE_LEADER,
	SOLDAT_ROLE_AUTORIFLE,
	SOLDAT_ROLE_GRENADIER,
	SOLDAT_ROLE_MEDIC,
	SOLDAT_ROLE_MARKSMAN,
	SOLDAT_ROLE_RTO,
	SOLDAT_ROLE_AT,
	SOLDAT_ROLE_SNIPER,
	SOLDAT_ROLE_HSNIPER,
	SOLDAT_ROLE_BREACHER,

	SOLDAT_ROLE_COUNT
};

// Hammer keyvalue "soldatautosquad" (choices): -1 "Default (npc_soldat_auto_squad)", 0 "No - stay out of auto squads", 1 "Yes"
// Soldats with no squadname of their own group up with nearby soldats unless this is 0.
bool npc_soldat_auto_squad_enabled();

// Squad assault phase, owned by the squad's commander (leader, or the next in the succession list)
enum SoldatAssaultPhase_t
{
	SOLDAT_PHASE_NONE = 0,
	SOLDAT_PHASE_SUPPORT,		// support element fixes the enemy, assault element takes position
	SOLDAT_PHASE_PROBE,			// assault teams bound forward under covering fire
	SOLDAT_PHASE_HOLD,			// probe stalled: go to ground, keep the position, support keeps firing
	SOLDAT_PHASE_EXPLOIT,		// breach made: everyone in the assault element goes in
	SOLDAT_PHASE_SWEEP,		// enemy gone to ground: assault element splits into pairs and searches round the last known position
};

// Hammer keyvalue "soldatpatrol" (choices): -1 "Default (npc_soldat_patrol)", 0 "Guard - never patrol", 1 "Patrol with the team"
// A soldat with any ai_goal behaviour (assault / standoff / follow / lead), or in a script, is treated as guarding too.

// What a station is for (see SetStation)
enum SoldatStationKind_t
{
	SOLDAT_STATION_NONE = 0,
	SOLDAT_STATION_PATROL,		// idle: slot in the patrol leader's formation
	SOLDAT_STATION_COMBAT,		// fight: slot on the commander's skirmish line
};

enum SoldatFormation_t
{
	SOLDAT_FORM_LINE = 0,		// abreast, facing the enemy
	SOLDAT_FORM_COLUMN,			// staggered file behind the leader
	SOLDAT_FORM_WEDGE,			// arrowhead, leader at the tip
	SOLDAT_FORM_CIRCLE,			// all-round at the halt
};

// was: class CNPC_Soldat : public CNPC_CombineS
class CNPC_Soldat : public CNPC_CombineHLABase
{
	DECLARE_CLASS(CNPC_Soldat, CNPC_CombineHLABase);
	DECLARE_DATADESC();

public:
	CNPC_Soldat();

	// Player-squad membership is a convar (npc_soldat_player_ally)
	virtual bool	CanBePlayerSquadmate();
	Class_T			Classify(void);

	void			Spawn(void);
	void			Precache(void);
	void			PostNPCInit(void);
	void			PrescheduleThink(void);
	void			OnListened(void);
	void			OnStartSchedule(int scheduleType);
	int				SelectSchedule(void);
	void			BuildScheduleTestBits(void);
	void			Event_Killed(const CTakeDamageInfo& info);
	int				OnTakeDamage_Alive(const CTakeDamageInfo& info);
	WeaponProficiency_t	CalcWeaponProficiency(CBaseCombatWeapon* pWeapon);
	int				DrawDebugTextOverlays(void);
	void			StartTask(const Task_t* pTask);		// patrol stations are walked, not run
	int				SelectFailSchedule(int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode);

	// Only the player decides who is in his squad (E on a soldat). Nothing in this class adds or removes anyone.
	bool			InPlayerSquad();

	// Russian voice (sound/vac_alife/ru/<voice>/<category>/NN.wav through
	// the VAC_RU.<voice>.<category> soundscripts)
	void			PainSound(const CTakeDamageInfo& info);
	void			DeathSound(const CTakeDamageInfo& info);
	void			IdleSound(void);
	void			AlertSound(void);
	void			LostEnemySound(CBaseEntity* pEnemy);
	void			FoundEnemySound(CBaseEntity* pEnemy);
	void			NotifyDeadFriend(CBaseEntity* pFriend);
	void			FollowSound();
	void			StopFollowSound();

	bool			PlayVoice(const char* pszCategory, bool bForce = false, float flGap = -1.0f);

	// Medic: a hurt squadmate calls him over (see OnTakeDamage_Alive)
	void			RequestHeal(CBaseEntity* pWho);
	void			UpdateOnRemove(void);

	// Called by the dying leader on his squadmates
	void			OnLeaderLost(WeaponProficiency_t prof, float flStun);

	int				GetSoldatRole() const { return m_iRole; }
	bool			IsLeaderRole() const { return m_iRole == SOLDAT_ROLE_LEADER || m_bActingLeader; }

	// Special infantry: fixes the enemy with fire from where he stands
	bool			IsSupportRole() const { return m_iRole == SOLDAT_ROLE_AUTORIFLE || m_iRole == SOLDAT_ROLE_MARKSMAN || m_iRole == SOLDAT_ROLE_SNIPER || m_iRole == SOLDAT_ROLE_HSNIPER; }
	// Assault element: the men who actually close with the enemy
	bool			IsAssaultRole() const { return m_iRole == SOLDAT_ROLE_RIFLEMAN || m_iRole == SOLDAT_ROLE_GRENADIER || m_iRole == SOLDAT_ROLE_AT || m_iRole == SOLDAT_ROLE_BREACHER; }
	int				GetAssaultPhase() const { return m_iAssaultPhase; }

	// Stock Combine speech (response system) still plays unless you define
	// NPC_SOLDAT_SILENCE_STOCK_VO and the signature below matches your
	// ai_speech.h - see the .cpp.
#ifdef NPC_SOLDAT_SILENCE_STOCK_VO
	virtual bool	SpeakDispatchResponse(AIConcept_t concept, AI_Response* response, AI_CriteriaSet* criteria, IRecipientFilter* filter = NULL) { return false; }
#endif

protected:
	virtual int		SelectCombatSchedule();
	virtual bool	ShouldInvestigateSound(CSound* pSound, CBaseEntity* pOwner);

private:
	// feelings
	void			UpdateFeelings(float dt);
	void			AddSuppression(float amount);
	void			UpdateProficiency();
	bool			SquadHasAT();

	// tactics
	void			UpdateWeaponSwap();
	void			UpdateLeaderOrders();
	void			UpdateLeaderRally();
	bool			IssueAdvanceOrder();
	void			HandleLeaderDown();
	bool			IsHeavyEnemy(CBaseEntity* pEnemy);
	const char* ContactCategory(CBaseEntity* pEnemy);

	// role behaviour
	int				SelectRoleSchedule();
	bool			BurstHold(float flOnMin, float flOnMax, float flOffMin, float flOffMax);
	bool			InBurstPause() const;
	int				RelocateToCover(float flCooldown);
	int				BackAway();
	bool			SquadHasBaseOfFire();

	// squad assault doctrine
	void			UpdateAssaultPhase();
	void			SetAssaultPhase(int iPhase);
	CNPC_Soldat* FindAssaultCommander(CNPC_Soldat* pExclude = NULL);
	void			UpdateCommandSuccession();

	// auto squad
	bool			AutoSquadEnabled() const { return m_iAutoSquad < 0 ? npc_soldat_auto_squad_enabled() : m_iAutoSquad != 0; }
	void			UpdateAutoSquad();
	int				GetAssaultTeam(CNPC_Soldat* pCmd, int* pnTeams);
	int				SelectDoctrineSchedule(CNPC_Soldat* pCmd, bool bSeen, float flDist, bool bHeavy, CBaseEntity* pEnemy);
	void			BoostSquadMorale(float flAmount);
	CNPC_Soldat* FindSquadMedic();

	// voice
	void			UpdateVoice();

	// medic: hands medkits to the player (tossed item_healthkit), heals hurt squadmates directly
	void			UpdateMedic();
	CBaseEntity* FindHealTarget();
	bool			MedicInRange(CBaseEntity* pTarget);
	void			DoHeal(CBaseEntity* pTarget);
	void			TossMedkit(CBaseEntity* pTarget);

	// room clearing: assault element splits into pairs that approach from different sides, then enter in turn
	void			UpdateSweep();
	int				GetSweepSlot(CNPC_Soldat* pCmd, int* pnTeams, int* pSlot);
	void			PlanSweep(CNPC_Soldat* pCmd, int iTeam, int nTeams, int iSlot);
	Vector			SweepSnap(const Vector& vecPos);
	void			SetSweepGoal(const Vector& vecPos);
	void			ReleaseSweep();

	// idle chatter
	bool			IsRelaxed();
	CNPC_Soldat* FindChatPartner();
	void			UpdateIdleChatter();
	void			UpdateChatReply();

	// accuracy (role + skill + suppression, capped while shaken by the leader's death)
	int				ComputeProficiency();

	// player squad bookkeeping
	void			UpdatePlayerSquadWatch();

	// stations: a world position a man is told to hold (info_target + stock SCHED_TARGET_CHASE)
	void			SetStation(const Vector& vecPos, int iKind, float flHold);
	void			ClearStation();
	void			UpdateStation();
	bool			AtStation();
	int				SelectStationSchedule(bool bSeen);			// combat
	int				SelectPatrolStationSchedule();				// idle
	Vector			ClampStation(const Vector& vecFrom, const Vector& vecRaw);
	Vector			SnapToNode(const Vector& vecPos, bool* pbFound);

	// combat formation (commander)
	int				CollectAssault(CNPC_Soldat** pOut, int nMax);
	void			AssignCombatStations(const Vector& vecEnemy);
	void			BeginBound(const Vector& vecEnemy, int nAssault);

	// idle team patrol
	bool			HasMapDuty();
	bool			CanPatrol();
	CNPC_Soldat* FindPatrolLeader();
	int				CollectPatrollers(CNPC_Soldat** pOut, int nMax);
	void			UpdatePatrol();
	void			LeaderPatrol();
	bool			PickPatrolGoal();

	// who he is
	int				m_iRole;				// keyvalue "soldatrole"
	int				m_iVoice;				// keyvalue "soldatvoice" (-1 = by entity index)
	int				m_iAutoSquad;			// keyvalue "soldatautosquad": -1 = npc_soldat_auto_squad convar, 0 = never, 1 = always
	float			m_flNextAutoSquadT;
	float			m_flNextCommandCheckT;
	float			m_flSkill;				// aim error multiplier (~1 trained)
	float			m_flNerve;				// how much he takes before he breaks
	float			m_flFear;				// 0 - 1
	float			m_flDiscipline;			// 0 - 1
	string_t		m_iszPrimary;			// the weapon he started with
	bool			m_bActingLeader;		// RTO who took command after the leader fell

	// how he feels
	float			m_flMorale;				// 1 = fine, <0.12 panic
	float			m_flSuppr;				// 0 - 1
	float			m_flLastThinkT;
	float			m_flLastSupprHeardT;
	bool			m_bPanicking;
	bool			m_bPanicFlee;
	bool			m_bPanicSpoken;
	int				m_iLastProficiency;

	// tactics timers
	float			m_flEngageFirstUntil;
	float			m_flStartleUntil;
	float			m_flNextOrderT;
	float			m_flNextWeaponCheckT;
	float			m_flPistolNearT;

	// role behaviour state
	bool			m_bInBurst;
	float			m_flBurstEndT;
	float			m_flBurstPauseT;
	float			m_flNextBackAwayT;
	float			m_flNextRelocateT;
	float			m_flLastHitT;
	float			m_flPushUntilT;
	float			m_flNextPushCheckT;
	float			m_flNextGrenadeT;
	float			m_flNextRallyT;
	float			m_flNextMedicCallT;

	// assault doctrine (only meaningful on the commander)
	int				m_iAssaultPhase;
	float			m_flPhaseStartT;
	float			m_flNextPhaseThinkT;
	int				m_iBoundTeam;			// which assault team is moving right now
	float			m_flNextBoundT;
	Vector			m_vecStrongpoint;		// where the enemy position was last marked
	int				m_nAssaultStart;		// assault-element strength when the phase cycle began
	bool			m_bHasSupport;			// squad has a support element

	// voice timers
	float			m_flNextVoiceT;
	float			m_flNextRadioT;
	float			m_flNextIdleTalkT;
	bool			m_bKillSpoken;
	bool			m_bPrecached;

	// medic state
	EHANDLE			m_hHealTarget;
	float			m_flNextHealCheckT;
	float			m_flHealGiveUpT;
	float			m_flNextHealKickT;
	float			m_flNextHealPlayerT;
	float			m_flNextHealAllyT;

	// sweep state (commander keeps m_vecSweepCenter, everyone else reads it)
	Vector			m_vecSweepCenter;
	EHANDLE			m_hSearchPoint;			// info_target we send our follow behaviour to
	Vector			m_vecSweepPt[3];		// approach, enter, far side
	bool			m_bSweepActive;
	int				m_iSweepStage;			// 0 approach, 1 enter, 2 push through, 3 done
	int				m_iSweepTeam;
	float			m_flSweepReachedT;
	float			m_flSweepSuppressT;
	float			m_flNextSweepThinkT;

	// idle chatter state
	float			m_flNextChatT;
	float			m_flChatReplyT;			// >0 = I owe a reply at this time
	EHANDLE			m_hChatPartner;
	int				m_iChatLeft;			// further lines allowed after my next reply

	// accuracy penalty after the leader falls
	float			m_flShakenUntilT;
	int				m_iShakenProf;
	float			m_flNextSweepAllowedT;	// commander: no new sweep before this

	bool			m_bWasInPlayerSquad;

	// station
	EHANDLE			m_hStationPt;
	Vector			m_vecStation;
	bool			m_bStation;
	int				m_iStationKind;
	float			m_flStationUntilT;		// expires unless the commander / patrol leader refreshes it
	float			m_flStationBlockedT;	// path failed: ignore stations until then
	float			m_flNextStationKickT;
	float			m_flAdvanceM;			// metres ahead of the commander's line (bounding)
	float			m_flFrontM;				// commander: furthest advance so far
	bool			m_bBoundPending;		// commander: start the first bound of a probe

	// patrol (state lives on the patrol leader)
	int				m_iPatrol;				// keyvalue "soldatpatrol": -1 convar, 0 guard, 1 patrol
	Vector			m_vecPatrolGoal;
	Vector			m_vecPatrolDir;
	bool			m_bPatrolMoving;
	float			m_flPatrolRestUntilT;
	float			m_flPatrolRestStartT;
	float			m_flPatrolGiveUpT;
	float			m_flNextPatrolThinkT;
	int				m_iPatrolForm;
};

#endif // NPC_SOLDAT_H