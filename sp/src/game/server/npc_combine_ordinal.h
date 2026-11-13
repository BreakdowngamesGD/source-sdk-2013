//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Ordinal - Overwatch Commando squad leader. Deploys manhacks
//			to flush out hiding enemies, barks orders that make squadmates push
//			the enemy, and leaves the squad disorganized when he dies.
//
//=============================================================================//

#ifndef NPC_COMBINE_ORDINAL_H
#define NPC_COMBINE_ORDINAL_H

#ifdef _WIN32
#pragma once
#endif

#include "npc_combines.h"

//=========================================================
//	>> CNPC_CombineOrdinal
//=========================================================
class CNPC_CombineOrdinal : public CNPC_CombineS
{
	DECLARE_CLASS(CNPC_CombineOrdinal, CNPC_CombineS);
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

#endif // NPC_COMBINE_ORDINAL_H