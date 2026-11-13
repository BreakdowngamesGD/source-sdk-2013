//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Wallhammer - A heavy combine soldier with a deployable shield
//
//=============================================================================//

#include "cbase.h"
#include "ai_hull.h"
#include "ai_motor.h"
#include "npc_combine.h"
#include "npc_combine_hla.h"
#include "npcevent.h"
#include "engine/IEngineSound.h"
#include "explode.h"
#include "props.h"
#include "entitylist.h"
#include "player.h"
#include "basecombatweapon.h"
#include "ammodef.h"
#include "IEffects.h"
#include "ndebugoverlay.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS(npc_combine_wallhammer, CNPC_CombineWallhammer);

// Convars
ConVar	sk_wallhammer_health("sk_wallhammer_health", "360");
ConVar	wallhammer_shield_duration("wallhammer_shield_duration", "5.5");
ConVar	wallhammer_shield_cooldown("wallhammer_shield_cooldown", "8");
ConVar	wallhammer_damage_multiplier("wallhammer_damage_multiplier", "1.9");
ConVar	wallhammer_fallback_damage("wallhammer_fallback_damage", "3", FCVAR_NONE, "Per-pellet damage used if the ammo definition (sk_npc_dmg_buckshot) resolves to 0.");
// NOTE: the two detection convars below are no longer used (the shield now raises when shot).
ConVar	wallhammer_detection_range("wallhammer_detection_range", "200");
ConVar	wallhammer_detection_dot("wallhammer_detection_dot", "-1", FCVAR_NONE, "Hostiles must be within this dot product of my facing to raise the shield. -1 = any direction, 0 = front half.");
ConVar	wallhammer_flash_range("wallhammer_flash_range", "120");
ConVar	wallhammer_flash_cooldown("wallhammer_flash_cooldown", "15", FCVAR_NONE, "Seconds before the Wallhammer's flash can go off again.");
ConVar	wallhammer_prop_destroy_range("wallhammer_prop_destroy_range", "80");
ConVar	wallhammer_prop_crush_distance("wallhammer_prop_crush_distance", "50");

ConVar	wallhammer_close_range("wallhammer_close_range", "90", FCVAR_NONE, "Target distance (in units) the Wallhammer strives to reach in combat.");
ConVar	wallhammer_aggressive_advance("wallhammer_aggressive_advance", "1", FCVAR_NONE, "1 = aggressively advance/charge target during combat if outside close range.");


ConVar	wallhammer_rush_enabled("wallhammer_rush_enabled", "1", FCVAR_NONE, "When the enemy goes out of sight, rush their last known position instead of peeking from afar.");
ConVar	wallhammer_rush_min_dist("wallhammer_rush_min_dist", "1", FCVAR_NONE, "Only rush if the last known position is farther than this. Closer than this, normal AI takes over.");
ConVar	wallhammer_rush_player_only("wallhammer_rush_player_only", "0", FCVAR_NONE, "1 = only rush when the enemy is the player.");

// Shield geometry, in the Wallhammer's local space (x = forward, y = left, z = up).
// The shield blocks a shot only if the shot's path passes through this box.
ConVar	wallhammer_shield_forward("wallhammer_shield_forward", "35", FCVAR_NONE, "Shield distance in front of the Wallhammer.");
ConVar	wallhammer_shield_zoffset("wallhammer_shield_zoffset", "40", FCVAR_NONE, "Shield center height above the Wallhammer's origin.");
ConVar	wallhammer_shield_width("wallhammer_shield_width", "60", FCVAR_NONE, "Shield block-box width (side to side).");
ConVar	wallhammer_shield_height("wallhammer_shield_height", "72", FCVAR_NONE, "Shield block-box height.");
ConVar	wallhammer_shield_thickness("wallhammer_shield_thickness", "6", FCVAR_NONE, "Shield block-box thickness (front to back).");

ConVar	wallhammer_debug("wallhammer_debug", "0", FCVAR_NONE, "1 = print flash/shield/damage info. 2 = also draw the shield block-box.");

