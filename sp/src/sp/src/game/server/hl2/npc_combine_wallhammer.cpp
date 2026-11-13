//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Wallhammer - A heavy combine soldier with a deployable shield
//
//=============================================================================//

#include "cbase.h"
#include "ai_hull.h"
#include "ai_motor.h"
#include "npc_combine_wallhammer.h"
#include "npcevent.h"
#include "engine/IEngineSound.h"
#include "explode.h"
#include "props.h"
#include "entitylist.h"
#include "player.h"
#include "basecombatweapon.h"
#include "ammodef.h"
#include "IEffects.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS(npc_combine_wallhammer, CNPC_CombineWallhammer);

// Convars
ConVar	sk_wallhammer_health("sk_wallhammer_health", "360");
ConVar	wallhammer_shield_duration("wallhammer_shield_duration", "5.5");
ConVar	wallhammer_shield_cooldown("wallhammer_shield_cooldown", "8");
ConVar	wallhammer_damage_multiplier("wallhammer_damage_multiplier", "1.9");
ConVar	wallhammer_detection_range("wallhammer_detection_range", "200");
ConVar	wallhammer_detection_dot("wallhammer_detection_dot", "-1", FCVAR_NONE, "Hostiles must be within this dot product of my facing to raise the shield. -1 = any direction, 0 = front half, 0.3 = narrow cone.");
ConVar	wallhammer_flash_range("wallhammer_flash_range", "120");
ConVar	wallhammer_prop_destroy_range("wallhammer_prop_destroy_range", "80");
ConVar	wallhammer_prop_crush_distance("wallhammer_prop_crush_distance", "50");
ConVar	wallhammer_shield_block_dot("wallhammer_shield_block_dot", "0.5", FCVAR_NONE, "Damage coming from within this dot product of my facing is blocked by the shield. 0.5 = ~60 degree arc each side.");
ConVar	wallhammer_debug("wallhammer_debug", "0", FCVAR_NONE, "Print shield/damage debug info.");

#define WALLHAMMER_SHIELD_MODEL		"models/ovr_load/force_shield.mdl"
#define WALLHAMMER_SHIELD_MAX_ALPHA	150
#define WALLHAMMER_SHIELD_FADE_TIME	0.6f
#define WALLHAMMER_THINK_INTERVAL	0.1f

BEGIN_DATADESC(CNPC_CombineWallhammer)
DEFINE_FIELD(m_bShieldActive, FIELD_BOOLEAN),
DEFINE_FIELD(m_bFlashUsed, FIELD_BOOLEAN),
DEFINE_FIELD(m_flShieldCooldown, FIELD_TIME),
DEFINE_FIELD(m_flNextShieldThink, FIELD_TIME),
DEFINE_FIELD(m_hShieldProp, FIELD_EHANDLE),
DEFINE_FIELD(m_iShieldAlpha, FIELD_INTEGER),
DEFINE_FIELD(m_flShieldFadeTime, FIELD_TIME),
DEFINE_FIELD(m_bShieldFadingOut, FIELD_BOOLEAN),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::Spawn(void)
{
	// Set default model if not specified. Must happen before BaseClass::Spawn(),
	// which calls Precache() and SetModel() for us.
	if (!GetModelName() || GetModelName() == NULL_STRING)
	{
		SetModelName(MAKE_STRING("models/hl2_combine_wallhammer.mdl"));
	}

	// Equip the shotgun through the normal spawn-equipment path so the weapon is
	// set up (capabilities, proficiency, animation translation) during NPCInit.
	if (m_spawnEquipment == NULL_STRING)
	{
		m_spawnEquipment = MAKE_STRING("weapon_shotgun");
	}

	// Initialize shield system
	m_bShieldActive = false;
	m_bFlashUsed = false;
	m_flShieldCooldown = gpGlobals->curtime;
	m_flNextShieldThink = 0.0f;
	m_hShieldProp = NULL;
	m_iShieldAlpha = 0;
	m_flShieldFadeTime = 0.0f;
	m_bShieldFadingOut = false;

	// Tactical variant: no grenades
	m_iNumGrenades = 0;

	BaseClass::Spawn();

	// CNPC_CombineS::Spawn() overwrites health with sk_combine_s_health,
	// so the Wallhammer's health has to be applied AFTER the base spawn.
	SetHealth(sk_wallhammer_health.GetFloat());
	SetMaxHealth(sk_wallhammer_health.GetFloat());
}

