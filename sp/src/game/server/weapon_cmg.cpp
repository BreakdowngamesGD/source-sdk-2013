//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: CMG (Combine Machine Gun) - the Suppressor's heavy support weapon.
//
//=============================================================================//

#include "cbase.h"
#include "basecombatweapon.h"
#include "npcevent.h"
#include "basecombatcharacter.h"
#include "ai_basenpc.h"
#include "player.h"
#include "weapon_cmg.h"
#include "gamerules.h"
#include "game.h"
#include "in_buttons.h"
#include "soundent.h"
#include "ammodef.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Per-bullet NPC damage. 0 = use the ammo definition (sk_npc_dmg_ar2).
ConVar	sk_npc_dmg_cmg("sk_npc_dmg_cmg", "0", FCVAR_REPLICATED, "NPC damage per CMG bullet. 0 = use the AR2 ammo definition.");

// Rounds per trigger pull for NPCs (the Suppressor's longer fire/pause cycle sits on top of this)
ConVar	sk_weapon_cmg_npc_burst_min("sk_weapon_cmg_npc_burst_min", "8");
ConVar	sk_weapon_cmg_npc_burst_max("sk_weapon_cmg_npc_burst_max", "16");

// Seconds between NPC shots
ConVar	sk_weapon_cmg_npc_fire_rate("sk_weapon_cmg_npc_fire_rate", "0.075");

BEGIN_DATADESC(CWeaponCMG)
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST(CWeaponCMG, DT_WeaponCMG)
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS(weapon_cmg, CWeaponCMG);
PRECACHE_WEAPON_REGISTER(weapon_cmg);

//-----------------------------------------------------------------------------
CWeaponCMG::CWeaponCMG()
{
	// A little more reach than the AR2 so he can pin from a distance
	m_fMinRange1 = 0;
	m_fMaxRange1 = 3072;

	// No energy ball
	m_fMinRange2 = 0;
	m_fMaxRange2 = 0;

	m_bAltFiresUnderwater = false;
}

//-----------------------------------------------------------------------------
// Purpose: Player primary fire. Goes straight to the machine gun base so the
//			AR2's EZ1 double-shot override never applies to the CMG.
//-----------------------------------------------------------------------------
void CWeaponCMG::PrimaryAttack(void)
{
	CHLMachineGun::PrimaryAttack();
}

//-----------------------------------------------------------------------------
// Purpose: Heavier, slower-building kick than the AR2
//-----------------------------------------------------------------------------
void CWeaponCMG::AddViewKick(void)
{
#define	CMG_EASY_DAMPEN			0.5f
#define	CMG_MAX_VERTICAL_KICK	6.0f	//Degrees
#define	CMG_SLIDE_LIMIT			4.0f	//Seconds

	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	float flDuration = m_fFireDuration;

	if (g_pGameRules->GetAutoAimMode() == AUTOAIM_ON_CONSOLE)
	{
		flDuration = MIN(flDuration, 0.75f);
	}

	DoMachineGunKick(pPlayer, CMG_EASY_DAMPEN, CMG_MAX_VERTICAL_KICK, flDuration, CMG_SLIDE_LIMIT);
}

//-----------------------------------------------------------------------------
// Purpose: One bullet per event, using the NPC's proficiency-adjusted aim.
//			Damage is left at 0 unless sk_npc_dmg_cmg is set, so the owner's
//			FireBullets() override (e.g. the Suppressor's damage multiplier)
//			can resolve it from the ammo definition.
//-----------------------------------------------------------------------------
void CWeaponCMG::FireNPCPrimaryAttack(CBaseCombatCharacter* pOperator, bool bUseWeaponAngles)
{
	Vector vecShootOrigin, vecShootDir;

	CAI_BaseNPC* npc = pOperator->MyNPCPointer();
	ASSERT(npc != NULL);

	if (bUseWeaponAngles)
	{
		QAngle	angShootDir;
		GetAttachment(LookupAttachment("muzzle"), vecShootOrigin, angShootDir);
		AngleVectors(angShootDir, &vecShootDir);
	}
	else
	{
		vecShootOrigin = pOperator->Weapon_ShootPosition();
		vecShootDir = npc->GetActualShootTrajectory(vecShootOrigin);
	}

	WeaponSoundRealtime(SINGLE_NPC);

	CSoundEnt::InsertSound(SOUND_COMBAT | SOUND_CONTEXT_GUNFIRE, pOperator->GetAbsOrigin(), SOUNDENT_VOLUME_MACHINEGUN, 0.2, pOperator, SOUNDENT_CHANNEL_WEAPON, pOperator->GetEnemy());

	FireBulletsInfo_t info;
	info.m_iShots = 1;
	info.m_vecSrc = vecShootOrigin;
	info.m_vecDirShooting = vecShootDir;
	info.m_vecSpread = VECTOR_CONE_PRECALCULATED;	// proficiency spread is baked into vecShootDir
	info.m_flDistance = MAX_TRACE_LENGTH;
	info.m_iAmmoType = m_iPrimaryAmmoType;
	info.m_iTracerFreq = 2;

	const float flDmg = sk_npc_dmg_cmg.GetFloat();
	if (flDmg > 0.0f)
	{
		info.m_flDamage = flDmg;
		info.m_iPlayerDamage = (int)flDmg;
	}

	pOperator->FireBullets(info);

	// NOTENOTE: Muzzle flash is handled client-side, same as the AR2

	m_iClip1 = m_iClip1 - 1;
}

//-----------------------------------------------------------------------------
// Purpose: Ignore the AR2's alt-fire anim event so a shared model can't throw
//			an energy ball from the CMG.
//-----------------------------------------------------------------------------
void CWeaponCMG::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	if (pEvent->event == EVENT_WEAPON_AR2_ALTFIRE)
		return;

	BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
}

//-----------------------------------------------------------------------------
int CWeaponCMG::GetMinBurst(void)
{
	return sk_weapon_cmg_npc_burst_min.GetInt();
}

int CWeaponCMG::GetMaxBurst(void)
{
	return MAX(sk_weapon_cmg_npc_burst_max.GetInt(), GetMinBurst());
}

float CWeaponCMG::GetFireRate(void)
{
	return sk_weapon_cmg_npc_fire_rate.GetFloat();
}

//-----------------------------------------------------------------------------
// Purpose: Sloppier than the AR2 at every level. He pins, he doesn't snipe.
//			{ spread scale, bias }
//-----------------------------------------------------------------------------
const WeaponProficiencyInfo_t* CWeaponCMG::GetProficiencyValues()
{
	static WeaponProficiencyInfo_t proficiencyTable[] =
	{
		{ 9.0,		0.75	},	// POOR
		{ 6.0,		0.75	},	// AVERAGE
		{ 4.0,		0.85	},	// GOOD
		{ 2.5,		0.75	},	// VERY GOOD
		{ 1.5,		1.0		},	// PERFECT
	};

	COMPILE_TIME_ASSERT(ARRAYSIZE(proficiencyTable) == WEAPON_PROFICIENCY_PERFECT + 1);

	return proficiencyTable;
}