//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Suppressor - A heavily armored Combine soldier that stands
//			its ground and pins enemies down with a minigun while others flank.
//
//=============================================================================//

#include "cbase.h"
#include "ai_hull.h"
#include "ai_motor.h"
#include "ai_squad.h"
#include "npc_combine.h"
#include "npc_combine_suppressor.h"
#include "npcevent.h"
#include "engine/IEngineSound.h"
#include "soundent.h"
#include "basecombatweapon.h"
#include "ammodef.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS(npc_combine_suppressor, CNPC_CombineSuppressor);

// Convars
ConVar	sk_suppressor_health("sk_suppressor_health", "400");
ConVar	suppressor_damage_multiplier("suppressor_damage_multiplier", "1.0");
ConVar	suppressor_fallback_damage("suppressor_fallback_damage", "3", FCVAR_NONE, "Per-bullet damage used if the ammo definition resolves to 0.");
ConVar	suppressor_weapon_proficiency("suppressor_weapon_proficiency", "1", FCVAR_NONE, "Accuracy with the minigun: 0=poor 1=average 2=good 3=very good 4=perfect. He is meant to pin, not snipe.");

// NOTE: the actual per-trigger-pull burst length comes from the weapon (see sk_css_m249_npc_burst_min/max).
// These control the longer fire/pause cycle on top of that.
ConVar	suppressor_burst_min("suppressor_burst_min", "5", FCVAR_NONE, "Minimum seconds of continuous fire before a pause.");
ConVar	suppressor_burst_max("suppressor_burst_max", "9", FCVAR_NONE, "Maximum seconds of continuous fire before a pause.");
ConVar	suppressor_pause_min("suppressor_pause_min", "1.5", FCVAR_NONE, "Minimum seconds he stops firing between bursts (he is vulnerable here).");
ConVar	suppressor_pause_max("suppressor_pause_max", "2.5", FCVAR_NONE, "Maximum seconds he stops firing between bursts.");
ConVar	suppressor_pin_time("suppressor_pin_time", "5", FCVAR_NONE, "Seconds he keeps suppressing the enemy's last known position after losing sight of them.");
ConVar	suppressor_hold_time("suppressor_hold_time", "10", FCVAR_NONE, "Seconds he holds position after losing the enemy before the normal Combine AI may reposition him.");

ConVar	suppressor_backup_range("suppressor_backup_range", "350", FCVAR_NONE, "A visible enemy closer than this makes him back away while firing. 0 = never back up.");
ConVar	suppressor_backup_cooldown("suppressor_backup_cooldown", "0.75", FCVAR_NONE, "Minimum seconds between starting back-away moves.");
ConVar	suppressor_backup_fail_delay("suppressor_backup_fail_delay", "3", FCVAR_NONE, "If backing away fails (cornered), seconds before he tries again. He stands and fires meanwhile.");

ConVar	suppressor_investigate_enabled("suppressor_investigate_enabled", "1", FCVAR_NONE, "1 = Suppressor walks to investigate sounds (gunfire, player noise, world noise) from things he hasn't seen.");
ConVar	suppressor_investigate_cooldown("suppressor_investigate_cooldown", "3", FCVAR_NONE, "Minimum seconds between starting investigations.");

ConVar	suppressor_debug("suppressor_debug", "0", FCVAR_NONE, "1 = print burst/voice/damage info.");

// ---------------------------------------------------------------------------
// Voice lines
// ---------------------------------------------------------------------------
// Folder (under sound/) holding the voice .wav files. Must end in a slash.
// Files are expected at sound/vj_hla_suppressor/sup/<name>_<NN>.wav
#define SUPPRESSOR_VOICE_DIR		"vj_hla_suppressor/sup/"

ConVar	suppressor_voice_enable("suppressor_voice_enable", "1", FCVAR_NONE, "Enable Suppressor voice lines.");
ConVar	suppressor_voice_min_gap("suppressor_voice_min_gap", "2.5", FCVAR_NONE, "Minimum seconds between non-critical voice lines.");

