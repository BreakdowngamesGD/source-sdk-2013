//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Grunt - combine_s clone that bodyguards the HLA NPCs.
//
//			* Picks a VIP (Ordinal > Wallhammer / Suppressor) and follows it using
//			  the stock follow behavior, which spreads followers around the target,
//			  so grunts naturally crowd around the VIP.
//			* In combat the stock CCombineFollowBehavior hands scheduling to the
//			  normal Combine combat AI while the VIP is in range, so they fight
//			  near it instead of wandering off.
//			* Caps how many grunts attach to one VIP so they spread out.
//			* Joins the VIP's squad (or creates one) so they share enemy info and
//			  attack slots, and the Ordinal's advance orders / leader-down logic
//			  apply to them automatically.
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

LINK_ENTITY_TO_CLASS(npc_combine_grunt, CNPC_CombineGrunt);

#define GRUNT_MODEL		"models/combine_grunt.mdl"	// change to your actual model path

// ---------------------------------------------------------------------------
// Convars
// ---------------------------------------------------------------------------
ConVar	grunt_protect_enabled("grunt_protect_enabled", "1", FCVAR_NONE, "1 = grunts attach to and crowd around Ordinal/Wallhammer/Suppressor.");
ConVar	grunt_search_radius("grunt_search_radius", "2500", FCVAR_NONE, "How far a grunt looks for a VIP to protect.");
ConVar	grunt_leash_radius("grunt_leash_radius", "3500", FCVAR_NONE, "If the VIP gets farther than this, the grunt drops it and looks for another.");
ConVar	grunt_max_per_vip("grunt_max_per_vip", "4", FCVAR_NONE, "Max grunts assigned to a single VIP, so they spread across VIPs.");
ConVar	grunt_ordinal_bonus("grunt_ordinal_bonus", "800", FCVAR_NONE, "Distance bonus (units) given to an Ordinal when picking a VIP. Higher = Ordinal always wins.");
ConVar	grunt_auto_squad("grunt_auto_squad", "1", FCVAR_NONE, "1 = join the VIP's squad (create one if needed) so the group shares enemy info.");
ConVar	grunt_debug("grunt_debug", "0", FCVAR_NONE, "1 = print protectee assignment info.");
ConVar grunt_investigate_leash("grunt_investigate_leash", "700", FCVAR_NONE, "A grunt guarding a VIP only investigates sounds within this distance of it. 0 = no limit.");

static const char* s_pszVipClasses[] =
{
	"npc_combine_ordinal",
	"npc_combine_wallhammer",
	"npc_combine_suppressor",
};

static bool Grunt_IsOrdinal(CBaseEntity* pEnt)
{
	return pEnt && FClassnameIs(pEnt, "npc_combine_ordinal");
}

BEGIN_DATADESC(CNPC_CombineGrunt)
DEFINE_FIELD(m_hProtectee, FIELD_EHANDLE),
DEFINE_FIELD(m_flNextProtecteeCheck, FIELD_TIME),
DEFINE_FIELD(m_flNextOrdinalCheck, FIELD_TIME),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CNPC_CombineGrunt::Spawn(void)
{
	// Set the default model if the mapper didn't specify one. Must happen before
	// BaseClass::Spawn(), which calls Precache() and SetModel().
	if (!GetModelName() || GetModelName() == NULL_STRING)
	{
		SetModelName(MAKE_STRING(GRUNT_MODEL));
	}

	// The EZ2 CNPC_Combine constructor adds SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE
	// (for npc_create). These are enemy soldiers: never player allies, never regenerating.
	//RemoveSpawnFlags(SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE);

	// Plain grunt loadout unless the mapper set spawnequipment in Hammer
	if (m_spawnEquipment == NULL_STRING)
	{
		m_spawnEquipment = MAKE_STRING("weapon_smg1");
	}

	m_hProtectee = NULL;
	m_flNextProtecteeCheck = 0.0f;
	m_flNextOrdinalCheck = 0.0f;

	// Everything else (model, health, kick, grenades) is stock combine_s
	BaseClass::Spawn();
}

//-----------------------------------------------------------------------------
// Purpose: How many other grunts are already assigned to this VIP?
//-----------------------------------------------------------------------------
int CNPC_CombineGrunt::CountProtectors(CBaseEntity* pVip)
{
	int nCount = 0;

	for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, "npc_combine_grunt");
		pEnt;
		pEnt = gEntList.FindEntityByClassname(pEnt, "npc_combine_grunt"))
	{
		if (pEnt == this || !pEnt->IsAlive())
			continue;

		CNPC_CombineGrunt* pGrunt = dynamic_cast<CNPC_CombineGrunt*>(pEnt);
		if (pGrunt && pGrunt->GetProtectee() == pVip)
		{
			nCount++;
		}
	}

	return nCount;
}