// ---------------------------------------------------------------------------
// Voice lines
// ---------------------------------------------------------------------------
// Folder (under sound/) that holds the .wav files from filenames.txt. CHANGE THIS to
// wherever you actually put them. Must end in a slash.
#define WALLHAMMER_VOICE_DIR		"combine/vo_charger/"

ConVar	wallhammer_voice_enable("wallhammer_voice_enable", "1", FCVAR_NONE, "Enable Wallhammer voice lines.");
ConVar	wallhammer_voice_min_gap("wallhammer_voice_min_gap", "2.5", FCVAR_NONE, "Minimum seconds between non-critical voice lines.");

enum WallhammerVoice_t
{
	WHV_ANNOUNCE_ATTACK = 0,
	WHV_ANNOUNCE_KILL,
	WHV_COMBAT_IDLE,
	WHV_DIE,
	WHV_GET_BACK,
	WHV_HEAR_SUSPICIOUS,
	WHV_IDLE,
	WHV_LOST_VISUAL,
	WHV_NEAR_PANIC,
	WHV_PAIN,
	WHV_RELOAD,
	WHV_SEES_GRENADES,
	WHV_SQUADMEMBER_LOST,

	WHV_COUNT
};

// File numbering has gaps (e.g. announceattack_06/07/10 don't exist), so each
// category lists the indices that actually exist.
static const int s_AnnounceAttackIdx[] = { 1, 2, 3, 4, 5, 8, 9, 11 };
static const int s_AnnounceKillIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const int s_CombatIdleIdx[] = { 12, 20, 30, 40, 50, 60, 70, 80, 90, 100 };	// 3-digit names
static const int s_DieIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const int s_GetBackIdx[] = { 1, 2, 3, 4, 5 };
static const int s_HearSuspiciousIdx[] = { 2, 3, 4, 5, 6 };
static const int s_IdleIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
static const int s_LostVisualIdx[] = { 1, 2, 3, 4, 5, 7, 8, 9, 10 };
static const int s_NearPanicIdx[] = { 2, 3, 4, 7 };
static const int s_PainIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const int s_ReloadIdx[] = { 1, 2, 3, 5, 6, 7, 8, 9 };
static const int s_SeesGrenadesIdx[] = { 1, 2, 3, 4 };
static const int s_SquadLostIdx[] = { 1, 2 };

struct WallhammerVoiceSet_t
{
	const char* pszPrefix;
	const int* pIndices;
	int			nCount;
	int			nDigits;
};

#define WHV_SET(prefix, arr, digits) { prefix, arr, ARRAYSIZE(arr), digits }

static const WallhammerVoiceSet_t s_VoiceSets[WHV_COUNT] =
{
	WHV_SET("announceattack",	s_AnnounceAttackIdx,	2),
	WHV_SET("announcekill",		s_AnnounceKillIdx,		2),
	WHV_SET("combat_idle",		s_CombatIdleIdx,		3),
	WHV_SET("die",				s_DieIdx,				2),
	WHV_SET("getback",			s_GetBackIdx,			2),
	WHV_SET("hear_suspicious",	s_HearSuspiciousIdx,	2),
	WHV_SET("idle",				s_IdleIdx,				2),
	WHV_SET("lostvisual",		s_LostVisualIdx,		2),
	WHV_SET("nearpanic",		s_NearPanicIdx,			2),
	WHV_SET("pain",				s_PainIdx,				2),
	WHV_SET("reload",			s_ReloadIdx,			2),
	WHV_SET("sees_grenades",	s_SeesGrenadesIdx,		2),
	WHV_SET("squadmemberlost",	s_SquadLostIdx,			2),
};

static void WallhammerBuildVoicePath(char* pszOut, int nOutLen, const WallhammerVoiceSet_t& set, int iIndex)
{
	Q_snprintf(pszOut, nOutLen, "%s%s_%0*d.wav", WALLHAMMER_VOICE_DIR, set.pszPrefix, set.nDigits, iIndex);
}

