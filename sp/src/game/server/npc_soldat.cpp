//=============================================================================//
// Purpose: npc_soldat - Russian Ground Forces soldier (Entropy : Zero 2).
//          See npc_soldat.h for what is and is not ported from the Lua brain.
//=============================================================================//

#include "cbase.h"
#include "ai_hull.h"
#include "ai_squad.h"
#include "ai_memory.h"
#include "ai_navigator.h"
#include "soundent.h"
#include "npcevent.h"
#include "basecombatweapon.h"
#include "npc_combine.h"
#include "npc_soldat.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS(npc_soldat, CNPC_Soldat);

#define SOLDAT_M2U					52.49f		// source units per metre
#define SOLDAT_TACTICAL_DEFAULT		0			// values from npc_combine.cpp (enum not in the header)
#define SOLDAT_TACTICAL_PRESSURE_UNTIL_CLOSE	2

// plain int/float clamps (avoids template deduction trouble with ARRAYSIZE / enums)
static inline int SoldatClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float SoldatClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Convars
// ---------------------------------------------------------------------------
ConVar npc_soldat_player_ally("npc_soldat_player_ally", "1", FCVAR_NONE, "1 = soldats can be commanded / join the player's squad (the old npc_soldat behaviour).");
ConVar npc_soldat_model("npc_soldat_model", "", FCVAR_NONE, "Force one model for every soldat without a mapper-set model. Empty = pick by camo + role.");
ConVar npc_soldat_camo("npc_soldat_camo", "standard", FCVAR_ARCHIVE, "Camo set: urban, woodland, wooden, desert, standard, random - or a raw folder name under models/taggart/ (see npc_soldat_camo_list). Saved to config. Applies to soldats spawned after the change.");
ConVar npc_soldat_random_roles("npc_soldat_random_roles", "1", FCVAR_ARCHIVE, "1 = a soldat whose map did not set a role (soldatrole -1) gets a random one each spawn; 0 = rifleman.");
ConVar npc_soldat_voice("npc_soldat_voice", "1", FCVAR_NONE, "1 = Russian voice lines (needs game_sounds_vac_ru.txt in the manifest).");
ConVar npc_soldat_voice_gap("npc_soldat_voice_gap", "2.5", FCVAR_NONE, "Minimum seconds between non-critical voice lines per man.");
ConVar npc_soldat_morale("npc_soldat_morale", "1", FCVAR_NONE, "1 = morale / panic enabled.");
ConVar npc_soldat_engage_first("npc_soldat_engage_first", "1", FCVAR_NONE, "1 = a contact in the open is shot at before any thought of cover.");
ConVar npc_soldat_at_use_rpg("npc_soldat_at_use_rpg", "0", FCVAR_NONE, "1 = AT gunners switch to weapon_rpg against striders/gunships (needs RPG animations on the model).");
ConVar npc_soldat_orders("npc_soldat_orders", "1", FCVAR_NONE, "1 = leaders order squadmates to advance on a hiding enemy.");
ConVar npc_soldat_order_hide_time("npc_soldat_order_hide_time", "4", FCVAR_NONE, "Seconds the enemy must be out of sight before the leader orders an advance.");
ConVar npc_soldat_order_cooldown("npc_soldat_order_cooldown", "15", FCVAR_NONE, "Minimum seconds between advance orders.");
ConVar npc_soldat_order_max_units("npc_soldat_order_max_units", "2", FCVAR_NONE, "Max squadmates ordered per advance order.");
ConVar npc_soldat_leader_down("npc_soldat_leader_down", "1", FCVAR_NONE, "1 = squad becomes disorganised when the leader dies.");
ConVar npc_soldat_leader_down_proficiency("npc_soldat_leader_down_proficiency", "0", FCVAR_NONE, "Squadmate accuracy after the leader dies (0 = poor).");
ConVar npc_soldat_leader_down_stun("npc_soldat_leader_down_stun", "2.5", FCVAR_NONE, "Seconds squadmates can't fire after the leader dies.");
ConVar npc_soldat_debug("npc_soldat_debug", "0", FCVAR_NONE, "1 = print soldat decisions.");

// ---------------------------------------------------------------------------
// Roles. Weapons are ONLY from the guns list. Lua RU kit -> closest entry:
//   AK-12 -> weapon_css_ak47      PKM -> weapon_css_m249     SVD -> weapon_css_g3sg1
//   AKS-74U -> weapon_smg2        PM  -> weapon_pistol       AK-50 -> weapon_css_awp
// ---------------------------------------------------------------------------
struct SoldatRoleDef_t
{
	const char* pszName;
	const char* pszWeapon;
	const char* pszSidearm;
	int			iFrags;
	int			iBaseProf;		// 0 poor .. 4 perfect
};

static const SoldatRoleDef_t s_Roles[SOLDAT_ROLE_COUNT] =
{
	{ "rifleman",	"weapon_css_ak47",	NULL,			2, 2 },
	{ "leader",		"weapon_css_ak47",	"weapon_pistol",1, 2 },
	{ "autorifle",	"weapon_css_m249",	"weapon_pistol",1, 1 },
	{ "grenadier",	"weapon_css_ak47",	NULL,			3, 2 },
	{ "medic",		"weapon_css_ak47",	NULL,			1, 1 },
	{ "marksman",	"weapon_css_g3sg1",	NULL,			1, 3 },
	{ "rto",		"weapon_css_ak47",	NULL,			1, 2 },
	{ "at",			"weapon_smg2",		"weapon_pistol",1, 2 },
	{ "sniper",		"weapon_css_scout",	"weapon_pistol",0, 4 },
	{ "hsniper",	"weapon_css_awp",	"weapon_pistol",0, 4 },
};

