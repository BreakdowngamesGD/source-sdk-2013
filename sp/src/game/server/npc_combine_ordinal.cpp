//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Ordinal - Overwatch Commando squad leader.
//
//			* Carries manhacks and deploys them (weapon lowered during the
//			  deploy animation = vulnerable) to flush hiding enemies.
//			* Periodically orders squadmates to advance on a hiding enemy.
//			* When he dies, the rest of the squad reports it over the radio and
//			  fights disorganized (worse aim, stunned, no more pressure orders).
//			* Weaker than a Wallhammer / Suppressor, no grenades.
//
//=============================================================================//

#include "cbase.h"
#include "ai_hull.h"
#include "ai_motor.h"
#include "ai_squad.h"
#include "npc_combine.h"
#include "npc_combine_hla.h"
#include "npcevent.h"
#include "engine/IEngineSound.h"
#include "soundent.h"
#include "basecombatweapon.h"
#include "ammodef.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS(npc_combine_ordinal, CNPC_CombineOrdinal);

// ---------------------------------------------------------------------------
// Convars
// ---------------------------------------------------------------------------
ConVar	sk_ordinal_health("sk_ordinal_health", "120");
ConVar	ordinal_damage_multiplier("ordinal_damage_multiplier", "1.0");
ConVar	ordinal_fallback_damage("ordinal_fallback_damage", "3", FCVAR_NONE, "Per-bullet damage used if the ammo definition resolves to 0.");
ConVar	ordinal_weapon_proficiency("ordinal_weapon_proficiency", "3", FCVAR_NONE, "Ordinal accuracy: 0=poor 1=average 2=good 3=very good 4=perfect.");

// Manhacks
ConVar	ordinal_manhack_count("ordinal_manhack_count", "3", FCVAR_NONE, "Manhacks carried if the map doesn't set the 'manhacks' keyvalue (0 in Hammer = use this).");
ConVar	ordinal_manhack_cooldown("ordinal_manhack_cooldown", "12", FCVAR_NONE, "Seconds after his manhack dies before he may deploy another.");

// Squad orders
ConVar	ordinal_orders_enabled("ordinal_orders_enabled", "1", FCVAR_NONE, "1 = Ordinal orders squadmates to advance on a hiding enemy.");
ConVar	ordinal_order_hide_time("ordinal_order_hide_time", "4", FCVAR_NONE, "Seconds the enemy must be out of sight before he orders an advance.");
ConVar	ordinal_order_cooldown("ordinal_order_cooldown", "15", FCVAR_NONE, "Minimum seconds between advance orders.");
ConVar	ordinal_order_max_units("ordinal_order_max_units", "2", FCVAR_NONE, "Max squadmates ordered per advance order.");

// Leader down
ConVar	ordinal_leader_down_enabled("ordinal_leader_down_enabled", "1", FCVAR_NONE, "1 = squad becomes disorganized when the Ordinal dies.");
ConVar	ordinal_leader_down_proficiency("ordinal_leader_down_proficiency", "0", FCVAR_NONE, "Squadmate accuracy after the Ordinal dies (0=poor).");
ConVar	ordinal_leader_down_stun("ordinal_leader_down_stun", "2.5", FCVAR_NONE, "Seconds squadmates can't fire after the Ordinal dies.");

ConVar	ordinal_debug("ordinal_debug", "0", FCVAR_NONE, "1 = print manhack/order/voice info.");

// Tactical variant values from npc_combine.cpp (the enum isn't in the header)
#define ORDINAL_TACTICAL_DEFAULT				0
#define ORDINAL_TACTICAL_PRESSURE_UNTIL_CLOSE	2

#define ORDINAL_MODEL		"models/combine_ordinal.mdl"
#define ORDINAL_WEAPON		"weapon_ar2"		// closest HL2 stand-in for the IR1. Change to taste, or set spawnequipment in Hammer.