enum SuppressorVoice_t
{
	SUPV_ANNOUNCE_ATTACK = 0,
	SUPV_FIRING,
	SUPV_SUPPRESSING,
	SUPV_ANNOUNCE_KILL,
	SUPV_ANNOUNCE_KILL_PLAYER,
	SUPV_LAUGH,
	SUPV_COMBAT_IDLE,
	SUPV_TAUNT,
	SUPV_DIE,
	SUPV_DIE_BY_PLAYER,
	SUPV_PAIN,
	SUPV_INJURED,
	SUPV_NEAR_PANIC,
	SUPV_RELOAD,
	SUPV_SEES_GRENADES,
	SUPV_DANGER_GRENADE,
	SUPV_SQUADMEMBER_LOST,
	SUPV_LAST_MAN,
	SUPV_LOST_VISUAL,
	SUPV_LOST_VISUAL_PLAYER,
	SUPV_REFIND_ENEMY,
	SUPV_HEAR_SUSPICIOUS,
	SUPV_IDLE,
	SUPV_RETREAT,

	SUPV_COUNT
};

// File numbering has gaps, so each category lists the indices that actually exist.
static const int s_SupAnnounceAttackIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static const int s_SupFiringIdx[] = { 1, 2, 4, 5, 6, 7, 8, 9, 10 };
static const int s_SupSuppressingIdx[] = { 1, 2, 3, 4, 5 };
static const int s_SupAnnounceKillIdx[] = { 1, 2, 3, 4, 5, 6, 8, 9, 11, 12, 13, 14, 15 };
static const int s_SupAnnounceKillPlayerIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static const int s_SupLaughIdx[] = { 1, 2, 3, 4, 5, 6 };
// combat_idle_NNN: 3-digit names (010, 012, 020, 021, 022, ... 200, 201, 202)
static const int s_SupCombatIdleIdx[] =
{
	10, 12, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62, 70, 71, 72,
	80, 81, 82, 90, 91, 92, 100, 101, 102, 110, 111, 112, 120, 121, 122, 130, 131, 132,
	140, 141, 142, 150, 151, 152, 160, 161, 162, 170, 171, 172, 180, 181, 182, 190, 191, 192,
	200, 201, 202
};
// taunt_NNN: 3-digit names (010, 011, 012, ... 200, 201, 202)
static const int s_SupTauntIdx[] =
{
	10, 11, 12, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62, 70, 71, 72,
	80, 81, 82, 90, 91, 92, 100, 101, 102, 110, 111, 112, 120, 121, 122, 130, 131, 132,
	140, 141, 142, 150, 151, 152, 160, 161, 162, 170, 171, 172, 180, 181, 182, 190, 191, 192,
	200, 201, 202
};
static const int s_SupDieIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static const int s_SupDieByPlayerIdx[] = { 18, 19, 20, 21, 22 };
static const int s_SupPainIdx[] = { 2, 3, 4, 5, 6, 7, 8, 10 };
static const int s_SupInjuredIdx[] = { 1, 2, 3, 4, 5, 6 };
static const int s_SupNearPanicIdx[] = { 2, 4, 8, 10 };
static const int s_SupReloadIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static const int s_SupSeesGrenadesIdx[] = { 1, 2, 3, 4 };
static const int s_SupDangerGrenadeIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const int s_SupSquadLostIdx[] = { 1, 2, 3, 5, 6, 7, 8, 9, 10, 11 };
static const int s_SupLastManIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static const int s_SupLostVisualIdx[] = { 1, 2, 3, 4, 6, 7, 8, 9, 10 };
static const int s_SupLostVisualPlayerIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const int s_SupRefindEnemyIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
static const int s_SupHearSuspiciousIdx[] = { 1, 2, 3, 4, 5, 6 };
static const int s_SupIdleIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
static const int s_SupRetreatIdx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

struct SuppressorVoiceSet_t
{
	const char* pszPrefix;
	const int* pIndices;
	int			nCount;
	int			nDigits;
};

#define SUPV_SET(prefix, arr, digits) { prefix, arr, ARRAYSIZE(arr), digits }