// Spawn weights for random roles (same order as SoldatRole_t)
static const int s_RoleWeights[SOLDAT_ROLE_COUNT] =
{
	40,	// rifleman
	6,	// leader
	9,	// autorifle
	10,	// grenadier
	8,	// medic
	7,	// marksman
	6,	// rto
	7,	// at
	4,	// sniper
	2,	// hsniper
};

static int SoldatRandomRole()
{
	int total = 0;
	for (int i = 0; i < SOLDAT_ROLE_COUNT; i++)
		total += s_RoleWeights[i];
	int r = RandomInt(0, total - 1);
	for (int i = 0; i < SOLDAT_ROLE_COUNT; i++)
	{
		if (r < s_RoleWeights[i])
			return i;
		r -= s_RoleWeights[i];
	}
	return SOLDAT_ROLE_RIFLEMAN;
}

// Camo sets (models/taggart/<folder>/). The aliases are my reading of the folder names -
// swap them if a set looks wrong in game.
struct SoldatCamo_t { const char* pszAlias; const char* pszFolder; };
static const SoldatCamo_t s_Camos[] =
{
	{ "urban",		"urbnsobr_troopers" },
	{ "woodland",	"woodsobr_troopers" },
	{ "wooden",		"woodomoh_troopers" },
	{ "desert",		"russiand_troopers" },
	{ "standard",	"russians_troopers" },
};

// Model file by role (same file names in every camo folder)
static const char* s_pszRoleModelFile[SOLDAT_ROLE_COUNT] =
{
	"visor_soldier_visor_up",		// rifleman
	"commander",					// leader
	"helmet_goggles_soldier",		// autorifle
	"goggles_soldier",				// grenadier
	"beret_soldier",				// medic
	"boonie_soldier",				// marksman
	"visor_soldier",				// rto
	"visor_soldier_visor_up",		// at
	"gear_soldier",				// sniper
	"boonie_soldier",				// hsniper
};

// "urban" / "random" / a raw folder name -> a folder
static const char* SoldatCamoFolder()
{
	const char* psz = npc_soldat_camo.GetString();
	if (!Q_stricmp(psz, "random"))
		return s_Camos[RandomInt(0, ARRAYSIZE(s_Camos) - 1)].pszFolder;
	for (int i = 0; i < ARRAYSIZE(s_Camos); i++)
	{
		if (!Q_stricmp(psz, s_Camos[i].pszAlias))
			return s_Camos[i].pszFolder;
	}
	return psz[0] ? psz : s_Camos[0].pszFolder;	// raw folder name
}

CON_COMMAND(npc_soldat_camo_list, "Lists the soldat camo sets")
{
	for (int i = 0; i < ARRAYSIZE(s_Camos); i++)
		Msg("  %-10s -> models/taggart/%s/\n", s_Camos[i].pszAlias, s_Camos[i].pszFolder);
	Msg("  random     -> a random set per soldat\n");
	Msg("current: %s\n", npc_soldat_camo.GetString());
}

// the six RU voices in sound/vac_alife/ru/
static const char* s_pszVoices[] = { "alexei", "dmitri", "ivan", "nikolai", "pavel", "sergei" };

// categories that exist for all six voices (hq_* except hq_nosupport are alexei-only: not used)
static const char* s_pszVoiceCats[] =
{
	"ack", "aid", "alone_contact", "alone_fight", "alone_mandown", "alone_reload", "attack", "blast_near",
	"clear", "contact", "contact_above", "contact_air", "contact_behind", "contact_close", "contact_creature",
	"contact_front", "contact_giant", "contact_left", "contact_many", "contact_right", "contact_unknown",
	"contact_vehicle", "cover", "creature_dead", "down", "flanked", "frag_out", "freaked", "glimpse", "grenade",
	"heard", "hold", "hurt", "kill", "low_ammo", "mandown", "medic", "moving", "panic", "point", "radio_contact",
	"reload", "shoot_it", "startle", "suppressed", "talk", "under_fire", "where_is", "withdraw",
};

BEGIN_DATADESC(CNPC_Soldat)
DEFINE_KEYFIELD(m_iRole, FIELD_INTEGER, "soldatrole"),
DEFINE_KEYFIELD(m_iVoice, FIELD_INTEGER, "soldatvoice"),
DEFINE_FIELD(m_flSkill, FIELD_FLOAT),
DEFINE_FIELD(m_flNerve, FIELD_FLOAT),
DEFINE_FIELD(m_flFear, FIELD_FLOAT),
DEFINE_FIELD(m_flDiscipline, FIELD_FLOAT),
DEFINE_FIELD(m_iszPrimary, FIELD_STRING),
DEFINE_FIELD(m_flMorale, FIELD_FLOAT),
DEFINE_FIELD(m_flSuppr, FIELD_FLOAT),
DEFINE_FIELD(m_flLastThinkT, FIELD_TIME),
DEFINE_FIELD(m_flLastSupprHeardT, FIELD_TIME),
DEFINE_FIELD(m_bPanicking, FIELD_BOOLEAN),
DEFINE_FIELD(m_bPanicFlee, FIELD_BOOLEAN),
DEFINE_FIELD(m_bPanicSpoken, FIELD_BOOLEAN),
DEFINE_FIELD(m_iLastProficiency, FIELD_INTEGER),
DEFINE_FIELD(m_flEngageFirstUntil, FIELD_TIME),
DEFINE_FIELD(m_flStartleUntil, FIELD_TIME),
DEFINE_FIELD(m_flNextOrderT, FIELD_TIME),
DEFINE_FIELD(m_flNextWeaponCheckT, FIELD_TIME),
DEFINE_FIELD(m_flPistolNearT, FIELD_TIME),
DEFINE_FIELD(m_flNextVoiceT, FIELD_TIME),
DEFINE_FIELD(m_flNextRadioT, FIELD_TIME),
DEFINE_FIELD(m_flNextIdleTalkT, FIELD_TIME),
DEFINE_FIELD(m_bKillSpoken, FIELD_BOOLEAN),
END_DATADESC()