// ---------------------------------------------------------------------------
// Voice lines
// ---------------------------------------------------------------------------
// Folder (under sound/) holding the voice .wav files. Must end in a slash.
// Files are expected at sound/<dir>/<name>_<NN>.wav
//
// Voice is OFF by default (ordinal_voice_enable 0) so the stock Combine voice is used
// until you drop your files in and fix the index lists below. Set the convar to 1 BEFORE
// the map loads so the sounds get precached.
#define ORDINAL_VOICE_DIR		"combine/vo_ordinal/"

ConVar	ordinal_voice_enable("ordinal_voice_enable", "0", FCVAR_NONE, "Enable Ordinal voice lines (set before map load). 0 = stock Combine voice.");
ConVar	ordinal_voice_min_gap("ordinal_voice_min_gap", "2.5", FCVAR_NONE, "Minimum seconds between non-critical voice lines.");

enum OrdinalVoice_t
{
	ORV_ANNOUNCE_ATTACK = 0,
	ORV_ORDER_ADVANCE,
	ORV_DEPLOY_MANHACK,
	ORV_LEADER_DOWN_REPORT,		// spoken by a SQUADMATE when the Ordinal dies
	ORV_ANNOUNCE_KILL,
	ORV_COMBAT_IDLE,
	ORV_DIE,
	ORV_PAIN,
	ORV_NEAR_PANIC,
	ORV_RELOAD,
	ORV_SEES_GRENADES,
	ORV_LOST_VISUAL,
	ORV_REFIND_ENEMY,
	ORV_SQUADMEMBER_LOST,
	ORV_HEAR_SUSPICIOUS,
	ORV_IDLE,

	ORV_COUNT
};

// PLACEHOLDER INDICES: edit each list to the file numbers that actually exist.
static const int s_OrdAnnounceAttackIdx[] = { 1, 2, 3 };
static const int s_OrdOrderAdvanceIdx[] = { 1, 2, 3 };
static const int s_OrdDeployManhackIdx[] = { 1, 2, 3 };
static const int s_OrdLeaderDownIdx[] = { 1, 2, 3 };
static const int s_OrdAnnounceKillIdx[] = { 1, 2, 3 };
static const int s_OrdCombatIdleIdx[] = { 10, 20, 30 };		// 3-digit names
static const int s_OrdDieIdx[] = { 1, 2, 3 };
static const int s_OrdPainIdx[] = { 1, 2, 3 };
static const int s_OrdNearPanicIdx[] = { 1, 2, 3 };
static const int s_OrdReloadIdx[] = { 1, 2, 3 };
static const int s_OrdSeesGrenadesIdx[] = { 1, 2, 3 };
static const int s_OrdLostVisualIdx[] = { 1, 2, 3 };
static const int s_OrdRefindEnemyIdx[] = { 1, 2, 3 };
static const int s_OrdSquadLostIdx[] = { 1, 2, 3 };
static const int s_OrdHearSuspiciousIdx[] = { 1, 2, 3 };
static const int s_OrdIdleIdx[] = { 1, 2, 3 };

struct OrdinalVoiceSet_t
{
	const char* pszPrefix;
	const int* pIndices;
	int			nCount;
	int			nDigits;
};

#define ORV_SET(prefix, arr, digits) { prefix, arr, ARRAYSIZE(arr), digits }

// Order must match OrdinalVoice_t
static const OrdinalVoiceSet_t s_OrdVoiceSets[ORV_COUNT] =
{
	ORV_SET("announceattack",		s_OrdAnnounceAttackIdx,	2),
	ORV_SET("order_advance",		s_OrdOrderAdvanceIdx,	2),
	ORV_SET("deploy_manhack",		s_OrdDeployManhackIdx,	2),
	ORV_SET("leader_down",			s_OrdLeaderDownIdx,		2),
	ORV_SET("announcekill",			s_OrdAnnounceKillIdx,	2),
	ORV_SET("combat_idle",			s_OrdCombatIdleIdx,		3),
	ORV_SET("die",					s_OrdDieIdx,			2),
	ORV_SET("pain",					s_OrdPainIdx,			2),
	ORV_SET("nearpanic",			s_OrdNearPanicIdx,		2),
	ORV_SET("reload",				s_OrdReloadIdx,			2),
	ORV_SET("sees_grenades",		s_OrdSeesGrenadesIdx,	2),
	ORV_SET("lostvisual",			s_OrdLostVisualIdx,		2),
	ORV_SET("refindenemy",			s_OrdRefindEnemyIdx,	2),
	ORV_SET("squadmemberlost",		s_OrdSquadLostIdx,		2),
	ORV_SET("hear_suspicious",		s_OrdHearSuspiciousIdx,	2),
	ORV_SET("idle",					s_OrdIdleIdx,			2),
};

