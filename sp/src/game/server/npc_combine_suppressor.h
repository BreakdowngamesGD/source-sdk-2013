//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Suppressor - A heavily armored Combine soldier that stands
//			its ground and pins enemies down with a minigun while others flank.
//
//=============================================================================//

#ifndef NPC_COMBINE_SUPPRESSOR_H
#define NPC_COMBINE_SUPPRESSOR_H

#ifdef _WIN32
#pragma once
#endif

#include "npc_combines.h"

//=========================================================
//	>> CNPC_CombineSuppressor
//=========================================================
class CNPC_CombineSuppressor : public CNPC_CombineS
{
	DECLARE_CLASS(CNPC_CombineSuppressor, CNPC_CombineS);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	void		Precache(void);
	void		PostNPCInit(void);
	void		PrescheduleThink(void);
	int			SelectSchedule(void);
	int			SelectFailSchedule(int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode);
	int			TranslateSchedule(int scheduleType);
	void		BuildScheduleTestBits(void);
	WeaponProficiency_t	CalcWeaponProficiency(CBaseCombatWeapon* pWeapon);

	// Scales NPC bullet damage (suppressor_damage_multiplier)
	void		FireBullets(const FireBulletsInfo_t& info);

	// Voice (MAPBASE signatures, matching npc_combine.cpp)
	void		PainSound(const CTakeDamageInfo& info);
	void		DeathSound(const CTakeDamageInfo& info);
	void		IdleSound(void);
	void		LostEnemySound(CBaseEntity* pEnemy);
	void		FoundEnemySound(CBaseEntity* pEnemy);
	void		NotifyDeadFriend(CBaseEntity* pFriend);

private:
	int			SelectSuppressSchedule(void);
	int			SelectInvestigateSchedule(void);

	bool		ShouldBackAway(CBaseEntity* pEnemy);
	bool		UpdateBurstCycle(bool bEnemySeen);	// returns true if he should hold fire right now
	bool		IsInBurstPause(void) const;

	bool		PlayVoice(int iVoice, bool bForce = false);
	void		UpdateVoice(void);

	// Burst cycle state
	bool		m_bInBurst;
	float		m_flBurstEndTime;
	float		m_flNextBurstTime;

	// Back-away state
	float		m_flNextBackAwayTime;

	// Investigation state
	float		m_flNextInvestigateTime;

	// Voice state
	float		m_flNextVoiceTime;
	float		m_flNextPainVoiceTime;
	float		m_flNextCombatIdleTime;
	bool		m_bNearPanicSpoken;
	bool		m_bKillSpoken;
};

#endif // NPC_COMBINE_SUPPRESSOR_H