//-----------------------------------------------------------------------------
CNPC_Soldat::CNPC_Soldat()
{
	m_iRole = -1;		// -1 = not set by the map: random in Spawn
	m_iVoice = -1;
	m_flSkill = 1.0f;
	m_flNerve = 1.0f;
	m_flFear = 0.4f;
	m_flDiscipline = 0.5f;
	m_iszPrimary = NULL_STRING;
	m_flMorale = 1.0f;
	m_flSuppr = 0.0f;
	m_flLastThinkT = 0.0f;
	m_flLastSupprHeardT = 0.0f;
	m_bPanicking = m_bPanicFlee = m_bPanicSpoken = false;
	m_iLastProficiency = -1;
	m_flEngageFirstUntil = m_flStartleUntil = m_flNextOrderT = m_flNextWeaponCheckT = m_flPistolNearT = 0.0f;
	m_flNextVoiceT = m_flNextRadioT = m_flNextIdleTalkT = 0.0f;
	m_bKillSpoken = false;
	m_bPrecached = false;

	// What the Combine constructor used to do for everyone
	if (npc_soldat_player_ally.GetBool())
		AddSpawnFlags(SF_COMBINE_COMMANDABLE | SF_COMBINE_REGENERATE);
}

//-----------------------------------------------------------------------------
// Player ally when commandable (npc_soldat_player_ally 1), else the stock Combine class.
// This is CNPC_Soldat's own: do NOT define CNPC_Combine::Classify here, npc_combine.cpp already does.
//-----------------------------------------------------------------------------
Class_T CNPC_Soldat::Classify(void)
{
	if (npc_soldat_player_ally.GetBool())
		return CLASS_PLAYER_ALLY;
	return BaseClass::Classify();
}

bool CNPC_Soldat::CanBePlayerSquadmate()
{
	return npc_soldat_player_ally.GetBool();
}

//-----------------------------------------------------------------------------
// Spawn: who he is, then the base spawn (which precaches and equips)
//-----------------------------------------------------------------------------
void CNPC_Soldat::Spawn()
{
	// role: the map's if it set one, else random (rifleman if npc_soldat_random_roles is 0)
	if (m_iRole < 0)
		m_iRole = npc_soldat_random_roles.GetBool() ? SoldatRandomRole() : SOLDAT_ROLE_RIFLEMAN;
	m_iRole = SoldatClampI(m_iRole, 0, SOLDAT_ROLE_COUNT - 1);
	if (m_iVoice < 0 || m_iVoice >= ARRAYSIZE(s_pszVoices))
		m_iVoice = entindex() % ARRAYSIZE(s_pszVoices);

	// persona (sv_persona): a trained shooter ~1, a little spread either way
	m_flSkill = RandomFloat(0.8f, 1.25f);
	m_flNerve = RandomFloat(0.8f, 1.2f);
	m_flFear = RandomFloat(0.2f, 0.7f);
	m_flDiscipline = RandomFloat(0.2f, 0.8f);
	m_flMorale = 1.0f;

	const SoldatRoleDef_t& R = s_Roles[m_iRole];

	if (GetModelName() == NULL_STRING)
	{
		if (npc_soldat_model.GetString()[0])
			SetModelName(AllocPooledString(npc_soldat_model.GetString()));
		else
			SetModelName(AllocPooledString(CFmtStr("models/taggart/%s/%s.mdl", SoldatCamoFolder(), s_pszRoleModelFile[m_iRole])));
	}

	if (m_spawnEquipment == NULL_STRING)
		m_spawnEquipment = AllocPooledString(R.pszWeapon);

	m_iNumGrenades = R.iFrags;

	BaseClass::Spawn();

	if (m_iRole == SOLDAT_ROLE_MEDIC)
	{
		inputdata_t dummy;
		InputSetMedicOn(dummy);
	}
}