// Order must match SuppressorVoice_t
static const SuppressorVoiceSet_t s_SupVoiceSets[SUPV_COUNT] =
{
	SUPV_SET("announceattack",				s_SupAnnounceAttackIdx,		2),
	SUPV_SET("firing",						s_SupFiringIdx,				2),
	SUPV_SET("suppressing",					s_SupSuppressingIdx,		2),
	SUPV_SET("announcekill",				s_SupAnnounceKillIdx,		2),
	SUPV_SET("announcekill_player",			s_SupAnnounceKillPlayerIdx,	2),
	SUPV_SET("laugh",						s_SupLaughIdx,				2),
	SUPV_SET("combat_idle",					s_SupCombatIdleIdx,			3),
	SUPV_SET("taunt",						s_SupTauntIdx,				3),
	SUPV_SET("die",							s_SupDieIdx,				2),
	SUPV_SET("die_by_player",				s_SupDieByPlayerIdx,		2),
	SUPV_SET("pain",						s_SupPainIdx,				2),
	SUPV_SET("injured",						s_SupInjuredIdx,			2),
	SUPV_SET("nearpanic",					s_SupNearPanicIdx,			2),
	SUPV_SET("reload",						s_SupReloadIdx,				2),
	SUPV_SET("sees_grenades",				s_SupSeesGrenadesIdx,		2),
	SUPV_SET("danger_grenade",				s_SupDangerGrenadeIdx,		2),
	SUPV_SET("squadmemberlost",				s_SupSquadLostIdx,			2),
	SUPV_SET("squadmemberlost_lastman",		s_SupLastManIdx,			2),
	SUPV_SET("lostvisual",					s_SupLostVisualIdx,			2),
	SUPV_SET("lostvisual_player",			s_SupLostVisualPlayerIdx,	2),
	SUPV_SET("refindenemy",					s_SupRefindEnemyIdx,		2),
	SUPV_SET("hear_suspicious",				s_SupHearSuspiciousIdx,		2),
	SUPV_SET("idle",						s_SupIdleIdx,				2),
	SUPV_SET("retreat",						s_SupRetreatIdx,			2),
};

static void SuppressorBuildVoicePath(char* pszOut, int nOutLen, const SuppressorVoiceSet_t& set, int iIndex)
{
	Q_snprintf(pszOut, nOutLen, "%s%s_%0*d.wav", SUPPRESSOR_VOICE_DIR, set.pszPrefix, set.nDigits, iIndex);
}

#define SUPPRESSOR_MODEL			"models/combine_suppressor.mdl"
#define SUPPRESSOR_WEAPON			"weapon_css_m249"

BEGIN_DATADESC(CNPC_CombineSuppressor)
DEFINE_FIELD(m_bInBurst, FIELD_BOOLEAN),
DEFINE_FIELD(m_flBurstEndTime, FIELD_TIME),
DEFINE_FIELD(m_flNextBurstTime, FIELD_TIME),
DEFINE_FIELD(m_flNextBackAwayTime, FIELD_TIME),
DEFINE_FIELD(m_flNextInvestigateTime, FIELD_TIME),
DEFINE_FIELD(m_flNextVoiceTime, FIELD_TIME),
DEFINE_FIELD(m_flNextPainVoiceTime, FIELD_TIME),
DEFINE_FIELD(m_flNextCombatIdleTime, FIELD_TIME),
DEFINE_FIELD(m_bNearPanicSpoken, FIELD_BOOLEAN),
DEFINE_FIELD(m_bKillSpoken, FIELD_BOOLEAN),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::Spawn(void)
{
	// Set default model if not specified. Must happen before BaseClass::Spawn(),
	// which calls Precache() and SetModel() for us.
	if (!GetModelName() || GetModelName() == NULL_STRING)
	{
		SetModelName(MAKE_STRING(SUPPRESSOR_MODEL));
	}

	// The EZ2 CNPC_Combine constructor adds SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE
	// (for npc_create). We never want an enemy heavy to be a player ally or regenerate.
	//RemoveSpawnFlags(SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE);

	// Equip the minigun through the normal spawn-equipment path
	if (m_spawnEquipment == NULL_STRING)
	{
		m_spawnEquipment = MAKE_STRING(SUPPRESSOR_WEAPON);
	}

	// Burst state
	m_bInBurst = false;
	m_flBurstEndTime = 0.0f;
	m_flNextBurstTime = 0.0f;

	// Back-away state
	m_flNextBackAwayTime = 0.0f;

	// Investigation state
	m_flNextInvestigateTime = 0.0f;

	// Voice state
	m_flNextVoiceTime = 0.0f;
	m_flNextPainVoiceTime = 0.0f;
	m_flNextCombatIdleTime = 0.0f;
	m_bNearPanicSpoken = false;
	m_bKillSpoken = false;

	// No grenades
	m_iNumGrenades = 0;

	BaseClass::Spawn();

	// CNPC_CombineS::Spawn() overwrites health with sk_combine_s_health,
	// so our health has to be applied AFTER the base spawn.
	SetHealth(sk_suppressor_health.GetFloat());
	SetMaxHealth(sk_suppressor_health.GetFloat());
}