//-----------------------------------------------------------------------------
// Purpose: Pick the best living VIP in range that still has room for us.
//			Score is distance, minus a bonus for the Ordinal.
//-----------------------------------------------------------------------------
CBaseEntity* CNPC_CombineGrunt::FindBestProtectee(bool bOrdinalOnly)
{
	const float flRadius = grunt_search_radius.GetFloat();
	const int nMax = grunt_max_per_vip.GetInt();

	CBaseEntity* pBest = NULL;
	float flBestScore = FLT_MAX;

	for (int i = 0; i < ARRAYSIZE(s_pszVipClasses); i++)
	{
		const char* pszClass = s_pszVipClasses[i];

		if (bOrdinalOnly && !FStrEq(pszClass, "npc_combine_ordinal"))
			continue;

		for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, pszClass);
			pEnt;
			pEnt = gEntList.FindEntityByClassname(pEnt, pszClass))
		{
			if (!pEnt->IsAlive())
				continue;

			// Only guard things on our side
			if (IRelationType(pEnt) == D_HT)
				continue;

			float flDist = GetAbsOrigin().DistTo(pEnt->GetAbsOrigin());
			if (flDist > flRadius)
				continue;

			if (CountProtectors(pEnt) >= nMax)
				continue;

			float flScore = flDist;
			if (Grunt_IsOrdinal(pEnt))
			{
				flScore -= grunt_ordinal_bonus.GetFloat();
			}

			if (flScore < flBestScore)
			{
				flBestScore = flScore;
				pBest = pEnt;
			}
		}
	}

	return pBest;
}


//-----------------------------------------------------------------------------
// Purpose: Put us in the VIP's squad so we share enemy info and attack slots.
//			Never steals a grunt out of a squad the mapper set up.
//-----------------------------------------------------------------------------
void CNPC_CombineGrunt::JoinProtecteeSquad(CBaseEntity* pVip)
{
	if (!grunt_auto_squad.GetBool() || !pVip)
		return;

	CAI_BaseNPC* pVipNPC = pVip->MyNPCPointer();
	if (!pVipNPC)
		return;

	CAI_Squad* pVipSquad = pVipNPC->GetSquad();
	CAI_Squad* pMySquad = GetSquad();

	if (pVipSquad)
	{
		if (!pMySquad)
		{
			AddToSquad(AllocPooledString(pVipSquad->GetName()));
		}
		// Both already in different squads: leave the mapper's setup alone
		return;
	}

	if (pMySquad)
	{
		// VIP is squadless, pull it into ours
		pVipNPC->AddToSquad(AllocPooledString(pMySquad->GetName()));
		return;
	}

	// Neither has a squad: make one
	char szName[64];
	Q_snprintf(szName, sizeof(szName), "grunt_squad_%d", pVip->entindex());
	string_t iszName = AllocPooledString(szName);

	pVipNPC->AddToSquad(iszName);
	AddToSquad(iszName);
}

//-----------------------------------------------------------------------------
// Purpose: Attach to (or detach from) a VIP. The stock follow behavior does
//			the movement and crowding.
//-----------------------------------------------------------------------------
void CNPC_CombineGrunt::SetProtectee(CBaseEntity* pVip)
{
	if (m_hProtectee.Get() == pVip)
		return;

	m_hProtectee = pVip;

	if (pVip)
	{
		JoinProtecteeSquad(pVip);

		m_FollowBehavior.SetFollowTarget(pVip);
		// Tight formation = bunch up close around the VIP.
		// If AIF_TIGHT doesn't exist in your tree, use AIF_SIMPLE.
		m_FollowBehavior.SetParameters(AIF_TIGHT);

		if (grunt_debug.GetBool())
		{
			DevMsg("Grunt %s: now protecting %s (%s)\n", GetDebugName(), pVip->GetDebugName(), pVip->GetClassname());
		}
	}
	else
	{
		ClearFollowTarget();

		if (grunt_debug.GetBool())
		{
			DevMsg("Grunt %s: no VIP to protect, back to normal AI\n", GetDebugName());
		}
	}
}

bool CNPC_CombineGrunt::ShouldInvestigateSound(CSound* pSound, CBaseEntity* pOwner)
{
	CBaseEntity* pVip = m_hProtectee.Get();
	const float flLeash = grunt_investigate_leash.GetFloat();
	if (!pVip || !pVip->IsAlive() || flLeash <= 0.0f)
		return true;

	return pVip->GetAbsOrigin().DistToSqr(pSound->GetSoundOrigin()) <= flLeash * flLeash;
}

//-----------------------------------------------------------------------------
// Purpose: Throttled VIP assignment. Everything else is stock combine_s.
//-----------------------------------------------------------------------------
void CNPC_CombineGrunt::PrescheduleThink(void)
{
	BaseClass::PrescheduleThink();

	if (!IsAlive() || !grunt_protect_enabled.GetBool())
		return;

	if (gpGlobals->curtime < m_flNextProtecteeCheck)
		return;

	m_flNextProtecteeCheck = gpGlobals->curtime + 1.0f;

	CBaseEntity* pCur = m_hProtectee.Get();
	bool bValid = (pCur != NULL) && pCur->IsAlive();

	// Leash: if the VIP got too far away, give up on it
	if (bValid && GetAbsOrigin().DistTo(pCur->GetAbsOrigin()) > grunt_leash_radius.GetFloat())
	{
		bValid = false;
	}

	// Guarding a lesser VIP while an Ordinal is available: switch (checked every few seconds)
	if (bValid && !Grunt_IsOrdinal(pCur) && gpGlobals->curtime >= m_flNextOrdinalCheck)
	{
		m_flNextOrdinalCheck = gpGlobals->curtime + 5.0f;

		CBaseEntity* pOrdinal = FindBestProtectee(true);
		if (pOrdinal)
		{
			SetProtectee(pOrdinal);
			return;
		}
	}

	if (!bValid)
	{
		// Current VIP is dead / gone / too far: find a new one (may be NULL)
		SetProtectee(FindBestProtectee(false));
	}
}