void CNPC_Soldat::Precache()
{
	// Precache can run before Spawn (level load): the role is not chosen yet, so cover every role
	if (m_iRole < 0)
	{
		for (int r = 0; r < SOLDAT_ROLE_COUNT; r++)
		{
			UTIL_PrecacheOther(s_Roles[r].pszWeapon);
			if (s_Roles[r].pszSidearm)
				UTIL_PrecacheOther(s_Roles[r].pszSidearm);
		}
		for (int c = 0; c < ARRAYSIZE(s_Camos); c++)
		{
			// every camo if random, else just the current one
			if (Q_stricmp(npc_soldat_camo.GetString(), "random") && Q_stricmp(s_Camos[c].pszFolder, SoldatCamoFolder()))
				continue;
			for (int r = 0; r < SOLDAT_ROLE_COUNT; r++)
				PrecacheModel(CFmtStr("models/taggart/%s/%s.mdl", s_Camos[c].pszFolder, s_pszRoleModelFile[r]));
		}
		UTIL_PrecacheOther("weapon_rpg");
		UTIL_PrecacheOther("weapon_frag");
		for (int i = 0; i < ARRAYSIZE(s_pszVoices); i++)
			for (int j = 0; j < ARRAYSIZE(s_pszVoiceCats); j++)
				PrecacheScriptSound(CFmtStr("VAC_RU.%s.%s", s_pszVoices[i], s_pszVoiceCats[j]));
		PrecacheScriptSound("VAC_RU.radio_blip");
		BaseClass::Precache();
		return;
	}

	const SoldatRoleDef_t& R = s_Roles[SoldatClampI(m_iRole, 0, SOLDAT_ROLE_COUNT - 1)];

	if (GetModelName() != NULL_STRING)
		PrecacheModel(STRING(GetModelName()));
	UTIL_PrecacheOther(R.pszWeapon);
	if (R.pszSidearm)
		UTIL_PrecacheOther(R.pszSidearm);
	if (m_iRole == SOLDAT_ROLE_AT && npc_soldat_at_use_rpg.GetBool())
		UTIL_PrecacheOther("weapon_rpg");
	UTIL_PrecacheOther("weapon_frag");

	int iVoice = (m_iVoice >= 0 && m_iVoice < ARRAYSIZE(s_pszVoices)) ? m_iVoice : 0;
	for (int i = 0; i < ARRAYSIZE(s_pszVoiceCats); i++)
		PrecacheScriptSound(CFmtStr("VAC_RU.%s.%s", s_pszVoices[iVoice], s_pszVoiceCats[i]));
	PrecacheScriptSound("VAC_RU.radio_blip");

	BaseClass::Precache();
}

void CNPC_Soldat::PostNPCInit()
{
	BaseClass::PostNPCInit();

	const SoldatRoleDef_t& R = s_Roles[m_iRole];

	if (!GetActiveWeapon())
		GiveWeapon(AllocPooledString(R.pszWeapon));

	if (GetActiveWeapon())
		m_iszPrimary = AllocPooledString(GetActiveWeapon()->GetClassname());

	if (R.pszSidearm && !Weapon_OwnsThisType(R.pszSidearm))
		GiveWeaponHolstered(AllocPooledString(R.pszSidearm));

	if (m_iRole == SOLDAT_ROLE_AT && npc_soldat_at_use_rpg.GetBool() && !Weapon_OwnsThisType("weapon_rpg"))
		GiveWeaponHolstered(AllocPooledString("weapon_rpg"));

	// snipers see further than a rifleman
	if (m_iRole == SOLDAT_ROLE_SNIPER || m_iRole == SOLDAT_ROLE_HSNIPER)
		SetDistLook(6000.0f);
	else if (m_iRole == SOLDAT_ROLE_MARKSMAN)
		SetDistLook(4000.0f);

	m_flLastThinkT = gpGlobals->curtime;
}

//-----------------------------------------------------------------------------
// Think
//-----------------------------------------------------------------------------
void CNPC_Soldat::PrescheduleThink()
{
	BaseClass::PrescheduleThink();

	if (!IsAlive())
		return;

	float now = gpGlobals->curtime;
	float dt = SoldatClampF(now - m_flLastThinkT, 0.0f, 1.0f);
	m_flLastThinkT = now;

	UpdateFeelings(dt);
	UpdateProficiency();
	UpdateWeaponSwap();
	UpdateLeaderOrders();
	UpdateVoice();
}

void CNPC_Soldat::AddSuppression(float amount)
{
	m_flSuppr = MIN(1.0f, m_flSuppr + amount * 0.45f / m_flNerve);
}

bool CNPC_Soldat::SquadHasAT()
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return m_iRole == SOLDAT_ROLE_AT;

	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (pS && pS->IsAlive() && pS->GetSoldatRole() == SOLDAT_ROLE_AT)
			return true;
	}
	return false;
}

bool CNPC_Soldat::IsHeavyEnemy(CBaseEntity* pEnemy)
{
	return pEnemy && (FClassnameIs(pEnemy, "npc_strider") || FClassnameIs(pEnemy, "npc_combinegunship") ||
		FClassnameIs(pEnemy, "npc_helicopter") || FClassnameIs(pEnemy, "npc_combinedropship"));
}

// suppression and morale (sv_brain B.Perceive / B.Suppress)
void CNPC_Soldat::UpdateFeelings(float dt)
{
	m_flSuppr = MAX(0.0f, m_flSuppr - 0.35f * dt);

	// head down while rounds are cracking about
	if (m_flSuppr > 0.5f && !IsMoving() && GetState() == NPC_STATE_COMBAT)
		DesireCrouch();

	if (!npc_soldat_morale.GetBool())
		return;

	bool bAlone = !GetSquad() || GetSquad()->NumMembers() <= 1;
	CBaseEntity* pEnemy = GetEnemy();
	bool bArmourNear = IsHeavyEnemy(pEnemy) && (pEnemy->GetAbsOrigin() - GetAbsOrigin()).Length() < 250.0f * SOLDAT_M2U;

	// back slowly when it is quiet and his squad is with him
	if (m_flSuppr < 0.2f && !bArmourNear)
		m_flMorale = MIN(1.0f, m_flMorale + 0.03f * dt * (bAlone ? 0.4f : 1.0f));

	// a tank and nothing to stop it; fire he cannot answer
	if (bArmourNear && !SquadHasAT())
		m_flMorale = MAX(0.0f, m_flMorale - 0.02f * dt / m_flNerve);
	if (m_flSuppr > 0.7f)
		m_flMorale = MAX(0.0f, m_flMorale - 0.03f * dt / m_flNerve);

	if (m_flMorale < 0.12f)
	{
		if (!m_bPanicking && GetState() == NPC_STATE_COMBAT)
		{
			m_bPanicking = true;
			m_bPanicFlee = RandomFloat(0, 1) < 0.6f;
			m_bPanicSpoken = false;
		}
	}
	else if (m_bPanicking && m_flMorale > 0.35f)
	{
		m_bPanicking = false;
	}
}

