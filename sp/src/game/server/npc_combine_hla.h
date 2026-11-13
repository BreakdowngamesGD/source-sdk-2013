//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Unified header for the HLA Combine NPCs:
//
//			CNPC_CombineHLABase   - shared base (sound investigation)
//			CNPC_CombineGrunt     - plain soldier, bodyguards the VIPs
//			CNPC_CombineOrdinal   - squad leader, manhacks + advance orders
//			CNPC_CombineWallhammer- shield / flash heavy
//			CNPC_CombineSuppressor- minigun heavy
//
//			All four derive from CNPC_CombineHLABase, which makes them walk to
//			investigate gunfire, footsteps (player running, etc.) and world
//			noises from things they haven't seen yet.
//
//=============================================================================//

#ifndef NPC_COMBINE_HLA_H
#define NPC_COMBINE_HLA_H

#ifdef _WIN32
#pragma once
#endif

#include "npc_combines.h"

class CSound;

//=========================================================
//	>> CNPC_CombineHLABase
//
//	Shared behavior for every HLA Combine. Implemented in
//	npc_combine_hla.cpp.
//=========================================================
class CNPC_CombineHLABase : public CNPC_CombineS
{
	DECLARE_CLASS(CNPC_CombineHLABase, CNPC_CombineS);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	int			SelectSchedule(void);
	void		BuildScheduleTestBits(void);

	// Make sure combat / player / world sounds actually raise COND_HEAR_* on us
	int			GetSoundInterests(void);

protected:
	// Returns SCHED_INVESTIGATE_SOUND or SCHED_NONE
	int			SelectInvestigateSchedule(void);

	// Subclass hook: veto a particular sound (the grunt uses it to stay near its VIP)
	virtual bool	ShouldInvestigateSound(CSound* pSound, CBaseEntity* pOwner) { return true; }

	// In a squad, only the closest idle member goes to look
	bool		IsBestSquadInvestigator(const Vector& vecSound);

	float		m_flNextInvestigateTime;
};

//=========================================================
//	>> CNPC_CombineGrunt
//=========================================================
class CNPC_CombineGrunt : public CNPC_CombineHLABase
{
	DECLARE_CLASS(CNPC_CombineGrunt, CNPC_CombineHLABase);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	void		PrescheduleThink(void);

	// Used by other grunts to spread themselves across VIPs
	CBaseEntity* GetProtectee(void) { return m_hProtectee.Get(); }

protected:
	bool		ShouldInvestigateSound(CSound* pSound, CBaseEntity* pOwner);

private:
	CBaseEntity* FindBestProtectee(bool bOrdinalOnly);
	int			CountProtectors(CBaseEntity* pVip);
	void		SetProtectee(CBaseEntity* pVip);
	void		JoinProtecteeSquad(CBaseEntity* pVip);

	EHANDLE		m_hProtectee;
	float		m_flNextProtecteeCheck;
	float		m_flNextOrdinalCheck;
};

//=========================================================
//	>> CNPC_CombineOrdinal
//=========================================================
class CNPC_CombineOrdinal : public CNPC_CombineHLABase
{
	DECLARE_CLASS(CNPC_CombineOrdinal, CNPC_CombineHLABase);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	void		Precache(void);
	void		PostNPCInit(void);
	void		PrescheduleThink(void);
	void		Event_Killed(const CTakeDamageInfo& info);
	WeaponProficiency_t	CalcWeaponProficiency(CBaseCombatWeapon* pWeapon);

	// Scales NPC bullet damage (ordinal_damage_multiplier)
	void		FireBullets(const FireBulletsInfo_t& info);

	// Voice (MAPBASE signatures, matching npc_combine.cpp).
	// When ordinal_voice_enable is 0 these fall through to the stock Combine voice.
	void		PainSound(const CTakeDamageInfo& info);
	void		DeathSound(const CTakeDamageInfo& info);
	void		IdleSound(void);
	void		LostEnemySound(CBaseEntity* pEnemy);
	void		FoundEnemySound(CBaseEntity* pEnemy);
	void		NotifyDeadFriend(CBaseEntity* pFriend);

private:
	// Manhacks
	void		UpdateManhackState(void);

	// Squad command
	void		UpdateSquadOrders(void);
	bool		IssueAdvanceOrder(void);
	void		HandleLeaderDown(void);

	// Voice
	bool		PlayVoice(int iVoice, bool bForce = false);
	bool		PlayVoiceOn(CBaseEntity* pSpeaker, int iVoice, bool bForce);
	void		UpdateVoice(void);

	// Manhack state
	bool		m_bHadManhack;			// Had a live manhack last think
	float		m_flNextManhackTime;	// Next time a manhack may be deployed
	bool		m_bManhackFlagByUs;		// We (not the mapper) set SF_COMBINE_NO_MANHACK_DEPLOY

	// Order state
	float		m_flNextOrderTime;

	// Voice state
	float		m_flNextVoiceTime;
	float		m_flNextPainVoiceTime;
	float		m_flNextCombatIdleTime;
	bool		m_bNearPanicSpoken;
	bool		m_bKillSpoken;
};

//=========================================================
//	>> CNPC_CombineWallhammer
//=========================================================
class CNPC_CombineWallhammer : public CNPC_CombineHLABase
{
	DECLARE_CLASS(CNPC_CombineWallhammer, CNPC_CombineHLABase);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	void		Precache(void);
	void		PostNPCInit(void);
	void		PrescheduleThink(void);
	void		Event_Killed(const CTakeDamageInfo& info);
	int			OnTakeDamage_Alive(const CTakeDamageInfo& info);
	int			SelectSchedule(void);
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

//=========================================================
//	>> CNPC_CombineSuppressor
//=========================================================
class CNPC_CombineSuppressor : public CNPC_CombineHLABase
{
	DECLARE_CLASS(CNPC_CombineSuppressor, CNPC_CombineHLABase);
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

	// Voice state
	float		m_flNextVoiceTime;
	float		m_flNextPainVoiceTime;
	float		m_flNextCombatIdleTime;
	bool		m_bNearPanicSpoken;
	bool		m_bKillSpoken;
};

#endif // NPC_COMBINE_HLA_H