#define WALLHAMMER_SHIELD_MODEL		"models/ovr_load/force_shield.mdl"
#define WALLHAMMER_SHIELD_MAX_ALPHA	150
#define WALLHAMMER_SHIELD_FADE_TIME	0.6f
#define WALLHAMMER_THINK_INTERVAL	0.1f

BEGIN_DATADESC(CNPC_CombineWallhammer)
DEFINE_FIELD(m_bShieldActive, FIELD_BOOLEAN),
DEFINE_FIELD(m_flNextFlashTime, FIELD_TIME),
DEFINE_FIELD(m_flShieldCooldown, FIELD_TIME),
DEFINE_FIELD(m_flNextShieldThink, FIELD_TIME),
DEFINE_FIELD(m_hShieldProp, FIELD_EHANDLE),
DEFINE_FIELD(m_iShieldAlpha, FIELD_INTEGER),
DEFINE_FIELD(m_flShieldFadeTime, FIELD_TIME),
DEFINE_FIELD(m_bShieldFadingOut, FIELD_BOOLEAN),
DEFINE_FIELD(m_flNextVoiceTime, FIELD_TIME),
DEFINE_FIELD(m_flNextPainVoiceTime, FIELD_TIME),
DEFINE_FIELD(m_flNextCombatIdleTime, FIELD_TIME),
DEFINE_FIELD(m_bNearPanicSpoken, FIELD_BOOLEAN),
DEFINE_FIELD(m_bKillSpoken, FIELD_BOOLEAN),
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
		SetModelName(MAKE_STRING("models/combine_wallhammer.mdl"));
	}

	// The EZ2 CNPC_Combine constructor adds SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE
	// (for npc_create). Commandable soldiers count as PLAYER ALLIES and regenerate health,
	// which we never want on an enemy heavy.
	//RemoveSpawnFlags(SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE);

	// Equip the shotgun through the normal spawn-equipment path so the weapon is
	// set up (capabilities, proficiency, animation translation) during NPCInit.
	if (m_spawnEquipment == NULL_STRING)
	{
		m_spawnEquipment = MAKE_STRING("weapon_flechette_shotgun");
	}

	// Initialize shield system
	m_bShieldActive = false;
	m_flNextFlashTime = 0.0f;
	m_flShieldCooldown = gpGlobals->curtime;
	m_flNextShieldThink = 0.0f;
	m_hShieldProp = NULL;
	m_iShieldAlpha = 0;
	m_flShieldFadeTime = 0.0f;
	m_bShieldFadingOut = false;

	// Voice state
	m_flNextVoiceTime = 0.0f;
	m_flNextPainVoiceTime = 0.0f;
	m_flNextCombatIdleTime = 0.0f;
	m_bNearPanicSpoken = false;
	m_bKillSpoken = false;

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
	PrecacheModel("models/combine_wallhammer.mdl");
	PrecacheModel(WALLHAMMER_SHIELD_MODEL);

	// These are raw .wav paths, not soundscript names, so use PrecacheSound
	PrecacheSound("npc/attack_helicopter/aheli_mine_drop1.wav");
	PrecacheSound("weapons/flashbang/flashbang_explode1.wav");

	UTIL_PrecacheOther("prop_dynamic_override");

	// Voice lines (raw .wav paths)
	for (int iSet = 0; iSet < WHV_COUNT; iSet++)
	{
		const WallhammerVoiceSet_t& set = s_VoiceSets[iSet];
		for (int i = 0; i < set.nCount; i++)
		{
			char szPath[256];
			WallhammerBuildVoicePath(szPath, sizeof(szPath), set, set.pIndices[i]);
			PrecacheSound(szPath);
		}
	}

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: Determines if the Wallhammer should aggressively push closer to the enemy
//-----------------------------------------------------------------------------
bool CNPC_CombineWallhammer::ShouldCloseDistance(void)
{
	if (!wallhammer_aggressive_advance.GetBool())
		return false;

	if (GetState() != NPC_STATE_COMBAT)
		return false;

	CBaseEntity* pEnemy = GetEnemy();
	if (!pEnemy || !pEnemy->IsAlive())
		return false;

	// Let the reload logic win: low OR empty, plus mid-reload schedules
	if (HasCondition(COND_NO_PRIMARY_AMMO) || HasCondition(COND_LOW_PRIMARY_AMMO))
		return false;

	if (IsCurSchedule(SCHED_COMBINE_HIDE_AND_RELOAD, false) ||
		IsCurSchedule(SCHED_RELOAD, false) ||
		IsCurSchedule(SCHED_HIDE_AND_RELOAD, false))
		return false;

	CBaseCombatWeapon* pWeapon = GetActiveWeapon();
	if (pWeapon && pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() <= 0)
		return false;

	if (IsUnreachable(pEnemy))
		return false;

	const float flCloseRange = wallhammer_close_range.GetFloat();
	return GetAbsOrigin().DistToSqr(pEnemy->GetAbsOrigin()) > (flCloseRange * flCloseRange);
}