// aim by his skill and how hard he is pinned (sv_brain B.Fire's error terms)
void CNPC_Soldat::UpdateProficiency()
{
	float score = (float)s_Roles[m_iRole].iBaseProf - (m_flSkill - 1.0f) * 2.5f - m_flSuppr * 2.0f;
	int prof = SoldatClampI((int)floor(score + 0.5f), 0, 4);
	if (prof == m_iLastProficiency)
		return;
	m_iLastProficiency = prof;
	SetCurrentWeaponProficiency((WeaponProficiency_t)prof);
}

WeaponProficiency_t CNPC_Soldat::CalcWeaponProficiency(CBaseCombatWeapon* pWeapon)
{
	float score = (float)s_Roles[SoldatClampI(m_iRole, 0, SOLDAT_ROLE_COUNT - 1)].iBaseProf - (m_flSkill - 1.0f) * 2.5f - m_flSuppr * 2.0f;
	return (WeaponProficiency_t)SoldatClampI((int)floor(score + 0.5f), 0, 4);
}

//-----------------------------------------------------------------------------
// Sidearm when the primary is dry with the enemy close (sv_brain B.SIDEARM);
// AT gunners to the RPG against big things (optional)
//-----------------------------------------------------------------------------
void CNPC_Soldat::UpdateWeaponSwap()
{
	float now = gpGlobals->curtime;
	if (now < m_flNextWeaponCheckT || m_iszPrimary == NULL_STRING)
		return;
	m_flNextWeaponCheckT = now + 0.5f;

	CBaseCombatWeapon* pActive = GetActiveWeapon();
	if (!pActive || IsInAScript())
		return;

	CBaseEntity* pEnemy = GetEnemy();
	float flDist = pEnemy ? (pEnemy->GetAbsOrigin() - GetAbsOrigin()).Length() / SOLDAT_M2U : 9999.0f;

	CBaseCombatWeapon* pPrimary = Weapon_OwnsThisType(STRING(m_iszPrimary));
	const char* pszSide = s_Roles[m_iRole].pszSidearm;
	CBaseCombatWeapon* pPistol = pszSide ? Weapon_OwnsThisType(pszSide) : NULL;
	CBaseCombatWeapon* pRPG = (m_iRole == SOLDAT_ROLE_AT && npc_soldat_at_use_rpg.GetBool()) ? Weapon_OwnsThisType("weapon_rpg") : NULL;

	bool bOnPistol = pPistol && pActive == pPistol;
	bool bOnRPG = pRPG && pActive == pRPG;

	// the RPG for big targets
	if (pRPG)
	{
		if (!bOnRPG && IsHeavyEnemy(pEnemy))
		{
			Weapon_Switch(pRPG);
			return;
		}
		if (bOnRPG && !IsHeavyEnemy(pEnemy) && pPrimary)
		{
			Weapon_Switch(pPrimary);
			return;
		}
	}

	if (!pPistol || !pPrimary)
		return;

	// rifle dry, enemy within 15 m: pistol
	if (!bOnPistol && !bOnRPG && pActive->UsesClipsForAmmo1() && pActive->Clip1() <= 0 && flDist < 15.0f)
	{
		Weapon_Switch(pPistol);
		m_flPistolNearT = now;
		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s: pistol out\n", GetDebugName());
		return;
	}

	// back to the rifle: nobody within 22 m for 4 s
	if (bOnPistol)
	{
		if (flDist < 22.0f)
			m_flPistolNearT = now;
		else if (now - m_flPistolNearT > 4.0f)
			Weapon_Switch(pPrimary);
	}
}

//-----------------------------------------------------------------------------
// The squad leader: orders an advance on a hiding enemy (ordinal pattern),
// and his death breaks the squad
//-----------------------------------------------------------------------------
void CNPC_Soldat::UpdateLeaderOrders()
{
	if (m_iRole != SOLDAT_ROLE_LEADER || !npc_soldat_orders.GetBool())
		return;
	if (GetState() != NPC_STATE_COMBAT)
		return;

	CBaseEntity* pEnemy = GetEnemy();
	if (!pEnemy || !pEnemy->IsAlive())
		return;

	float now = gpGlobals->curtime;
	if (now < m_flNextOrderT || HasCondition(COND_SEE_ENEMY))
		return;
	if (now - GetEnemyLastTimeSeen() < npc_soldat_order_hide_time.GetFloat())
		return;

	if (IssueAdvanceOrder())
	{
		m_flNextOrderT = now + npc_soldat_order_cooldown.GetFloat();
		PlayVoice("attack", true);
	}
	else
	{
		m_flNextOrderT = now + 3.0f;
	}
}

