//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: CNPC_CombineHLABase - shared behavior for the HLA Combine NPCs.
//			Right now that is sound investigation: gunfire, player footsteps
//			(running, etc.) and world noises from things they haven't seen.
//
//=============================================================================//

#include "cbase.h"
#include "ai_hull.h"
#include "ai_squad.h"
#include "npc_combine.h"
#include "npc_combine_hla.h"
#include "soundent.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// ---------------------------------------------------------------------------
// Convars (shared by all four NPCs; replaces the old per-class investigate convars)
// ---------------------------------------------------------------------------
ConVar	combine_investigate_enabled("combine_investigate_enabled", "1", FCVAR_NONE, "1 = HLA Combines walk to investigate sounds (gunfire, player noise, world noise) from things they haven't seen.");
ConVar	combine_investigate_cooldown("combine_investigate_cooldown", "3", FCVAR_NONE, "Minimum seconds between starting investigations.");
ConVar	combine_investigate_squad_closest("combine_investigate_squad_closest", "1", FCVAR_NONE, "1 = in a squad, only the member closest to the sound goes to look (so the whole squad doesn't pile onto one spot).");
ConVar	combine_investigate_debug("combine_investigate_debug", "0", FCVAR_NONE, "1 = print investigation info.");

BEGIN_DATADESC(CNPC_CombineHLABase)
DEFINE_FIELD(m_flNextInvestigateTime, FIELD_TIME),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CNPC_CombineHLABase::Spawn(void)
{
	m_flNextInvestigateTime = 0.0f;

	BaseClass::Spawn();
}

//-----------------------------------------------------------------------------
// Purpose: Make sure we're "listening" for the sound types we investigate, so the
//			COND_HEAR_* conditions get raised even if the stock list changes.
//-----------------------------------------------------------------------------
int CNPC_CombineHLABase::GetSoundInterests(void)
{
	return BaseClass::GetSoundInterests() | SOUND_COMBAT | SOUND_PLAYER | SOUND_WORLD;
}

//-----------------------------------------------------------------------------
// Purpose: Let sounds interrupt idle/alert/patrol schedules so we can go
//			investigate. Only while off cooldown, otherwise a lingering sound
//			would thrash the schedule.
//-----------------------------------------------------------------------------
void CNPC_CombineHLABase::BuildScheduleTestBits(void)
{
	BaseClass::BuildScheduleTestBits();

	if (combine_investigate_enabled.GetBool() &&
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
// Purpose: Investigate first, then the normal Combine AI. Subclasses that have
//			their own SelectSchedule should run it first and fall through to
//			BaseClass::SelectSchedule() so this still gets a turn.
//-----------------------------------------------------------------------------
int CNPC_CombineHLABase::SelectSchedule(void)
{
	int iInvestigate = SelectInvestigateSchedule();
	if (iInvestigate != SCHED_NONE)
		return iInvestigate;

	return BaseClass::SelectSchedule();
}

//-----------------------------------------------------------------------------
// Purpose: In a squad, only the closest free member investigates.
//-----------------------------------------------------------------------------
bool CNPC_CombineHLABase::IsBestSquadInvestigator(const Vector& vecSound)
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad || !combine_investigate_squad_closest.GetBool())
		return true;

	const float flMyDistSqr = GetAbsOrigin().DistToSqr(vecSound);

	AISquadIter_t iter;
	for (CAI_BaseNPC* pMember = pSquad->GetFirstMember(&iter); pMember; pMember = pSquad->GetNextMember(&iter))
	{
		if (pMember == this || !pMember->IsAlive())
			continue;

		// Only other HLA Combines count (skips manhacks etc.)
		CNPC_CombineHLABase* pOther = dynamic_cast<CNPC_CombineHLABase*>(pMember);
		if (!pOther)
			continue;

		// Someone who's already fighting isn't going to wander off
		if (pOther->GetEnemy())
			continue;

		if (pOther->GetAbsOrigin().DistToSqr(vecSound) < flMyDistSqr)
			return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: The stock Combine only investigates when it's in a squad AND is the
//			member nearest to a SOUND_COMBAT sound. React to combat, player and
//			world noises ourselves.
//-----------------------------------------------------------------------------
int CNPC_CombineHLABase::SelectInvestigateSchedule(void)
{
	if (!combine_investigate_enabled.GetBool())
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

	// Subclass veto (e.g. grunts don't wander far from their VIP)
	if (!ShouldInvestigateSound(pSound, pOwner))
		return SCHED_NONE;

	// Squad: let the closest member handle it
	if (!IsBestSquadInvestigator(pSound->GetSoundOrigin()))
		return SCHED_NONE;

	m_flNextInvestigateTime = gpGlobals->curtime + combine_investigate_cooldown.GetFloat();

	if (combine_investigate_debug.GetBool())
	{
		DevMsg("%s (%s): investigating sound (type %d)\n", GetDebugName(), GetClassname(), pSound->m_iType);
	}

	return SCHED_INVESTIGATE_SOUND;
}