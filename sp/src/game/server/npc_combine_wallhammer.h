//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Wallhammer - A heavy combine soldier with a deployable shield
//
//=============================================================================//

#ifndef NPC_COMBINE_WALLHAMMER_H
#define NPC_COMBINE_WALLHAMMER_H

#ifdef _WIN32
#pragma once
#endif

#include "npc_combines.h"

//=========================================================
//	>> CNPC_CombineWallhammer
//=========================================================
class CNPC_CombineWallhammer : public CNPC_CombineS
{
	DECLARE_CLASS(CNPC_CombineWallhammer, CNPC_CombineS);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	void		Precache(void);
	void		PostNPCInit(void);
	void		PrescheduleThink(void);
	void		Event_Killed(const CTakeDamageInfo& info);
	int			OnTakeDamage_Alive(const CTakeDamageInfo& info);
	int			SelectSchedule(void);
	int			SelectInvestigateSchedule(void);
	int			TranslateSchedule(int scheduleType);
	void		BuildScheduleTestBits(void);
	Vector		Weapon_ShootPosition(void);
	WeaponProficiency_t	CalcWeaponProficiency(CBaseCombatWeapon* pWeapon);

	// Hitscan damage (bullets, melee) arrives here with the trace, so we can
	// tell whether the shot went THROUGH the shield before reaching us.
	void		TraceAttack(const CTakeDamageInfo& info, const Vector& vecDir, trace_t* ptr, CDmgAccumulator* pAccumulator = NULL);

	// Scales NPC bullet damage (wallhammer_damage_multiplier)
	void		FireBullets(const FireBulletsInfo_t& info);

	// Shield system
	bool		IsShieldActive(void) { return m_bShieldActive; }
	bool		ShieldInterceptsRay(const Vector& vecStart, const Vector& vecEnd, Vector* pvecHit = NULL);
	void		ActivateShield(void);
	void		DeactivateShield(void);
	void		UpdateShieldVisuals(void);

	// Flash effect system
	void		TriggerFlashEffect(void);

	// Wallhammer-specific behaviors
	void		DestroyPropsNearby(void);

	// Voice (overrides use the MAPBASE signatures, matching npc_combine.cpp)
	void		PainSound(const CTakeDamageInfo& info);
	void		DeathSound(const CTakeDamageInfo& info);
	void		IdleSound(void);
	void		LostEnemySound(CBaseEntity* pEnemy);
	void		NotifyDeadFriend(CBaseEntity* pFriend);

private:
	int			CountHostilesInRange(float flRange, float flMinDot);
	void		GetShieldBounds(Vector& mins, Vector& maxs);
	void		RemoveShieldProp(void);
	bool		ShouldRushEnemyLKP(void);
	bool		ShouldCloseDistance(void);

	bool		PlayVoice(int iVoice, bool bForce = false);
	void		UpdateVoice(void);

	// Shield state
	bool		m_bShieldActive;
	float		m_flNextFlashTime;		// Flash may fire again after this time
	float		m_flShieldCooldown;		// While active: time the shield ends. While inactive: time it may be used again.
	float		m_flNextShieldThink;	// Throttles the (relatively expensive) entity queries
	// Investigation state
	float		m_flNextInvestigateTime;
	CHandle<CBaseEntity> m_hShieldProp;

	// Shield visual state
	int			m_iShieldAlpha;
	float		m_flShieldFadeTime;
	bool		m_bShieldFadingOut;

	// Voice state
	float		m_flNextVoiceTime;
	float		m_flNextPainVoiceTime;
	float		m_flNextCombatIdleTime;
	bool		m_bNearPanicSpoken;
	bool		m_bKillSpoken;
};

#endif // NPC_COMBINE_WALLHAMMER_H