bool CNPC_Soldat::IssueAdvanceOrder()
{
	CAI_Squad* pSquad = GetSquad();
	CBaseEntity* pEnemy = GetEnemy();
	if (!pSquad || !pEnemy)
		return false;

	const int nMax = npc_soldat_order_max_units.GetInt();
	int nOrdered = 0;

	AISquadIter_t iter;
	for (CAI_BaseNPC* pMember = pSquad->GetFirstMember(&iter); pMember && nOrdered < nMax; pMember = pSquad->GetNextMember(&iter))
	{
		if (pMember == this || !pMember->IsAlive())
			continue;

		CNPC_Combine* pCombine = dynamic_cast<CNPC_Combine*>(pMember);
		if (!pCombine)
			continue;

		// snipers and the machine gun hold their ground
		CNPC_Soldat* pSoldat = dynamic_cast<CNPC_Soldat*>(pMember);
		if (pSoldat && (pSoldat->GetSoldatRole() == SOLDAT_ROLE_SNIPER || pSoldat->GetSoldatRole() == SOLDAT_ROLE_HSNIPER ||
			pSoldat->GetSoldatRole() == SOLDAT_ROLE_AUTORIFLE || pSoldat->GetSoldatRole() == SOLDAT_ROLE_MARKSMAN))
			continue;

		if (pCombine->m_iTacticalVariant != SOLDAT_TACTICAL_DEFAULT)
			continue;

		pCombine->UpdateEnemyMemory(pEnemy, GetEnemyLKP(), this);
		pCombine->m_iTacticalVariant = SOLDAT_TACTICAL_PRESSURE_UNTIL_CLOSE;
		pCombine->ClearSchedule("Soldat leader ordered advance");
		nOrdered++;
	}

	if (npc_soldat_debug.GetBool())
		DevMsg("npc_soldat %s: advance order to %d\n", GetDebugName(), nOrdered);

	return nOrdered > 0;
}

void CNPC_Soldat::OnLeaderLost(WeaponProficiency_t prof, float flStun)
{
	m_iTacticalVariant = SOLDAT_TACTICAL_DEFAULT;
	SetCurrentWeaponProficiency(prof);
	m_iLastProficiency = (int)prof;
	m_flNextAttack = gpGlobals->curtime + flStun;
	m_flMorale = MAX(0.0f, m_flMorale - 0.2f / m_flNerve);
	ClearSchedule("Squad leader down");
}

void CNPC_Soldat::HandleLeaderDown()
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return;

	AISquadIter_t iter;
	// another leader takes command
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (pS && pS != this && pS->IsAlive() && pS->IsLeaderRole())
			return;
	}

	WeaponProficiency_t prof = (WeaponProficiency_t)SoldatClampI(npc_soldat_leader_down_proficiency.GetInt(), (int)WEAPON_PROFICIENCY_POOR, (int)WEAPON_PROFICIENCY_PERFECT);
	CNPC_Soldat* pReporter = NULL;

	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		if (p == this || !p->IsAlive())
			continue;

		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (pS)
		{
			pS->OnLeaderLost(prof, npc_soldat_leader_down_stun.GetFloat());
			if (!pReporter)
				pReporter = pS;
		}
		else if (CNPC_Combine* pC = dynamic_cast<CNPC_Combine*>(p))
		{
			pC->m_iTacticalVariant = SOLDAT_TACTICAL_DEFAULT;
			pC->SetCurrentWeaponProficiency(prof);
			pC->m_flNextAttack = gpGlobals->curtime + npc_soldat_leader_down_stun.GetFloat();
			pC->ClearSchedule("Squad leader down");
		}
	}

	if (pReporter)
	{
		pReporter->EmitSound("VAC_RU.radio_blip");
		pReporter->PlayVoice("radio_contact", true);
	}
}

void CNPC_Soldat::Event_Killed(const CTakeDamageInfo& info)
{
	if (m_iRole == SOLDAT_ROLE_LEADER && npc_soldat_leader_down.GetBool())
		HandleLeaderDown();

	BaseClass::Event_Killed(info);
}

int CNPC_Soldat::OnTakeDamage_Alive(const CTakeDamageInfo& info)
{
	AddSuppression(0.5f);
	m_flMorale = MAX(0.0f, m_flMorale - 0.12f / m_flNerve);
	return BaseClass::OnTakeDamage_Alive(info);
}

//-----------------------------------------------------------------------------
// Hearing: the startle at ease (B.Startle) and rounds cracking close (B.Suppress)
//-----------------------------------------------------------------------------
void CNPC_Soldat::OnListened()
{
	BaseClass::OnListened();

	float now = gpGlobals->curtime;

	// rounds landing near him
	if (HasCondition(COND_HEAR_BULLET_IMPACT) && now - m_flLastSupprHeardT > 0.3f)
	{
		CSound* pImpact = GetBestSound(SOUND_BULLET_IMPACT);
		if (pImpact && (pImpact->GetSoundOrigin() - GetAbsOrigin()).Length() < 5.0f * SOLDAT_M2U)
		{
			m_flLastSupprHeardT = now;
			AddSuppression(0.15f);
			if (m_flSuppr > 0.6f)
				PlayVoice("suppressed");
			else if (m_flSuppr > 0.2f && !HasCondition(COND_SEE_ENEMY))
				PlayVoice("under_fire");
		}
	}

	if (HasCondition(COND_HEAR_DANGER))
		PlayVoice("grenade", true, 3.0f);

	// caught at ease by gunfire: a beat to take it in, no firing meanwhile
	if ((m_NPCState == NPC_STATE_IDLE || m_NPCState == NPC_STATE_ALERT) && now > m_flStartleUntil &&
		(HasCondition(COND_HEAR_COMBAT) || HasCondition(COND_HEAR_BULLET_IMPACT)))
	{
		CSound* pSound = GetBestSound(SOUND_COMBAT | SOUND_BULLET_IMPACT);
		if (pSound)
		{
			Vector vecAt = pSound->GetSoundReactOrigin();
			float flDist = (vecAt - GetAbsOrigin()).Length() / SOLDAT_M2U;
			bool bClose = flDist < 60.0f;

			// 0.9 s for the undisciplined down to 0.3 s for an old hand
			float flJudge = Lerp(m_flDiscipline, 0.9f, 0.3f) * (bClose ? 0.6f : 1.0f) * (0.8f + 0.5f * m_flFear);
			m_flStartleUntil = now + flJudge + 3.0f;
			m_flNextAttack = MAX(m_flNextAttack, now + flJudge);

			AddLookTarget(vecAt + Vector(0, 0, 40), 0.8f, 1.5f, 0.1f);
			PlayVoice(bClose ? "startle" : "heard", true, 4.0f);

			if (npc_soldat_debug.GetBool())
				DevMsg("npc_soldat %s: startle %.0f m, judging %.2f s\n", GetDebugName(), flDist, flJudge);
		}
	}
}

