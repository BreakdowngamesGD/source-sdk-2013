//=============================================================================//
// Purpose: npc_soldat - Russian Ground Forces soldier for Entropy : Zero 2.
//
//   A C++ port of the useful parts of the V.A.C. GMod soldier brain
//   (sv_brain / sv_mind / sv_squadmind), RU side only:
//     * roles (leader, rifleman, autorifle, grenadier, medic, marksman, rto,
//       at, sniper, hsniper) with guns-list weapons only
//     * per-man persona (skill, nerve, fear, discipline) and a Russian voice
//     * suppression, morale, panic (flee or cower)
//     * "engage first" on close contacts, startle on gunfire while at ease
//     * leader: orders squadmates to advance on a hiding enemy; squad falls
//       apart when he dies
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

// Hammer keyvalue "soldatrole" (choices) - order matches s_Roles in the .cpp
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

	SOLDAT_ROLE_COUNT
};

class CNPC_Soldat : public CNPC_CombineS
{
	DECLARE_CLASS(CNPC_Soldat, CNPC_CombineS);
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
	void			Event_Killed(const CTakeDamageInfo& info);
	int				OnTakeDamage_Alive(const CTakeDamageInfo& info);
	WeaponProficiency_t	CalcWeaponProficiency(CBaseCombatWeapon* pWeapon);
	int				DrawDebugTextOverlays(void);

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

	// Called by the dying leader on his squadmates
	void			OnLeaderLost(WeaponProficiency_t prof, float flStun);

	int				GetSoldatRole() const { return m_iRole; }
	bool			IsLeaderRole() const { return m_iRole == SOLDAT_ROLE_LEADER; }

	// Stock Combine speech (response system) still plays unless you define
	// NPC_SOLDAT_SILENCE_STOCK_VO and the signature below matches your
	// ai_speech.h - see the .cpp.
#ifdef NPC_SOLDAT_SILENCE_STOCK_VO
	virtual bool	SpeakDispatchResponse(AIConcept_t concept, AI_Response* response, AI_CriteriaSet* criteria, IRecipientFilter* filter = NULL) { return false; }
#endif

protected:
	virtual int		SelectCombatSchedule();

private:
	// feelings
	void			UpdateFeelings(float dt);
	void			AddSuppression(float amount);
	void			UpdateProficiency();
	bool			SquadHasAT();

	// tactics
	void			UpdateWeaponSwap();
	void			UpdateLeaderOrders();
	bool			IssueAdvanceOrder();
	void			HandleLeaderDown();
	bool			IsHeavyEnemy(CBaseEntity* pEnemy);
	const char* ContactCategory(CBaseEntity* pEnemy);

	// voice
	void			UpdateVoice();

	// who he is
	int				m_iRole;				// keyvalue "soldatrole"
	int				m_iVoice;				// keyvalue "soldatvoice" (-1 = by entity index)
	float			m_flSkill;				// aim error multiplier (~1 trained)
	float			m_flNerve;				// how much he takes before he breaks
	float			m_flFear;				// 0 - 1
	float			m_flDiscipline;			// 0 - 1
	string_t		m_iszPrimary;			// the weapon he started with

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

	// voice timers
	float			m_flNextVoiceT;
	float			m_flNextRadioT;
	float			m_flNextIdleTalkT;
	bool			m_bKillSpoken;
	bool			m_bPrecached;
};

#endif // NPC_SOLDAT_H