//-----------------------------------------------------------------------------
// Purpose: Precache all resources
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::Precache()
{
	PrecacheModel("models/hl2_combine_wallhammer.mdl");
	PrecacheModel(WALLHAMMER_SHIELD_MODEL);

	// These are raw .wav paths, not soundscript names, so use PrecacheSound
	PrecacheSound("npc/attack_helicopter/aheli_mine_drop1.wav");
	PrecacheSound("weapons/flashbang/flashbang_explode1.wav");

	UTIL_PrecacheOther("prop_dynamic_override");

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: Post-spawn initialization
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::PostNPCInit()
{
	// Fallback in case spawn equipment was explicitly cleared and nothing else equipped
	if (!GetActiveWeapon())
	{
		GiveWeapon(MAKE_STRING("weapon_shotgun"));
	}

	BaseClass::PostNPCInit();

	// Optional: start alert. Done after the base init so it isn't overwritten.
	SetState(NPC_STATE_ALERT);
}

//-----------------------------------------------------------------------------
// Purpose: Wallhammer logic. Runs inside the NPC's own AI think, so we never
//			hijack the think function (which would stop the AI from running).
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::PrescheduleThink(void)
{
	BaseClass::PrescheduleThink();

	if (!IsAlive())
		return;

	if (gpGlobals->curtime >= m_flNextShieldThink)
	{
		m_flNextShieldThink = gpGlobals->curtime + WALLHAMMER_THINK_INTERVAL;

		if (!m_bShieldActive)
		{
			// Raise the shield if a hostile is close and we're off cooldown
			if (gpGlobals->curtime >= m_flShieldCooldown && CountNearbyHostiles() > 0)
			{
				ActivateShield();
			}
		}
		else if (gpGlobals->curtime >= m_flShieldCooldown)
		{
			// Shield duration expired
			DeactivateShield();
		}

		DestroyPropsNearby();
	}

	// Fade runs every think so it looks smooth
	UpdateShieldVisuals();
}

//-----------------------------------------------------------------------------
// Purpose: Count hostile players/NPCs in range (and optionally in front)
//-----------------------------------------------------------------------------
int CNPC_CombineWallhammer::CountNearbyHostiles(void)
{
	const Vector vecPos = GetAbsOrigin();
	const float flRange = wallhammer_detection_range.GetFloat();
	const float flMinDot = wallhammer_detection_dot.GetFloat();

	Vector vecForward;
	GetVectors(&vecForward, NULL, NULL);

	CBaseEntity* pList[64];
	int nCount = UTIL_EntitiesInSphere(pList, ARRAYSIZE(pList), vecPos, flRange, FL_CLIENT | FL_NPC);

	int iHostiles = 0;
	for (int i = 0; i < nCount; i++)
	{
		CBaseEntity* pEnt = pList[i];
		if (!pEnt || pEnt == this || !pEnt->IsAlive())
			continue;

		if (pEnt->GetFlags() & FL_NOTARGET)
			continue;

		if (IRelationType(pEnt) != D_HT)
			continue;

		// Facing check uses OUR facing, not our velocity
		if (flMinDot > -1.0f)
		{
			Vector vecDir = pEnt->GetAbsOrigin() - vecPos;
			vecDir.z = 0;
			if (VectorNormalize(vecDir) > 0.0f && vecForward.Dot(vecDir) < flMinDot)
				continue;
		}

		iHostiles++;
	}

	return iHostiles;
}

//-----------------------------------------------------------------------------
// Purpose: Activate the shield barrier
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::ActivateShield()
{
	if (m_bShieldActive)
		return;

	// Clear any leftover prop from a previous fade
	RemoveShieldProp();

	m_bShieldActive = true;
	m_bFlashUsed = false;
	m_flShieldCooldown = gpGlobals->curtime + wallhammer_shield_duration.GetFloat();
	m_iShieldAlpha = 0;
	m_bShieldFadingOut = false;
	m_flShieldFadeTime = gpGlobals->curtime;

	// NOTE: we deliberately do NOT set m_takedamage = DAMAGE_NO. That would block
	// damage from every direction. Blocking is handled directionally in
	// OnTakeDamage_Alive() via IsShieldBlockingDamage().

	EmitSound("npc/attack_helicopter/aheli_mine_drop1.wav");

	// Create the shield prop. Model/solid are set through keyvalues BEFORE spawn
	// (SetModel + Spawn on a prop_dynamic is overridden by the "model" keyvalue).
	CBaseEntity* pShield = CreateEntityByName("prop_dynamic_override");
	if (pShield)
	{
		pShield->KeyValue("model", WALLHAMMER_SHIELD_MODEL);
		pShield->KeyValue("solid", "0");
		pShield->SetAbsOrigin(GetAbsOrigin());
		DispatchSpawn(pShield);

		// Apply scale/render settings after spawn so they aren't reset
		CBaseAnimating* pAnim = pShield->GetBaseAnimating();
		if (pAnim)
		{
			pAnim->SetModelScale(0.24f);
		}

		pShield->SetRenderMode(kRenderTransColor);
		pShield->SetRenderColor(0, 255, 255);
		pShield->SetRenderColorA(0);

		pShield->SetParent(this);
		pShield->SetLocalOrigin(Vector(35, 0, 40));
		pShield->SetLocalAngles(QAngle(0, 90, 0));

		m_hShieldProp = pShield;
	}

	// The flash goes off as the shield deploys, regardless of who is looking
	TriggerFlashEffect();
}

//-----------------------------------------------------------------------------
// Purpose: Deactivate the shield barrier
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::DeactivateShield()
{
	if (!m_bShieldActive)
		return;

	m_bShieldActive = false;
	m_flShieldCooldown = gpGlobals->curtime + wallhammer_shield_cooldown.GetFloat();
	m_bShieldFadingOut = true;
	m_flShieldFadeTime = gpGlobals->curtime;
}

//-----------------------------------------------------------------------------
// Purpose: Remove the shield prop immediately
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::RemoveShieldProp(void)
{
	if (m_hShieldProp.Get())
	{
		UTIL_Remove(m_hShieldProp.Get());
	}

	m_hShieldProp = NULL;
	m_bShieldFadingOut = false;
}

//-----------------------------------------------------------------------------
// Purpose: Update shield visual effects (fade in/out)
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::UpdateShieldVisuals()
{
	CBaseEntity* pShield = m_hShieldProp.Get();
	if (!pShield)
		return;

	float flFrac = clamp((gpGlobals->curtime - m_flShieldFadeTime) / WALLHAMMER_SHIELD_FADE_TIME, 0.0f, 1.0f);

	if (m_bShieldFadingOut)
	{
		m_iShieldAlpha = (int)(WALLHAMMER_SHIELD_MAX_ALPHA * (1.0f - flFrac));

		if (flFrac >= 1.0f)
		{
			RemoveShieldProp();
			return;
		}
	}
	else if (m_bShieldActive)
	{
		m_iShieldAlpha = (int)(WALLHAMMER_SHIELD_MAX_ALPHA * flFrac);
	}

	pShield->SetRenderColorA(m_iShieldAlpha);
}

//-----------------------------------------------------------------------------
// Purpose: Flash. Always produces light + sound. Only blinds players who
//			actually have the Wallhammer in view.
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::TriggerFlashEffect()
{
	m_bFlashUsed = true;

	const Vector vecPos = WorldSpaceCenter();
	const float flRange = wallhammer_flash_range.GetFloat();

	// Always: sound and light, so the player can tell it went off
	EmitSound("weapons/flashbang/flashbang_explode1.wav");

	CBroadcastRecipientFilter filter;
	te->DynamicLight(filter, 0.0f, &vecPos, 255, 255, 255, 5, flRange * 2.0f, 0.15f, 300.0f);

	CBaseEntity* pList[64];
	int nCount = UTIL_EntitiesInSphere(pList, ARRAYSIZE(pList), vecPos, flRange, FL_CLIENT | FL_NPC);

	for (int i = 0; i < nCount; i++)
	{
		CBaseEntity* pEntity = pList[i];
		if (!pEntity || pEntity == this)
			continue;

		if (pEntity->IsPlayer())
		{
			CBasePlayer* pPlayer = static_cast<CBasePlayer*>(pEntity);

			// Screen flash only if the player can actually see the flash
			if (pPlayer->FInViewCone(this) && pPlayer->FVisible(this))
			{
				color32 white = { 255, 255, 255, 255 };
				UTIL_ScreenFade(pPlayer, white, 0.2f, 3.0f, FFADE_IN);
			}
		}
		else if (pEntity->IsNPC() && IRelationType(pEntity) == D_HT)
		{
			// Degrade hostile NPC weapon proficiency (not our allies)
			CAI_BaseNPC* pNPC = pEntity->MyNPCPointer();
			if (pNPC)
			{
				pNPC->SetCurrentWeaponProficiency(WEAPON_PROFICIENCY_POOR);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Destroy props nearby (signature wallhammer behavior)
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::DestroyPropsNearby()
{
	const Vector vecPos = GetAbsOrigin();
	const float flPropRange = wallhammer_prop_destroy_range.GetFloat();
	const float flCrushDistance = wallhammer_prop_crush_distance.GetFloat();

	CBaseEntity* pList[64];
	int nCount = UTIL_EntitiesInSphere(pList, ARRAYSIZE(pList), vecPos, flPropRange, 0);

	for (int i = 0; i < nCount; i++)
	{
		CBaseEntity* pEntity = pList[i];
		if (!pEntity || pEntity == this || pEntity == m_hShieldProp.Get() || pEntity->GetHealth() <= 0)
			continue;

		// Only target props
		if (Q_strncmp(pEntity->GetClassname(), "prop", 4) != 0)
			continue;

		if (!pEntity->VPhysicsGetObject())
			continue;

		if (vecPos.DistTo(pEntity->WorldSpaceCenter()) <= flCrushDistance)
		{
			CTakeDamageInfo dmgInfo(this, this, 1000.0f, DMG_CRUSH);
			pEntity->TakeDamage(dmgInfo);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Is damage coming from a direction the shield covers?
//-----------------------------------------------------------------------------
bool CNPC_CombineWallhammer::IsShieldBlockingDamage(const Vector& vecDamageSource)
{
	if (!m_bShieldActive)
		return false;

	Vector vecToSource = vecDamageSource - GetAbsOrigin();
	vecToSource.z = 0;
	if (VectorNormalize(vecToSource) <= 0.0f)
		return false;

	Vector vecForward;
	GetVectors(&vecForward, NULL, NULL);
	vecForward.z = 0;
	VectorNormalize(vecForward);

	return vecForward.Dot(vecToSource) > wallhammer_shield_block_dot.GetFloat();
}

//-----------------------------------------------------------------------------
// Purpose: Modify incoming damage
//-----------------------------------------------------------------------------
int CNPC_CombineWallhammer::OnTakeDamage_Alive(const CTakeDamageInfo& info)
{
	if (m_bShieldActive)
	{
		// Work out where the damage came from
		Vector vecSource = info.GetDamagePosition();
		if ((vecSource == vec3_origin || vecSource == vec3_invalid) && info.GetAttacker())
		{
			vecSource = info.GetAttacker()->WorldSpaceCenter();
		}

		if (IsShieldBlockingDamage(vecSource))
		{
			if (wallhammer_debug.GetBool())
			{
				DevMsg("Wallhammer: shield blocked %.1f damage\n", info.GetDamage());
			}

			// Feedback so the player can see the hit was absorbed
			g_pEffects->Sparks(info.GetDamagePosition());
			return 0;
		}
	}

	return BaseClass::OnTakeDamage_Alive(info);
}

//-----------------------------------------------------------------------------
// Purpose: Scale outgoing bullet damage by wallhammer_damage_multiplier
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::FireBullets(const FireBulletsInfo_t& info)
{
	FireBulletsInfo_t modified = info;

	// A damage of 0 means "use the ammo definition" (sk_npc_dmg_*)
	float flBase = (info.m_flDamage > 0) ? info.m_flDamage : GetAmmoDef()->NPCDamage(info.m_iAmmoType);

	if (wallhammer_debug.GetBool())
	{
		DevMsg("Wallhammer FireBullets: base damage %.2f x %.2f (ammo %d)\n",
			flBase, wallhammer_damage_multiplier.GetFloat(), info.m_iAmmoType);
	}

	modified.m_flDamage = flBase * wallhammer_damage_multiplier.GetFloat();
	modified.m_iPlayerDamage = (int)modified.m_flDamage;

	BaseClass::FireBullets(modified);
}

//-----------------------------------------------------------------------------
// Purpose: Clean up the shield when we die
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::Event_Killed(const CTakeDamageInfo& info)
{
	m_bShieldActive = false;
	RemoveShieldProp();

	BaseClass::Event_Killed(info);
}