//-----------------------------------------------------------------------------
// Combat: a close contact in the open is shot at first (B.ENGAGE_FIRST)
//-----------------------------------------------------------------------------
int CNPC_Soldat::SelectCombatSchedule()
{
	CBaseEntity* pEnemy = GetEnemy();
	float now = gpGlobals->curtime;

	if (npc_soldat_engage_first.GetBool() && pEnemy)
	{
		if (HasCondition(COND_NEW_ENEMY) && HasCondition(COND_SEE_ENEMY) && m_flSuppr < 0.3f && !IsHeavyEnemy(pEnemy))
		{
			float flDist = (pEnemy->GetAbsOrigin() - GetAbsOrigin()).Length() / SOLDAT_M2U;
			if (flDist < 40.0f)
				m_flEngageFirstUntil = now + RandomFloat(1.5f, 3.0f);
			else if (flDist < 200.0f)
				m_flEngageFirstUntil = now + RandomFloat(2.0f, 3.5f);
		}

		if (now < m_flEngageFirstUntil && m_flSuppr < 0.3f && HasCondition(COND_SEE_ENEMY) && HasCondition(COND_CAN_RANGE_ATTACK1) &&
			!HasCondition(COND_NO_PRIMARY_AMMO) && OccupyStrategySlotRange(SQUAD_SLOT_ATTACK1, SQUAD_SLOT_ATTACK2))
		{
			return SCHED_RANGE_ATTACK1;
		}
	}

	return BaseClass::SelectCombatSchedule();
}

// His nerve gone: run for the rear or cower (B.Panic)
int CNPC_Soldat::SelectSchedule()
{
	if (m_bPanicking && GetState() == NPC_STATE_COMBAT && GetEnemy() && !IsInAScript())
	{
		if (!m_bPanicSpoken)
		{
			m_bPanicSpoken = true;
			PlayVoice("panic", true);
		}
		return m_bPanicFlee ? SCHED_RUN_FROM_ENEMY : SCHED_COWER;
	}

	return BaseClass::SelectSchedule();
}

// Say it as he starts it
void CNPC_Soldat::OnStartSchedule(int scheduleType)
{
	BaseClass::OnStartSchedule(scheduleType);

	bool bAlone = !GetSquad() || GetSquad()->NumMembers() <= 1;

	if (scheduleType == SCHED_COMBINE_RANGE_ATTACK2 || scheduleType == SCHED_COMBINE_GRENADE_AND_RELOAD || scheduleType == SCHED_COMBINE_TOSS_GRENADE_COVER1)
		PlayVoice("frag_out", true);
	else if (scheduleType == SCHED_RELOAD || scheduleType == SCHED_COMBINE_HIDE_AND_RELOAD)
		PlayVoice(bAlone ? "alone_reload" : "reload");
	else if (scheduleType == SCHED_COMBINE_TAKE_COVER1)
	{
		if (RandomFloat(0, 1) < 0.35f)
			PlayVoice("cover");
	}
	else if (scheduleType == SCHED_COMBINE_PRESS_ATTACK || scheduleType == SCHED_COMBINE_ASSAULT)
	{
		if (RandomFloat(0, 1) < 0.4f)
			PlayVoice("moving");
	}
	else if (scheduleType == SCHED_COMBINE_ORDER_SURRENDER)
		PlayVoice("hold", true);
	else if (scheduleType == SCHED_COMBINE_BURNING_STAND)
		PlayVoice("hurt", true);
}

//-----------------------------------------------------------------------------
// Voice
//-----------------------------------------------------------------------------
bool CNPC_Soldat::PlayVoice(const char* pszCategory, bool bForce, float flGap)
{
	if (!npc_soldat_voice.GetBool() || !pszCategory)
		return false;
	if (GetFlags() & FL_DISSOLVING)
		return false;

	float now = gpGlobals->curtime;
	if (!bForce && now < m_flNextVoiceT)
		return false;

	int iVoice = (m_iVoice >= 0 && m_iVoice < ARRAYSIZE(s_pszVoices)) ? m_iVoice : 0;
	EmitSound(CFmtStr("VAC_RU.%s.%s", s_pszVoices[iVoice], pszCategory));

	m_flNextVoiceT = now + (flGap >= 0.0f ? flGap : npc_soldat_voice_gap.GetFloat());
	return true;
}