//-----------------------------------------------------------------------------
// Purpose: Post-spawn initialization
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::PostNPCInit()
{
	// Fallback in case spawn equipment was explicitly cleared and nothing else equipped
	if (!GetActiveWeapon())
	{
		GiveWeapon(MAKE_STRING("weapon_flechette_shotgun"));
	}

	BaseClass::PostNPCInit();

	// Optional: start alert. Done after the base init so it isn't overwritten.
	SetState(NPC_STATE_ALERT);
}

//-----------------------------------------------------------------------------
// Purpose: Interrupt long-range or static schedules when distance needs to be closed,
//			and let sounds interrupt idle/alert schedules so he can investigate.
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::BuildScheduleTestBits(void)
{
	BaseClass::BuildScheduleTestBits();

	if (GetState() == NPC_STATE_COMBAT &&
		!IsCurSchedule(SCHED_COMBINE_PRESS_ATTACK, false))
	{
		if (ShouldCloseDistance())
		{
			SetCustomInterruptCondition(COND_TOO_FAR_TO_ATTACK);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Select aggressive charging schedule if outside close-quarters range.
//			Falls through to CNPC_CombineHLABase, which handles sound investigation.
//-----------------------------------------------------------------------------
int CNPC_CombineWallhammer::SelectSchedule(void)
{
	if (GetState() == NPC_STATE_COMBAT && GetEnemy() && GetEnemy()->IsAlive())
	{
		if (ShouldCloseDistance())
		{
			return SCHED_COMBINE_PRESS_ATTACK;
		}
	}

	return BaseClass::SelectSchedule();
}

//-----------------------------------------------------------------------------
// Purpose: Wallhammer logic. Runs inside the NPC's own AI think.
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::PrescheduleThink(void)
{
	BaseClass::PrescheduleThink();

	if (!IsAlive())
		return;

	// Infinite Reserve Ammo: Keep ammo inventory topped off so he can always reload,
	// while allowing current clip ammo to empty naturally for reload anims & voice lines.
	CBaseCombatWeapon* pWeapon = GetActiveWeapon();
	if (pWeapon && pWeapon->m_iPrimaryAmmoType != -1)
	{
		SetAmmoCount(999, pWeapon->m_iPrimaryAmmoType);
	}

	if (gpGlobals->curtime >= m_flNextShieldThink)
	{
		m_flNextShieldThink = gpGlobals->curtime + WALLHAMMER_THINK_INTERVAL;

		// Shield duration expired
		if (m_bShieldActive && gpGlobals->curtime >= m_flShieldCooldown)
		{
			DeactivateShield();
		}

		// Flash goes off whenever a hostile gets close, shield or no shield
		if (gpGlobals->curtime >= m_flNextFlashTime &&
			CountHostilesInRange(wallhammer_flash_range.GetFloat(), -1.0f) > 0)
		{
			TriggerFlashEffect();
		}

		DestroyPropsNearby();
	}

	// Visualize the block-box so it can be tuned to match the model
	if (m_bShieldActive && wallhammer_debug.GetInt() >= 2)
	{
		Vector mins, maxs;
		GetShieldBounds(mins, maxs);
		NDebugOverlay::BoxAngles(GetAbsOrigin(), mins, maxs, GetAbsAngles(), 0, 255, 255, 40, 0.1f);
	}

	// Fade runs every think so it looks smooth
	UpdateShieldVisuals();

	UpdateVoice();
}

//-----------------------------------------------------------------------------
// Purpose: Count hostile players/NPCs within a range (and optionally in front)
//-----------------------------------------------------------------------------
int CNPC_CombineWallhammer::CountHostilesInRange(float flRange, float flMinDot)
{
	const Vector vecPos = GetAbsOrigin();

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

		// Players always count (the Wallhammer reacts to the player regardless of
		// squad/ally flags); NPCs count if we're hostile toward them.
		if (!pEnt->IsPlayer() && IRelationType(pEnt) != D_HT)
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
// Purpose: Shield block-box in the Wallhammer's local space
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::GetShieldBounds(Vector& mins, Vector& maxs)
{
	const float flFwd = wallhammer_shield_forward.GetFloat();
	const float flZ = wallhammer_shield_zoffset.GetFloat();
	const float flHalfW = wallhammer_shield_width.GetFloat() * 0.5f;
	const float flHalfH = wallhammer_shield_height.GetFloat() * 0.5f;
	const float flHalfT = wallhammer_shield_thickness.GetFloat() * 0.5f;

	mins.Init(flFwd - flHalfT, -flHalfW, flZ - flHalfH);
	maxs.Init(flFwd + flHalfT, flHalfW, flZ + flHalfH);
}

//-----------------------------------------------------------------------------
// Purpose: Does the segment vecStart -> vecEnd pass through the shield?
//			(Ray vs. box slab test, done in the Wallhammer's local space.)
//-----------------------------------------------------------------------------
bool CNPC_CombineWallhammer::ShieldInterceptsRay(const Vector& vecStart, const Vector& vecEnd, Vector* pvecHit)
{
	if (!m_bShieldActive)
		return false;

	const matrix3x4_t& mat = EntityToWorldTransform();

	Vector vecLocalStart, vecLocalEnd;
	VectorITransform(vecStart, mat, vecLocalStart);
	VectorITransform(vecEnd, mat, vecLocalEnd);

	Vector mins, maxs;
	GetShieldBounds(mins, maxs);

	const Vector vecDelta = vecLocalEnd - vecLocalStart;

	float tMin = 0.0f;
	float tMax = 1.0f;

	for (int i = 0; i < 3; i++)
	{
		if (fabs(vecDelta[i]) < 1e-6f)
		{
			// Parallel to this slab: must already be inside it
			if (vecLocalStart[i] < mins[i] || vecLocalStart[i] > maxs[i])
				return false;
		}
		else
		{
			float flInv = 1.0f / vecDelta[i];
			float t1 = (mins[i] - vecLocalStart[i]) * flInv;
			float t2 = (maxs[i] - vecLocalStart[i]) * flInv;
			if (t1 > t2)
			{
				V_swap(t1, t2);
			}

			tMin = MAX(tMin, t1);
			tMax = MIN(tMax, t2);

			if (tMin > tMax)
				return false;
		}
	}

	if (pvecHit)
	{
		Vector vecLocalHit = vecLocalStart + vecDelta * tMin;
		VectorTransform(vecLocalHit, mat, *pvecHit);
	}

	return true;
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
	m_flShieldCooldown = gpGlobals->curtime + wallhammer_shield_duration.GetFloat();
	m_iShieldAlpha = 0;
	m_bShieldFadingOut = false;
	m_flShieldFadeTime = gpGlobals->curtime;

	// NOTE: we deliberately do NOT set m_takedamage = DAMAGE_NO. Blocking is done per-shot
	// in TraceAttack() / OnTakeDamage_Alive() by checking the shot's path against the shield.

	EmitSound("npc/attack_helicopter/aheli_mine_drop1.wav");
	PlayVoice(WHV_ANNOUNCE_ATTACK);

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
		pShield->SetLocalOrigin(Vector(wallhammer_shield_forward.GetFloat(), 0, wallhammer_shield_zoffset.GetFloat()));
		pShield->SetLocalAngles(QAngle(0, 90, 0));

		m_hShieldProp = pShield;
	}
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
	m_flNextFlashTime = gpGlobals->curtime + wallhammer_flash_cooldown.GetFloat();

	const Vector vecPos = WorldSpaceCenter();
	const float flRange = wallhammer_flash_range.GetFloat();

	// Always: sound and light, so the player can tell it went off
	EmitSound("weapons/flashbang/flashbang_explode1.wav");
	PlayVoice(WHV_GET_BACK, true);

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

			// Screen flash only if the player is facing the flash and has a clear line to it
			bool bSeesFlash = pPlayer->FInViewCone(vecPos) && pPlayer->FVisible(vecPos, MASK_OPAQUE);

			if (wallhammer_debug.GetBool())
			{
				DevMsg("Wallhammer flash: player in range, sees flash = %d\n", (int)bSeesFlash);
			}

			if (bSeesFlash)
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
// Purpose: Hitscan damage (bullets, melee). ptr->startpos is where the shot began
//			and ptr->endpos is where it hit us. If that path crosses the shield box,
//			the shield eats the hit: no damage, no blood. Shots that reach us
//			without crossing the shield (from behind/the sides) land normally.
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::TraceAttack(const CTakeDamageInfo& info, const Vector& vecDir, trace_t* ptr, CDmgAccumulator* pAccumulator)
{
	if (m_bShieldActive)
	{
		Vector vecShieldHit;
		if (ShieldInterceptsRay(ptr->startpos, ptr->endpos, &vecShieldHit))
		{
			if (wallhammer_debug.GetBool())
			{
				DevMsg("Wallhammer: shield absorbed a hit (%.1f damage)\n", info.GetDamage());
			}

			g_pEffects->Sparks(vecShieldHit);
			return;
		}
	}

	BaseClass::TraceAttack(info, vecDir, ptr, pAccumulator);
}

//-----------------------------------------------------------------------------
// Purpose: Damage that doesn't come through a trace. Explosions are checked by
//			casting from the blast origin to our center through the shield box.
//			Also raises the shield in reaction to being hurt.
//-----------------------------------------------------------------------------
int CNPC_CombineWallhammer::OnTakeDamage_Alive(const CTakeDamageInfo& info)
{
	if (m_bShieldActive && (info.GetDamageType() & DMG_BLAST))
	{
		Vector vecSource = info.GetDamagePosition();
		if ((vecSource == vec3_origin || vecSource == vec3_invalid) && info.GetAttacker())
		{
			vecSource = info.GetAttacker()->WorldSpaceCenter();
		}

		if (ShieldInterceptsRay(vecSource, WorldSpaceCenter()))
		{
			if (wallhammer_debug.GetBool())
			{
				DevMsg("Wallhammer: shield absorbed a blast (%.1f damage)\n", info.GetDamage());
			}
			return 0;
		}
	}

	int ret = BaseClass::OnTakeDamage_Alive(info);

	// Raise the shield in reaction to being hurt (off cooldown, still alive,
	// and not from ourselves or a friendly).
	if (ret > 0 && !m_bShieldActive && IsAlive() && gpGlobals->curtime >= m_flShieldCooldown)
	{
		CBaseEntity* pAttacker = info.GetAttacker();
		if (pAttacker != this && !(pAttacker && IRelationType(pAttacker) == D_LI))
		{
			ActivateShield();
		}
	}

	return ret;
}

//-----------------------------------------------------------------------------
// Purpose: Scale outgoing bullet damage by wallhammer_damage_multiplier
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::FireBullets(const FireBulletsInfo_t& info)
{
	FireBulletsInfo_t modified = info;

	// A damage of 0 means "use the ammo definition" (sk_npc_dmg_*)
	float flBase = (info.m_flDamage > 0) ? info.m_flDamage : GetAmmoDef()->NPCDamage(info.m_iAmmoType);

	// If the skill files aren't loaded the ammo damage is 0, which means harmless bullets
	if (flBase <= 0.0f)
	{
		flBase = wallhammer_fallback_damage.GetFloat();
	}

	modified.m_flDamage = flBase * wallhammer_damage_multiplier.GetFloat();
	modified.m_iPlayerDamage = (int)modified.m_flDamage;

	if (wallhammer_debug.GetBool())
	{
		DevMsg("Wallhammer FireBullets: %d shots, damage %.2f (base %.2f x %.2f), ammo %d\n",
			info.m_iShots, modified.m_flDamage, flBase, wallhammer_damage_multiplier.GetFloat(), info.m_iAmmoType);
	}

	BaseClass::FireBullets(modified);
}

//-----------------------------------------------------------------------------
// Purpose: Play a random line from a voice category on the voice channel.
//			bForce skips the min-gap throttle (used for pain/death/flash).
//-----------------------------------------------------------------------------
bool CNPC_CombineWallhammer::PlayVoice(int iVoice, bool bForce)
{
	if (!wallhammer_voice_enable.GetBool())
		return false;

	if (iVoice < 0 || iVoice >= WHV_COUNT)
		return false;

	if (!bForce && gpGlobals->curtime < m_flNextVoiceTime)
		return false;

	const WallhammerVoiceSet_t& set = s_VoiceSets[iVoice];
	int iPick = set.pIndices[random->RandomInt(0, set.nCount - 1)];

	char szPath[256];
	WallhammerBuildVoicePath(szPath, sizeof(szPath), set, iPick);

	CPASAttenuationFilter filter(this);

	EmitSound_t params;
	params.m_pSoundName = szPath;
	params.m_nChannel = CHAN_VOICE;		// a new line replaces whatever he was saying
	params.m_flVolume = 1.0f;
	params.m_SoundLevel = SNDLVL_TALKING;

	EmitSound(filter, entindex(), params);

	m_flNextVoiceTime = gpGlobals->curtime + wallhammer_voice_min_gap.GetFloat();

	if (wallhammer_debug.GetBool())
	{
		DevMsg("Wallhammer voice: %s\n", szPath);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Situational voice lines that don't have a dedicated hook. Called
//			every AI think; everything here is throttled.
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::UpdateVoice()
{
	if (!IsAlive())
		return;

	const NPC_STATE state = GetState();

	// --- Dead enemy: announce the kill once per kill
	if (HasCondition(COND_ENEMY_DEAD))
	{
		if (!m_bKillSpoken)
		{
			m_bKillSpoken = true;
			PlayVoice(WHV_ANNOUNCE_KILL, true);
		}
	}
	else
	{
		m_bKillSpoken = false;
	}

	// --- Low health: panic line, re-armed once he's healed back up
	const float flHealthFrac = (float)GetHealth() / (float)MAX(GetMaxHealth(), 1);
	if (flHealthFrac < 0.2f)
	{
		if (!m_bNearPanicSpoken && PlayVoice(WHV_NEAR_PANIC))
		{
			m_bNearPanicSpoken = true;
		}
	}
	else if (flHealthFrac > 0.4f)
	{
		m_bNearPanicSpoken = false;
	}

	if (state == NPC_STATE_COMBAT)
	{
		// --- Grenade / danger sound nearby
		if (HasCondition(COND_HEAR_DANGER))
		{
			PlayVoice(WHV_SEES_GRENADES);
		}
		// --- Reloading
		else if (HasCondition(COND_NO_PRIMARY_AMMO) || HasCondition(COND_LOW_PRIMARY_AMMO))
		{
			PlayVoice(WHV_RELOAD);
		}
		// --- Ambient combat chatter every so often
		else if (gpGlobals->curtime >= m_flNextCombatIdleTime)
		{
			m_flNextCombatIdleTime = gpGlobals->curtime + random->RandomFloat(8.0f, 16.0f);
			PlayVoice(WHV_COMBAT_IDLE);
		}
	}
	else if (state == NPC_STATE_IDLE || state == NPC_STATE_ALERT)
	{
		// --- Heard something but nothing seen yet
		if (HasCondition(COND_HEAR_COMBAT) || HasCondition(COND_HEAR_PLAYER) || HasCondition(COND_HEAR_WORLD))
		{
			PlayVoice(WHV_HEAR_SUSPICIOUS);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Voice overrides. These replace the Combine response-system / sentence
//			lines for these events.
//-----------------------------------------------------------------------------
void CNPC_CombineWallhammer::PainSound(const CTakeDamageInfo& info)
{
	if (GetFlags() & FL_DISSOLVING)
		return;

	if (gpGlobals->curtime < m_flNextPainVoiceTime)
		return;

	m_flNextPainVoiceTime = gpGlobals->curtime + 1.0f;
	PlayVoice(WHV_PAIN, true);
}

void CNPC_CombineWallhammer::DeathSound(const CTakeDamageInfo& info)
{
	if (GetFlags() & FL_DISSOLVING)
		return;

	PlayVoice(WHV_DIE, true);
}

void CNPC_CombineWallhammer::IdleSound(void)
{
	PlayVoice(WHV_IDLE);
}

void CNPC_CombineWallhammer::LostEnemySound(CBaseEntity* pEnemy)
{
	PlayVoice(WHV_LOST_VISUAL);
}

void CNPC_CombineWallhammer::NotifyDeadFriend(CBaseEntity* pFriend)
{
	PlayVoice(WHV_SQUADMEMBER_LOST);
}

//-----------------------------------------------------------------------------
// Purpose: Override static defensive/peek schedules to force aggressive rushes
//-----------------------------------------------------------------------------
int CNPC_CombineWallhammer::TranslateSchedule(int scheduleType)
{
	// Allow reload schedules to execute naturally
	if (scheduleType == SCHED_RELOAD || scheduleType == SCHED_HIDE_AND_RELOAD)
	{
		return BaseClass::TranslateSchedule(scheduleType);
	}

	switch (scheduleType)
	{
	case SCHED_ESTABLISH_LINE_OF_FIRE:
	case SCHED_TAKE_COVER_FROM_ENEMY:
	case SCHED_COMBINE_WAIT_IN_COVER:
	case SCHED_COMBINE_ENTER_OVERWATCH:
	case SCHED_COMBINE_OVERWATCH:
	case SCHED_RANGE_ATTACK1:
		if (ShouldCloseDistance())
		{
			if (wallhammer_debug.GetBool())
			{
				DevMsg("Wallhammer: closing distance / pressing attack\n");
			}
			return SCHED_COMBINE_PRESS_ATTACK;
		}
		break;
	}

	return BaseClass::TranslateSchedule(scheduleType);
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

//-----------------------------------------------------------------------------
// Purpose: CNPC_Combine::HasShotgun() only recognizes weapon_shotgun, so the
//			flechette shotgun gets the rifle shoot height. Standing rifle height
//			is 57 and shotgun height is 36 (crouching is 36 for both), so pull
//			the standing position down by the difference.
//-----------------------------------------------------------------------------
Vector CNPC_CombineWallhammer::Weapon_ShootPosition(void)
{
	Vector vecPos = BaseClass::Weapon_ShootPosition();

	CBaseCombatWeapon* pWeapon = GetActiveWeapon();
	if (pWeapon && FClassnameIs(pWeapon, "weapon_flechette_shotgun"))
	{
		bool bStanding = !IsCrouching();
		if ((CapabilitiesGet() & bits_CAP_DUCK) && IsCrouchedActivity(GetActivity()))
		{
			bStanding = false;
		}

		if (bStanding)
		{
			vecPos.z -= 21.0f;	// 57 - 36
		}
	}

	return vecPos;
}

//-----------------------------------------------------------------------------
// Purpose: Treat the flechette shotgun like the regular shotgun (PERFECT)
//-----------------------------------------------------------------------------
WeaponProficiency_t CNPC_CombineWallhammer::CalcWeaponProficiency(CBaseCombatWeapon* pWeapon)
{
	if (pWeapon && FClassnameIs(pWeapon, "weapon_flechette_shotgun"))
	{
		return WEAPON_PROFICIENCY_PERFECT;
	}

	return BaseClass::CalcWeaponProficiency(pWeapon);
}