//-----------------------------------------------------------------------------
// Purpose: Precache all resources
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::Precache()
{
	PrecacheModel(SUPPRESSOR_MODEL);

	// Voice lines (raw .wav paths, relative to the sound/ folder)
	for (int iSet = 0; iSet < SUPV_COUNT; iSet++)
	{
		const SuppressorVoiceSet_t& set = s_SupVoiceSets[iSet];
		for (int i = 0; i < set.nCount; i++)
		{
			char szPath[256];
			SuppressorBuildVoicePath(szPath, sizeof(szPath), set, set.pIndices[i]);
			PrecacheSound(szPath);
		}
	}

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Purpose: Post-spawn initialization
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::PostNPCInit()
{
	// Fallback in case spawn equipment was explicitly cleared and nothing else equipped
	if (!GetActiveWeapon())
	{
		GiveWeapon(MAKE_STRING(SUPPRESSOR_WEAPON));
	}

	BaseClass::PostNPCInit();

	SetState(NPC_STATE_ALERT);
}

//-----------------------------------------------------------------------------
// Purpose: Per-think logic. Runs inside the NPC's own AI think.
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::PrescheduleThink(void)
{
	BaseClass::PrescheduleThink();

	if (!IsAlive())
		return;

	// Some stock Combine schedules (e.g. SCHED_COMBINE_SUPPRESS) crouch the soldier.
	// A heavy never crouches.
	if (IsCrouching())
	{
		Stand();
	}

	// Infinite reserve ammo: keep the inventory topped off so he can always reload,
	// while the clip empties naturally (so he is vulnerable while reloading).
	CBaseCombatWeapon* pWeapon = GetActiveWeapon();
	if (pWeapon && pWeapon->m_iPrimaryAmmoType != -1)
	{
		SetAmmoCount(999, pWeapon->m_iPrimaryAmmoType);
	}

	// No enemy: reset the burst cycle so the next fight starts with a fresh burst
	if (!GetEnemy())
	{
		m_bInBurst = false;
		m_flNextBurstTime = 0.0f;
	}

	UpdateVoice();
}

//-----------------------------------------------------------------------------
// Purpose: Has a visible enemy come close enough that he should back up?
//-----------------------------------------------------------------------------
bool CNPC_CombineSuppressor::ShouldBackAway(CBaseEntity* pEnemy)
{
	const float flRange = suppressor_backup_range.GetFloat();
	if (flRange <= 0.0f || !pEnemy)
		return false;

	if (gpGlobals->curtime < m_flNextBackAwayTime)
		return false;

	return WorldSpaceCenter().DistToSqr(pEnemy->WorldSpaceCenter()) < (flRange * flRange);
}

//-----------------------------------------------------------------------------
// Purpose: Burst cycle. Returns true if he should be holding fire right now.
//			Fires for a few seconds, then pauses for a couple. The pause is when
//			he's vulnerable.
//-----------------------------------------------------------------------------
bool CNPC_CombineSuppressor::UpdateBurstCycle(bool bEnemySeen)
{
	if (m_bInBurst)
	{
		if (gpGlobals->curtime < m_flBurstEndTime)
			return false;

		// Burst over, start the pause
		m_bInBurst = false;
		m_flNextBurstTime = gpGlobals->curtime + random->RandomFloat(suppressor_pause_min.GetFloat(), suppressor_pause_max.GetFloat());

		if (suppressor_debug.GetBool())
		{
			DevMsg("Suppressor: burst over, pausing until %.1f\n", m_flNextBurstTime);
		}
		return true;
	}

	if (gpGlobals->curtime < m_flNextBurstTime)
		return true;

	// Pause over, start a new burst
	m_bInBurst = true;
	m_flBurstEndTime = gpGlobals->curtime + random->RandomFloat(suppressor_burst_min.GetFloat(), suppressor_burst_max.GetFloat());
	PlayVoice(bEnemySeen ? SUPV_FIRING : SUPV_SUPPRESSING);

	if (suppressor_debug.GetBool())
	{
		DevMsg("Suppressor: new burst until %.1f\n", m_flBurstEndTime);
	}
	return false;
}

bool CNPC_CombineSuppressor::IsInBurstPause(void) const
{
	return !m_bInBurst && gpGlobals->curtime < m_flNextBurstTime;
}

//-----------------------------------------------------------------------------
// Purpose: Don't let damage, low ammo, etc. break his firing schedules, and keep
//			the burst pause from being cut short, then let sounds break idle
//			schedules so he can investigate.
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::BuildScheduleTestBits(void)
{
	BaseClass::BuildScheduleTestBits();

	if (GetState() == NPC_STATE_COMBAT)
	{
		const bool bFiring = IsCurSchedule(SCHED_COMBINE_RANGE_ATTACK1, false) ||
			IsCurSchedule(SCHED_COMBINE_SUPPRESS, false) ||
			IsCurSchedule(SCHED_COMBINE_SIGNAL_SUPPRESS, false);

		if (bFiring)
		{
			// He stands his ground. Getting shot doesn't make him flinch or run,
			// and he fires until the clip is actually empty.
			ClearCustomInterruptCondition(COND_LIGHT_DAMAGE);
			ClearCustomInterruptCondition(COND_HEAVY_DAMAGE);
			ClearCustomInterruptCondition(COND_LOW_PRIMARY_AMMO);
		}

		// While backing away, nothing but a new enemy, a dead enemy or a kick
		// opportunity should end the move early.
		if (IsCurSchedule(SCHED_BACK_AWAY_FROM_ENEMY, false))
		{
			ClearCustomInterruptCondition(COND_LIGHT_DAMAGE);
			ClearCustomInterruptCondition(COND_HEAVY_DAMAGE);
			ClearCustomInterruptCondition(COND_LOW_PRIMARY_AMMO);
			ClearCustomInterruptCondition(COND_CAN_RANGE_ATTACK1);
			ClearCustomInterruptCondition(COND_CAN_RANGE_ATTACK2);
			ClearCustomInterruptCondition(COND_TOO_CLOSE_TO_ATTACK);
		}

		// During the pause, being able to attack again must not end the wait early
		if (IsCurSchedule(SCHED_COMBAT_FACE, false) && IsInBurstPause())
		{
			ClearCustomInterruptCondition(COND_CAN_RANGE_ATTACK1);
			ClearCustomInterruptCondition(COND_CAN_RANGE_ATTACK2);
		}
	}

	// Let sounds interrupt idle/alert/patrol schedules so he can go investigate.
	// Only while off cooldown, otherwise a lingering sound would thrash the schedule.
	if (suppressor_investigate_enabled.GetBool() &&
		(GetState() == NPC_STATE_IDLE || GetState() == NPC_STATE_ALERT) &&
		!IsCurSchedule(SCHED_INVESTIGATE_SOUND, false) &&
		gpGlobals->curtime >= m_flNextInvestigateTime)
	{
		SetCustomInterruptCondition(COND_HEAR_COMBAT);
		SetCustomInterruptCondition(COND_HEAR_PLAYER);
		SetCustomInterruptCondition(COND_HEAR_WORLD);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Suppression schedule selection
//-----------------------------------------------------------------------------
int CNPC_CombineSuppressor::SelectSchedule(void)
{
	int iSuppress = SelectSuppressSchedule();
	if (iSuppress != SCHED_NONE)
		return iSuppress;

	int iInvestigate = SelectInvestigateSchedule();
	if (iInvestigate != SCHED_NONE)
		return iInvestigate;

	return BaseClass::SelectSchedule();
}

//-----------------------------------------------------------------------------
// Purpose: Combat logic. Pin the enemy down from where he stands, backing up
//			if they get too close. Anything that returns SCHED_NONE falls through
//			to the normal Combine AI, which handles grenades/danger (relocating),
//			melee kicks, and so on.
//-----------------------------------------------------------------------------
int CNPC_CombineSuppressor::SelectSuppressSchedule(void)
{
	if (GetState() != NPC_STATE_COMBAT)
		return SCHED_NONE;

	CBaseEntity* pEnemy = GetEnemy();
	if (!pEnemy || !pEnemy->IsAlive() || !GetActiveWeapon())
		return SCHED_NONE;

	// Let the base AI deal with these (take cover from grenades, flinch from
	// physics objects, burning, bugbait, dead enemy, enemies he's afraid of)
	if (IsOnFire() ||
		HasCondition(COND_ENEMY_DEAD) ||
		HasCondition(COND_HEAR_DANGER) || HasCondition(COND_HEAR_MOVE_AWAY) ||
		HasCondition(COND_HEAR_PHYSICS_DANGER) ||
		HasCondition(COND_COMBINE_HIT_BY_BUGBAIT) ||
		HasCondition(COND_CAN_MELEE_ATTACK1) ||
		IRelationType(pEnemy) == D_FR)
		return SCHED_NONE;

	// Out of ammo: reload right where he stands. This is his weak moment.
	CBaseCombatWeapon* pWeapon = GetActiveWeapon();
	if (HasCondition(COND_NO_PRIMARY_AMMO) || (pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() <= 0))
	{
		return SCHED_RELOAD;
	}

	const bool bSeen = HasCondition(COND_SEE_ENEMY);
	const float flSinceSeen = gpGlobals->curtime - GetEnemyLastTimeSeen();

	// Enemy got close: back away. He keeps firing while he moves (move-and-shoot).
	if (bSeen && ShouldBackAway(pEnemy))
	{
		m_flNextBackAwayTime = gpGlobals->curtime + suppressor_backup_cooldown.GetFloat();
		PlayVoice(SUPV_RETREAT);

		if (suppressor_debug.GetBool())
		{
			DevMsg("Suppressor: enemy too close, backing away\n");
		}

		return SCHED_BACK_AWAY_FROM_ENEMY;
	}

	// A friendly is in the line of fire (probably moving in for the kill)
	if (HasCondition(COND_WEAPON_BLOCKED_BY_FRIEND))
		return SCHED_COMBAT_FACE;

	// Clear shot: fire
	if (bSeen && HasCondition(COND_CAN_RANGE_ATTACK1))
	{
		if (UpdateBurstCycle(true))
			return SCHED_COMBAT_FACE;

		return SCHED_RANGE_ATTACK1;
	}

	// Visible but out of range: hold position rather than chase
	if (bSeen && HasCondition(COND_TOO_FAR_TO_ATTACK))
		return SCHED_COMBAT_FACE;

	// Enemy ducked behind cover: keep the pressure on their last known position
	if (flSinceSeen <= suppressor_pin_time.GetFloat())
	{
		if (UpdateBurstCycle(false))
			return SCHED_COMBAT_FACE;

		return SCHED_RANGE_ATTACK1;
	}

	// Lost them for a bit: hold the line and wait for them to pop back out
	if (flSinceSeen <= suppressor_hold_time.GetFloat())
		return SCHED_COMBAT_FACE;

	// Lost them for good: normal AI may reposition him
	return SCHED_NONE;
}

//-----------------------------------------------------------------------------
// Purpose: If backing away fails (cornered), stop trying for a while and just
//			stand and fire instead of failing in a loop.
//-----------------------------------------------------------------------------
int CNPC_CombineSuppressor::SelectFailSchedule(int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode)
{
	if (failedSchedule == SCHED_BACK_AWAY_FROM_ENEMY)
	{
		m_flNextBackAwayTime = gpGlobals->curtime + suppressor_backup_fail_delay.GetFloat();

		if (suppressor_debug.GetBool())
		{
			DevMsg("Suppressor: back-away failed, holding position\n");
		}

		return SCHED_COMBAT_FACE;
	}

	return BaseClass::SelectFailSchedule(failedSchedule, failedTask, taskFailCode);
}

//-----------------------------------------------------------------------------
// Purpose: The stock Combine only investigates when it's in a squad AND is the
//			member nearest to a SOUND_COMBAT sound. React to combat, player and
//			world noises ourselves.
//-----------------------------------------------------------------------------
int CNPC_CombineSuppressor::SelectInvestigateSchedule(void)
{
	if (!suppressor_investigate_enabled.GetBool())
		return SCHED_NONE;

	const NPC_STATE state = GetState();
	if (state != NPC_STATE_IDLE && state != NPC_STATE_ALERT)
		return SCHED_NONE;

	if (GetEnemy() || IsInAScript() || IsOnFire())
		return SCHED_NONE;

	if (gpGlobals->curtime < m_flNextInvestigateTime)
		return SCHED_NONE;

	// Anything urgent is handled by the base class (take cover, flinch, etc.)
	if (HasCondition(COND_HEAR_DANGER) || HasCondition(COND_HEAR_MOVE_AWAY) ||
		HasCondition(COND_HEAR_PHYSICS_DANGER) ||
		HasCondition(COND_LIGHT_DAMAGE) || HasCondition(COND_HEAVY_DAMAGE))
		return SCHED_NONE;

	if (!HasCondition(COND_HEAR_COMBAT) && !HasCondition(COND_HEAR_PLAYER) && !HasCondition(COND_HEAR_WORLD))
		return SCHED_NONE;

	CSound* pSound = GetBestSound(SOUND_COMBAT | SOUND_PLAYER | SOUND_WORLD);
	if (!pSound)
		return SCHED_NONE;

	// Ignore our own noise and anything made by friends
	CBaseEntity* pOwner = pSound->m_hOwner;
	if (pOwner && (pOwner == this || IRelationType(pOwner) == D_LI))
		return SCHED_NONE;

	m_flNextInvestigateTime = gpGlobals->curtime + suppressor_investigate_cooldown.GetFloat();

	if (suppressor_debug.GetBool())
	{
		DevMsg("Suppressor: investigating sound (type %d)\n", pSound->m_iType);
	}

	return SCHED_INVESTIGATE_SOUND;
}

//-----------------------------------------------------------------------------
// Purpose: Keep him planted. The stock Combine runs to cover to reload, and
//			drops its attack when low on ammo; the Suppressor does neither.
//-----------------------------------------------------------------------------
int CNPC_CombineSuppressor::TranslateSchedule(int scheduleType)
{
	// Reload in place instead of running off to cover
	if (scheduleType == SCHED_HIDE_AND_RELOAD)
	{
		return SCHED_RELOAD;
	}

	// "Low" (not empty) ammo shouldn't stop the attack, he fires until the clip is dry
	if (scheduleType == SCHED_RANGE_ATTACK1 &&
		HasCondition(COND_LOW_PRIMARY_AMMO) && !HasCondition(COND_NO_PRIMARY_AMMO))
	{
		return SCHED_COMBINE_RANGE_ATTACK1;
	}

	return BaseClass::TranslateSchedule(scheduleType);
}

//-----------------------------------------------------------------------------
// Purpose: Minigun accuracy. He is meant to pin enemies, not snipe them.
//-----------------------------------------------------------------------------
WeaponProficiency_t CNPC_CombineSuppressor::CalcWeaponProficiency(CBaseCombatWeapon* pWeapon)
{
	if (pWeapon && FClassnameIs(pWeapon, SUPPRESSOR_WEAPON))
	{
		int iProf = clamp(suppressor_weapon_proficiency.GetInt(), (int)WEAPON_PROFICIENCY_POOR, (int)WEAPON_PROFICIENCY_PERFECT);
		return (WeaponProficiency_t)iProf;
	}

	return BaseClass::CalcWeaponProficiency(pWeapon);
}

//-----------------------------------------------------------------------------
// Purpose: Scale outgoing bullet damage by suppressor_damage_multiplier
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::FireBullets(const FireBulletsInfo_t& info)
{
	FireBulletsInfo_t modified = info;

	// A damage of 0 means "use the ammo definition" (sk_npc_dmg_*)
	float flBase = (info.m_flDamage > 0) ? info.m_flDamage : GetAmmoDef()->NPCDamage(info.m_iAmmoType);

	// If the skill files aren't loaded the ammo damage is 0, which means harmless bullets
	if (flBase <= 0.0f)
	{
		flBase = suppressor_fallback_damage.GetFloat();
	}

	modified.m_flDamage = flBase * suppressor_damage_multiplier.GetFloat();
	modified.m_iPlayerDamage = (int)modified.m_flDamage;

	if (suppressor_debug.GetBool())
	{
		DevMsg("Suppressor FireBullets: %d shots, damage %.2f (base %.2f x %.2f), ammo %d\n",
			info.m_iShots, modified.m_flDamage, flBase, suppressor_damage_multiplier.GetFloat(), info.m_iAmmoType);
	}

	BaseClass::FireBullets(modified);
}

//-----------------------------------------------------------------------------
// Purpose: Play a random line from a voice category on the voice channel.
//			bForce skips the min-gap throttle (used for pain/death/kill).
//-----------------------------------------------------------------------------
bool CNPC_CombineSuppressor::PlayVoice(int iVoice, bool bForce)
{
	if (!suppressor_voice_enable.GetBool())
		return false;

	if (iVoice < 0 || iVoice >= SUPV_COUNT)
		return false;

	if (!bForce && gpGlobals->curtime < m_flNextVoiceTime)
		return false;

	const SuppressorVoiceSet_t& set = s_SupVoiceSets[iVoice];
	int iPick = set.pIndices[random->RandomInt(0, set.nCount - 1)];

	char szPath[256];
	SuppressorBuildVoicePath(szPath, sizeof(szPath), set, iPick);

	CPASAttenuationFilter filter(this);

	EmitSound_t params;
	params.m_pSoundName = szPath;
	params.m_nChannel = CHAN_VOICE;		// a new line replaces whatever he was saying
	params.m_flVolume = 1.0f;
	params.m_SoundLevel = SNDLVL_TALKING;

	EmitSound(filter, entindex(), params);

	m_flNextVoiceTime = gpGlobals->curtime + suppressor_voice_min_gap.GetFloat();

	if (suppressor_debug.GetBool())
	{
		DevMsg("Suppressor voice: %s\n", szPath);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Situational voice lines that don't have a dedicated hook. Called
//			every AI think; everything here is throttled.
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::UpdateVoice()
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

			if (random->RandomInt(0, 99) < 30)
			{
				PlayVoice(SUPV_LAUGH, true);
			}
			else
			{
				CBaseEntity* pEnemy = GetEnemy();
				PlayVoice((pEnemy && pEnemy->IsPlayer()) ? SUPV_ANNOUNCE_KILL_PLAYER : SUPV_ANNOUNCE_KILL, true);
			}
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
		if (!m_bNearPanicSpoken && PlayVoice(SUPV_NEAR_PANIC))
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
			PlayVoice(random->RandomInt(0, 1) ? SUPV_SEES_GRENADES : SUPV_DANGER_GRENADE);
		}
		// --- Reloading
		else if (HasCondition(COND_NO_PRIMARY_AMMO))
		{
			PlayVoice(SUPV_RELOAD);
		}
		// --- Just spotted a new enemy
		else if (HasCondition(COND_NEW_ENEMY))
		{
			PlayVoice(SUPV_ANNOUNCE_ATTACK);
		}
		// --- Ambient combat chatter every so often (taunts if the enemy is hiding)
		else if (gpGlobals->curtime >= m_flNextCombatIdleTime)
		{
			m_flNextCombatIdleTime = gpGlobals->curtime + random->RandomFloat(8.0f, 16.0f);
			PlayVoice(HasCondition(COND_SEE_ENEMY) ? SUPV_COMBAT_IDLE : SUPV_TAUNT);
		}
	}
	else if (state == NPC_STATE_IDLE || state == NPC_STATE_ALERT)
	{
		// --- Heard something but nothing seen yet
		if (HasCondition(COND_HEAR_COMBAT) || HasCondition(COND_HEAR_PLAYER) || HasCondition(COND_HEAR_WORLD))
		{
			PlayVoice(SUPV_HEAR_SUSPICIOUS);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Voice overrides. These replace the Combine response-system / sentence
//			lines for these events.
//-----------------------------------------------------------------------------
void CNPC_CombineSuppressor::PainSound(const CTakeDamageInfo& info)
{
	if (GetFlags() & FL_DISSOLVING)
		return;

	if (gpGlobals->curtime < m_flNextPainVoiceTime)
		return;

	m_flNextPainVoiceTime = gpGlobals->curtime + 1.0f;

	// Once he's hurt, mix in the "injured" lines
	const float flHealthFrac = (float)GetHealth() / (float)MAX(GetMaxHealth(), 1);
	const bool bInjured = (flHealthFrac < 0.5f) && random->RandomInt(0, 1);

	PlayVoice(bInjured ? SUPV_INJURED : SUPV_PAIN, true);
}

void CNPC_CombineSuppressor::DeathSound(const CTakeDamageInfo& info)
{
	if (GetFlags() & FL_DISSOLVING)
		return;

	CBaseEntity* pAttacker = info.GetAttacker();
	PlayVoice((pAttacker && pAttacker->IsPlayer()) ? SUPV_DIE_BY_PLAYER : SUPV_DIE, true);
}

void CNPC_CombineSuppressor::IdleSound(void)
{
	PlayVoice(SUPV_IDLE);
}

void CNPC_CombineSuppressor::LostEnemySound(CBaseEntity* pEnemy)
{
	PlayVoice((pEnemy && pEnemy->IsPlayer()) ? SUPV_LOST_VISUAL_PLAYER : SUPV_LOST_VISUAL);
}

void CNPC_CombineSuppressor::FoundEnemySound(CBaseEntity* pEnemy)
{
	PlayVoice(SUPV_REFIND_ENEMY);
}

void CNPC_CombineSuppressor::NotifyDeadFriend(CBaseEntity* pFriend)
{
	CAI_Squad* pSquad = GetSquad();
	if (pSquad && pSquad->NumMembers() < 2)
	{
		PlayVoice(SUPV_LAST_MAN);
		return;
	}

	PlayVoice(SUPV_SQUADMEMBER_LOST);
}