// the contact call by where and what (V.ContactEvent)
const char* CNPC_Soldat::ContactCategory(CBaseEntity* pEnemy)
{
	if (!pEnemy)
		return "contact";

	if (IsHeavyEnemy(pEnemy))
		return (FClassnameIs(pEnemy, "npc_strider")) ? "contact_giant" : "contact_air";

	if (FClassnameIs(pEnemy, "npc_hunter"))
		return "contact_giant";

	switch (pEnemy->Classify())
	{
	case CLASS_ZOMBIE:
	case CLASS_HEADCRAB:
	case CLASS_ANTLION:
	case CLASS_BARNACLE:
		return "contact_creature";
	}

	if (GetEnemies() && GetEnemies()->NumEnemies() >= 3)
		return "contact_many";

	Vector vecTo = pEnemy->WorldSpaceCenter() - GetAbsOrigin();
	float flDist = vecTo.Length() / SOLDAT_M2U;
	if (vecTo.z > 150.0f)
		return "contact_above";
	if (flDist < 10.0f)
		return "contact_close";

	vecTo.z = 0;
	VectorNormalize(vecTo);
	Vector vecFwd, vecRight;
	GetVectors(&vecFwd, &vecRight, NULL);
	if (vecTo.Dot(vecFwd) < -0.3f)
		return "contact_behind";
	float flR = vecTo.Dot(vecRight);
	if (flR < -0.5f)
		return "contact_left";
	if (flR > 0.5f)
		return "contact_right";
	return "contact_front";
}

void CNPC_Soldat::FoundEnemySound(CBaseEntity* pEnemy)
{
	if (!npc_soldat_voice.GetBool())
		return;

	float now = gpGlobals->curtime;
	bool bAlone = !GetSquad() || GetSquad()->NumMembers() <= 1;
	const char* pszCat = ContactCategory(pEnemy);

	// something huge or unknown shakes them (B.Perceive, "the less nerve, the more")
	if (!Q_stricmp(pszCat, "contact_giant") || !Q_stricmp(pszCat, "contact_creature"))
		m_flMorale = MAX(0.15f, m_flMorale - (!Q_stricmp(pszCat, "contact_giant") ? 0.14f : 0.07f) * (0.4f + m_flFear));

	if (bAlone && !Q_stricmp(pszCat, "contact"))
		pszCat = "alone_contact";

	// leader and radioman report up the chain, at most once in 90 s
	if ((m_iRole == SOLDAT_ROLE_LEADER || m_iRole == SOLDAT_ROLE_RTO) && now > m_flNextRadioT)
	{
		m_flNextRadioT = now + 90.0f;
		EmitSound("VAC_RU.radio_blip");
		PlayVoice("radio_contact", true, 3.0f);
		return;
	}

	PlayVoice(pszCat, true, 3.0f);
}

void CNPC_Soldat::LostEnemySound(CBaseEntity* pEnemy)
{
	PlayVoice("where_is", false, 6.0f);
}

void CNPC_Soldat::AlertSound(void)
{
	PlayVoice("glimpse", false, 6.0f);
}

void CNPC_Soldat::IdleSound(void)
{
	if (gpGlobals->curtime < m_flNextIdleTalkT)
		return;
	m_flNextIdleTalkT = gpGlobals->curtime + RandomFloat(20.0f, 60.0f);
	PlayVoice("talk", false, 5.0f);
}

void CNPC_Soldat::PainSound(const CTakeDamageInfo& info)
{
	if (GetFlags() & FL_DISSOLVING)
		return;
	PlayVoice("hurt", true, 1.0f);
}

void CNPC_Soldat::DeathSound(const CTakeDamageInfo& info)
{
	PlayVoice("hurt", true);
}

void CNPC_Soldat::NotifyDeadFriend(CBaseEntity* pFriend)
{
	// the base class must see his manhack die; it also says its own stock line
	BaseClass::NotifyDeadFriend(pFriend);

	if (!pFriend || FClassnameIs(pFriend, "npc_manhack"))
		return;

	// losses round him (B.FriendFalls)
	m_flMorale = MAX(0.0f, m_flMorale - 0.18f / m_flNerve);

	bool bAlone = !GetSquad() || GetSquad()->NumMembers() <= 1;
	PlayVoice(bAlone ? "alone_mandown" : "mandown", true, 2.0f);
}

void CNPC_Soldat::FollowSound()
{
	PlayVoice("ack", true);
}

void CNPC_Soldat::StopFollowSound()
{
	PlayVoice("hold", true);
}

// situational lines with no hook of their own (V.UpdateVoice)
void CNPC_Soldat::UpdateVoice()
{
	if (!npc_soldat_voice.GetBool())
		return;

	// a kill, once per kill
	if (HasCondition(COND_ENEMY_DEAD))
	{
		if (!m_bKillSpoken)
		{
			m_bKillSpoken = true;
			CBaseEntity* pEnemy = GetEnemy();
			bool bCreature = pEnemy && (pEnemy->Classify() == CLASS_ZOMBIE || pEnemy->Classify() == CLASS_HEADCRAB || pEnemy->Classify() == CLASS_ANTLION);
			PlayVoice(bCreature ? "creature_dead" : "kill", false, 4.0f);
		}
	}
	else
	{
		m_bKillSpoken = false;
	}

	if (GetState() == NPC_STATE_COMBAT && HasCondition(COND_LOW_PRIMARY_AMMO) && !HasCondition(COND_NO_PRIMARY_AMMO))
		PlayVoice("low_ammo", false, 12.0f);
}

int CNPC_Soldat::DrawDebugTextOverlays(void)
{
	int text_offset = BaseClass::DrawDebugTextOverlays();

	if (m_debugOverlays & OVERLAY_TEXT_BIT)
	{
		char tempstr[256];
		Q_snprintf(tempstr, sizeof(tempstr), "Soldat: %s, voice %s, morale %.2f, suppr %.2f%s",
			s_Roles[m_iRole].pszName, s_pszVoices[SoldatClampI(m_iVoice, 0, (int)ARRAYSIZE(s_pszVoices) - 1)], m_flMorale, m_flSuppr,
			m_bPanicking ? (m_bPanicFlee ? " PANIC(flee)" : " PANIC(cower)") : "");
		EntityText(text_offset, tempstr, 0);
		text_offset++;
	}
	return text_offset;
}