static void OrdinalBuildVoicePath(char* pszOut, int nOutLen, const OrdinalVoiceSet_t& set, int iIndex)
{
	Q_snprintf(pszOut, nOutLen, "%s%s_%0*d.wav", ORDINAL_VOICE_DIR, set.pszPrefix, set.nDigits, iIndex);
}

BEGIN_DATADESC(CNPC_CombineOrdinal)
DEFINE_FIELD(m_bHadManhack, FIELD_BOOLEAN),
DEFINE_FIELD(m_flNextManhackTime, FIELD_TIME),
DEFINE_FIELD(m_bManhackFlagByUs, FIELD_BOOLEAN),
DEFINE_FIELD(m_flNextOrderTime, FIELD_TIME),
DEFINE_FIELD(m_flNextVoiceTime, FIELD_TIME),
DEFINE_FIELD(m_flNextPainVoiceTime, FIELD_TIME),
DEFINE_FIELD(m_flNextCombatIdleTime, FIELD_TIME),
DEFINE_FIELD(m_bNearPanicSpoken, FIELD_BOOLEAN),
DEFINE_FIELD(m_bKillSpoken, FIELD_BOOLEAN),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::Spawn(void)
{
	// Must happen before BaseClass::Spawn(), which calls Precache() and SetModel()
	if (!GetModelName() || GetModelName() == NULL_STRING)
	{
		SetModelName(MAKE_STRING(ORDINAL_MODEL));
	}

	// The EZ2 CNPC_Combine constructor adds SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE
	// (for npc_create). An enemy officer is never a player ally and never regenerates.
	//RemoveSpawnFlags(SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE);

	if (m_spawnEquipment == NULL_STRING)
	{
		m_spawnEquipment = MAKE_STRING(ORDINAL_WEAPON);
	}

	// Manhacks: the base class (EZ) handles carrying, the bodygroup and the deploy
	// schedule. We only need to make sure he has some.
	if (m_iManhacks <= 0)
	{
		m_iManhacks = ordinal_manhack_count.GetInt();
	}

	m_bHadManhack = false;
	m_flNextManhackTime = 0.0f;
	m_bManhackFlagByUs = false;

	m_flNextOrderTime = 0.0f;

	// Voice state
	m_flNextVoiceTime = 0.0f;
	m_flNextPainVoiceTime = 0.0f;
	m_flNextCombatIdleTime = 0.0f;
	m_bNearPanicSpoken = false;
	m_bKillSpoken = false;

	// No grenades (the in-game Ordinal doesn't use them)
	m_iNumGrenades = 0;

	BaseClass::Spawn();

	// CNPC_CombineS::Spawn() overwrites health, so apply ours AFTER the base spawn.
	SetHealth(sk_ordinal_health.GetFloat());
	SetMaxHealth(sk_ordinal_health.GetFloat());
}

//-----------------------------------------------------------------------------
// Purpose: Precache all resources
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::Precache()
{
	PrecacheModel(ORDINAL_MODEL);

	UTIL_PrecacheOther("npc_manhack");

	if (ordinal_voice_enable.GetBool())
	{
		for (int iSet = 0; iSet < ORV_COUNT; iSet++)
		{
			const OrdinalVoiceSet_t& set = s_OrdVoiceSets[iSet];
			for (int i = 0; i < set.nCount; i++)
			{
				char szPath[256];
				OrdinalBuildVoicePath(szPath, sizeof(szPath), set, set.pIndices[i]);
				PrecacheSound(szPath);
			}
		}
	}

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: Post-spawn initialization
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::PostNPCInit()
{
	if (!GetActiveWeapon())
	{
		GiveWeapon(MAKE_STRING(ORDINAL_WEAPON));
	}

	BaseClass::PostNPCInit();

	SetState(NPC_STATE_ALERT);
}

//-----------------------------------------------------------------------------
// Purpose: Per-think logic. Runs inside the NPC's own AI think.
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::PrescheduleThink(void)
{
	BaseClass::PrescheduleThink();

	if (!IsAlive())
		return;

	// Infinite reserve ammo: keep the inventory topped off so he can always reload,
	// while the clip empties naturally.
	CBaseCombatWeapon* pWeapon = GetActiveWeapon();
	if (pWeapon && pWeapon->m_iPrimaryAmmoType != -1)
	{
		SetAmmoCount(999, pWeapon->m_iPrimaryAmmoType);
	}

	UpdateManhackState();
	UpdateSquadOrders();
	UpdateVoice();
}

//-----------------------------------------------------------------------------
// Purpose: The stock Combine deploys a manhack the moment it has an enemy and none
//			out. CanDeployManhack() isn't virtual, but it honors
//			SF_COMBINE_NO_MANHACK_DEPLOY, so we use that flag to impose a cooldown
//			after his manhack dies. (A flag the mapper set is left alone.)
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::UpdateManhackState(void)
{
	const bool bHaveManhack = (m_hManhack != NULL);

	if (bHaveManhack && !m_bHadManhack)
	{
		// Just released one
		PlayVoice(ORV_DEPLOY_MANHACK, true);

		if (ordinal_debug.GetBool())
		{
			DevMsg("Ordinal: manhack deployed (%d left)\n", m_iManhacks);
		}
	}
	else if (!bHaveManhack && m_bHadManhack)
	{
		// His manhack died (the base class clears m_hManhack in NotifyDeadFriend)
		m_flNextManhackTime = gpGlobals->curtime + ordinal_manhack_cooldown.GetFloat();

		if (ordinal_debug.GetBool())
		{
			DevMsg("Ordinal: manhack lost, next deploy at %.1f\n", m_flNextManhackTime);
		}
	}

	m_bHadManhack = bHaveManhack;

	const bool bOnCooldown = gpGlobals->curtime < m_flNextManhackTime;

	if (bOnCooldown)
	{
		if (!m_bManhackFlagByUs && !HasSpawnFlags(SF_COMBINE_NO_MANHACK_DEPLOY))
		{
			AddSpawnFlags(SF_COMBINE_NO_MANHACK_DEPLOY);
			m_bManhackFlagByUs = true;
		}
	}
	else if (m_bManhackFlagByUs)
	{
		RemoveSpawnFlags(SF_COMBINE_NO_MANHACK_DEPLOY);
		m_bManhackFlagByUs = false;
	}
}

//-----------------------------------------------------------------------------
// Purpose: When the enemy has been hiding for a while, bark an advance order.
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::UpdateSquadOrders(void)
{
	if (!ordinal_orders_enabled.GetBool())
		return;

	if (GetState() != NPC_STATE_COMBAT)
		return;

	CBaseEntity* pEnemy = GetEnemy();
	if (!pEnemy || !pEnemy->IsAlive())
		return;

	if (gpGlobals->curtime < m_flNextOrderTime)
		return;

	// Only when the enemy is hiding
	if (HasCondition(COND_SEE_ENEMY))
		return;

	if (gpGlobals->curtime - GetEnemyLastTimeSeen() < ordinal_order_hide_time.GetFloat())
		return;

	if (IssueAdvanceOrder())
	{
		m_flNextOrderTime = gpGlobals->curtime + ordinal_order_cooldown.GetFloat();
		PlayVoice(ORV_ORDER_ADVANCE, true);
	}
	else
	{
		// Nobody to order, check again shortly
		m_flNextOrderTime = gpGlobals->curtime + 3.0f;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Tell nearby soldiers where the enemy is and have them push it.
//			Uses the stock "pressure enemy until close" tactical variant, which
//			resets itself once the soldier is within 30 feet.
//-----------------------------------------------------------------------------
bool CNPC_CombineOrdinal::IssueAdvanceOrder(void)
{
	CAI_Squad* pSquad = GetSquad();
	CBaseEntity* pEnemy = GetEnemy();
	if (!pSquad || !pEnemy)
		return false;

	const int nMax = ordinal_order_max_units.GetInt();
	int nOrdered = 0;

	AISquadIter_t iter;
	for (CAI_BaseNPC* pMember = pSquad->GetFirstMember(&iter); pMember && nOrdered < nMax; pMember = pSquad->GetNextMember(&iter))
	{
		if (pMember == this || !pMember->IsAlive())
			continue;

		// Suppressors hold their ground by design; manhacks aren't soldiers
		if (FClassnameIs(pMember, "npc_combine_suppressor") || FClassnameIs(pMember, "npc_manhack"))
			continue;

		CNPC_Combine* pCombine = dynamic_cast<CNPC_Combine*>(pMember);
		if (!pCombine)
			continue;

		// Already pushing
		if (pCombine->m_iTacticalVariant != ORDINAL_TACTICAL_DEFAULT)
			continue;

		// Make sure they know where the enemy is, then make them re-evaluate
		pCombine->UpdateEnemyMemory(pEnemy, GetEnemyLKP(), this);
		pCombine->m_iTacticalVariant = ORDINAL_TACTICAL_PRESSURE_UNTIL_CLOSE;
		pCombine->ClearSchedule("Ordinal ordered advance");

		nOrdered++;
	}

	if (ordinal_debug.GetBool())
	{
		DevMsg("Ordinal: advance order issued to %d squadmate(s)\n", nOrdered);
	}

	return nOrdered > 0;
}

//-----------------------------------------------------------------------------
// Purpose: The squad reports the loss over the radio and falls apart.
//			Skipped if another Ordinal is still alive in the squad.
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::HandleLeaderDown(void)
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return;

	AISquadIter_t iter;

	// Another Ordinal takes command
	for (CAI_BaseNPC* pMember = pSquad->GetFirstMember(&iter); pMember; pMember = pSquad->GetNextMember(&iter))
	{
		if (pMember != this && pMember->IsAlive() && dynamic_cast<CNPC_CombineOrdinal*>(pMember))
			return;
	}

	CBaseEntity* pReporter = NULL;
	const WeaponProficiency_t prof = (WeaponProficiency_t)clamp(ordinal_leader_down_proficiency.GetInt(), (int)WEAPON_PROFICIENCY_POOR, (int)WEAPON_PROFICIENCY_PERFECT);

	for (CAI_BaseNPC* pMember = pSquad->GetFirstMember(&iter); pMember; pMember = pSquad->GetNextMember(&iter))
	{
		if (pMember == this || !pMember->IsAlive())
			continue;

		CNPC_Combine* pCombine = dynamic_cast<CNPC_Combine*>(pMember);
		if (!pCombine)
			continue;

		// No more coordinated pushes, worse aim, and a moment of confusion
		pCombine->m_iTacticalVariant = ORDINAL_TACTICAL_DEFAULT;
		pCombine->SetCurrentWeaponProficiency(prof);
		pCombine->m_flNextAttack = gpGlobals->curtime + ordinal_leader_down_stun.GetFloat();
		pCombine->ClearSchedule("Squad leader down");

		if (!pReporter)
		{
			pReporter = pCombine;
		}
	}

	// One squadmate reports to Overwatch
	if (pReporter)
	{
		PlayVoiceOn(pReporter, ORV_LEADER_DOWN_REPORT, true);
	}

	if (ordinal_debug.GetBool())
	{
		DevMsg("Ordinal: leader down, squad disorganized\n");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Disorganize the squad when we die (manhack release is handled by
//			CNPC_Combine::Event_Killed).
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::Event_Killed(const CTakeDamageInfo& info)
{
	if (ordinal_leader_down_enabled.GetBool())
	{
		HandleLeaderDown();
	}

	BaseClass::Event_Killed(info);
}

//-----------------------------------------------------------------------------
// Purpose: Ordinal accuracy
//-----------------------------------------------------------------------------
WeaponProficiency_t CNPC_CombineOrdinal::CalcWeaponProficiency(CBaseCombatWeapon* pWeapon)
{
	if (pWeapon)
	{
		int iProf = clamp(ordinal_weapon_proficiency.GetInt(), (int)WEAPON_PROFICIENCY_POOR, (int)WEAPON_PROFICIENCY_PERFECT);
		return (WeaponProficiency_t)iProf;
	}

	return BaseClass::CalcWeaponProficiency(pWeapon);
}

//-----------------------------------------------------------------------------
// Purpose: Scale outgoing bullet damage by ordinal_damage_multiplier
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::FireBullets(const FireBulletsInfo_t& info)
{
	FireBulletsInfo_t modified = info;

	// A damage of 0 means "use the ammo definition" (sk_npc_dmg_*)
	float flBase = (info.m_flDamage > 0) ? info.m_flDamage : GetAmmoDef()->NPCDamage(info.m_iAmmoType);

	if (flBase <= 0.0f)
	{
		flBase = ordinal_fallback_damage.GetFloat();
	}

	modified.m_flDamage = flBase * ordinal_damage_multiplier.GetFloat();
	modified.m_iPlayerDamage = (int)modified.m_flDamage;

	if (ordinal_debug.GetBool())
	{
		DevMsg("Ordinal FireBullets: %d shots, damage %.2f (base %.2f x %.2f), ammo %d\n",
			info.m_iShots, modified.m_flDamage, flBase, ordinal_damage_multiplier.GetFloat(), info.m_iAmmoType);
	}

	BaseClass::FireBullets(modified);
}

//-----------------------------------------------------------------------------
// Purpose: Play a random line from a voice category on the given speaker.
//			bForce skips the min-gap throttle (only throttled for ourselves).
//-----------------------------------------------------------------------------
bool CNPC_CombineOrdinal::PlayVoiceOn(CBaseEntity* pSpeaker, int iVoice, bool bForce)
{
	if (!ordinal_voice_enable.GetBool() || !pSpeaker)
		return false;

	if (iVoice < 0 || iVoice >= ORV_COUNT)
		return false;

	const bool bSelf = (pSpeaker == this);

	if (bSelf && !bForce && gpGlobals->curtime < m_flNextVoiceTime)
		return false;

	const OrdinalVoiceSet_t& set = s_OrdVoiceSets[iVoice];
	int iPick = set.pIndices[random->RandomInt(0, set.nCount - 1)];

	char szPath[256];
	OrdinalBuildVoicePath(szPath, sizeof(szPath), set, iPick);

	CPASAttenuationFilter filter(pSpeaker);

	EmitSound_t params;
	params.m_pSoundName = szPath;
	params.m_nChannel = CHAN_VOICE;
	params.m_flVolume = 1.0f;
	params.m_SoundLevel = SNDLVL_TALKING;

	pSpeaker->EmitSound(filter, pSpeaker->entindex(), params);

	if (bSelf)
	{
		m_flNextVoiceTime = gpGlobals->curtime + ordinal_voice_min_gap.GetFloat();
	}

	if (ordinal_debug.GetBool())
	{
		DevMsg("Ordinal voice: %s\n", szPath);
	}

	return true;
}

bool CNPC_CombineOrdinal::PlayVoice(int iVoice, bool bForce)
{
	return PlayVoiceOn(this, iVoice, bForce);
}

//-----------------------------------------------------------------------------
// Purpose: Situational voice lines that don't have a dedicated hook. Called
//			every AI think; everything here is throttled.
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::UpdateVoice()
{
	if (!ordinal_voice_enable.GetBool() || !IsAlive())
		return;

	const NPC_STATE state = GetState();

	// Dead enemy: announce the kill once per kill
	if (HasCondition(COND_ENEMY_DEAD))
	{
		if (!m_bKillSpoken)
		{
			m_bKillSpoken = true;
			PlayVoice(ORV_ANNOUNCE_KILL, true);
		}
	}
	else
	{
		m_bKillSpoken = false;
	}

	// Low health: panic line, re-armed once he's healed back up
	const float flHealthFrac = (float)GetHealth() / (float)MAX(GetMaxHealth(), 1);
	if (flHealthFrac < 0.2f)
	{
		if (!m_bNearPanicSpoken && PlayVoice(ORV_NEAR_PANIC))
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
		if (HasCondition(COND_HEAR_DANGER))
		{
			PlayVoice(ORV_SEES_GRENADES);
		}
		else if (HasCondition(COND_NO_PRIMARY_AMMO) || HasCondition(COND_LOW_PRIMARY_AMMO))
		{
			PlayVoice(ORV_RELOAD);
		}
		else if (HasCondition(COND_NEW_ENEMY))
		{
			PlayVoice(ORV_ANNOUNCE_ATTACK);
		}
		else if (gpGlobals->curtime >= m_flNextCombatIdleTime)
		{
			m_flNextCombatIdleTime = gpGlobals->curtime + random->RandomFloat(8.0f, 16.0f);
			PlayVoice(ORV_COMBAT_IDLE);
		}
	}
	else if (state == NPC_STATE_IDLE || state == NPC_STATE_ALERT)
	{
		if (HasCondition(COND_HEAR_COMBAT) || HasCondition(COND_HEAR_PLAYER) || HasCondition(COND_HEAR_WORLD))
		{
			PlayVoice(ORV_HEAR_SUSPICIOUS);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Voice overrides. With ordinal_voice_enable 0 these use the stock
//			Combine voice.
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::PainSound(const CTakeDamageInfo& info)
{
	if (!ordinal_voice_enable.GetBool())
	{
		BaseClass::PainSound(info);
		return;
	}

	if (GetFlags() & FL_DISSOLVING)
		return;

	if (gpGlobals->curtime < m_flNextPainVoiceTime)
		return;

	m_flNextPainVoiceTime = gpGlobals->curtime + 1.0f;
	PlayVoice(ORV_PAIN, true);
}

void CNPC_CombineOrdinal::DeathSound(const CTakeDamageInfo& info)
{
	if (!ordinal_voice_enable.GetBool())
	{
		BaseClass::DeathSound(info);
		return;
	}

	if (GetFlags() & FL_DISSOLVING)
		return;

	PlayVoice(ORV_DIE, true);
}

void CNPC_CombineOrdinal::IdleSound(void)
{
	if (!ordinal_voice_enable.GetBool())
	{
		BaseClass::IdleSound();
		return;
	}

	PlayVoice(ORV_IDLE);
}

void CNPC_CombineOrdinal::LostEnemySound(CBaseEntity* pEnemy)
{
	if (!ordinal_voice_enable.GetBool())
	{
		BaseClass::LostEnemySound(pEnemy);
		return;
	}

	PlayVoice(ORV_LOST_VISUAL);
}

void CNPC_CombineOrdinal::FoundEnemySound(CBaseEntity* pEnemy)
{
	if (!ordinal_voice_enable.GetBool())
	{
		BaseClass::FoundEnemySound(pEnemy);
		return;
	}

	PlayVoice(ORV_REFIND_ENEMY);
}

//-----------------------------------------------------------------------------
// Purpose: The base class MUST see our manhack die (it clears m_hManhack there),
//			otherwise the Ordinal would never be able to deploy another one.
//-----------------------------------------------------------------------------
void CNPC_CombineOrdinal::NotifyDeadFriend(CBaseEntity* pFriend)
{
	if (pFriend == m_hManhack || !ordinal_voice_enable.GetBool())
	{
		BaseClass::NotifyDeadFriend(pFriend);
		return;
	}

	// No call-outs for squadmates' dead manhacks
	if (FClassnameIs(pFriend, "npc_manhack"))
		return;

	PlayVoice(ORV_SQUADMEMBER_LOST);
}