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
ConVar npc_soldat_investigate_following("npc_soldat_investigate_following", "0", FCVAR_NONE, "1 = soldats following the player (commanded) still wander off to investigate sounds.");
ConVar npc_soldat_debug("npc_soldat_debug", "0", FCVAR_NONE, "1 = print soldat decisions.");

// Role behaviour
ConVar npc_soldat_role_behavior("npc_soldat_role_behavior", "1", FCVAR_NONE, "1 = each role fights its own way (suppressing, overwatch, breaching, fire and manoeuvre...). 0 = stock Combine tactics for everyone.");
ConVar npc_soldat_push_enabled("npc_soldat_push_enabled", "1", FCVAR_NONE, "1 = riflemen/grenadiers/AT push forward while a base of fire keeps the enemy's head down.");
ConVar npc_soldat_rally("npc_soldat_rally", "1", FCVAR_NONE, "1 = leaders steady squadmates whose morale is low.");

// Russian deliberate-assault doctrine (squad level)
ConVar npc_soldat_doctrine("npc_soldat_doctrine", "1", FCVAR_NONE, "1 = squads with a leader fight a deliberate assault: support-by-fire, probe in bounding teams, hold if stalled, exploit a breach. 0 = old per-role push logic.");
ConVar npc_soldat_support_time("npc_soldat_support_time", "5", FCVAR_NONE, "Minimum seconds the support element fires before the assault teams step off.");
ConVar npc_soldat_bound_time("npc_soldat_bound_time", "4", FCVAR_NONE, "Seconds each assault team moves before the next team takes its turn.");
ConVar npc_soldat_breach_dist("npc_soldat_breach_dist", "12", FCVAR_NONE, "Metres: an assault man this close to the enemy position counts as a breach and the whole assault element goes in.");
ConVar npc_soldat_hold_time("npc_soldat_hold_time", "8", FCVAR_NONE, "Minimum seconds a stalled probe holds ground before the leader tries again.");
// Auto squad (keyvalue soldatautosquad overrides per soldat)
ConVar npc_soldat_auto_squad("npc_soldat_auto_squad", "1", FCVAR_NONE, "1 = soldats with no squadname group up with nearby soldats. A map can opt a soldat out with soldatautosquad 0.");
ConVar npc_soldat_auto_squad_range("npc_soldat_auto_squad_range", "30", FCVAR_NONE, "Metres: how close soldats must be to auto-squad.");
ConVar npc_soldat_auto_squad_max("npc_soldat_auto_squad_max", "8", FCVAR_NONE, "Max members in an auto squad.");

bool npc_soldat_auto_squad_enabled() { return npc_soldat_auto_squad.GetBool(); }

ConVar npc_soldat_exploit_time("npc_soldat_exploit_time", "12", FCVAR_NONE, "Seconds the assault element presses through a breach before the leader re-evaluates.");

// Idle chatter
ConVar npc_soldat_chatter("npc_soldat_chatter", "1", FCVAR_NONE, "1 = relaxed soldats talk amongst themselves.");
ConVar npc_soldat_chatter_range("npc_soldat_chatter_range", "12", FCVAR_NONE, "Metres: how close (and in view) two soldats must be to chat.");
ConVar npc_soldat_chatter_min("npc_soldat_chatter_min", "7", FCVAR_NONE, "Minimum seconds between conversations.");
ConVar npc_soldat_chatter_max("npc_soldat_chatter_max", "18", FCVAR_NONE, "Maximum seconds between conversations.");
ConVar npc_soldat_chatter_lines("npc_soldat_chatter_lines", "3", FCVAR_NONE, "Max extra back-and-forth lines after the opener and first reply.");

// ---------------------------------------------------------------------------
// Roles. Weapons come from the weapon_css_* set (scripts/weapon_css_*.txt):
//   ak47 / mp5navy / tmp / xm1014 / m249 / awp / glock
//   rifleman  AK-47        leader   MP5 + Glock    autorifle M249 + Glock
//   grenadier AK-47        medic    TMP            marksman  AK-47 + Glock
//   rto       MP5          at       TMP + Glock    sniper    AWP + Glock
//   hsniper   AWP + Glock  breacher XM1014 + Glock
// A weapon set by the mapper (spawnequipment) still wins; the role only fills
// in the weapon when the map did not.
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
	{ "rifleman",	"weapon_css_ak47",		NULL,				2, 2 },
	{ "leader",		"weapon_css_mp5navy",	"weapon_css_glock",	1, 2 },
	{ "autorifle",	"weapon_css_sg552",		"weapon_css_glock",	1, 1 },
	{ "grenadier",	"weapon_css_ak47",		NULL,				3, 2 },
	{ "medic",		"weapon_css_tmp",		NULL,				1, 1 },
	{ "marksman",	"weapon_css_ak47",		"weapon_css_glock",	1, 3 },
	{ "rto",		"weapon_css_mp5navy",	NULL,				1, 2 },
	{ "at",			"weapon_css_tmp",		"weapon_css_glock",	1, 2 },
	{ "sniper",		"weapon_css_awp",		"weapon_css_glock",	0, 4 },
	{ "hsniper",	"weapon_css_awp",		"weapon_css_glock",	0, 4 },
	{ "breacher",	"weapon_css_xm1014",	"weapon_css_glock",	2, 2 },
};

// Spawn weights for random roles (same order as SoldatRole_t)
static const int s_RoleWeights[SOLDAT_ROLE_COUNT] =
{
	36,	// rifleman
	6,	// leader
	9,	// autorifle
	9,	// grenadier
	7,	// medic
	7,	// marksman
	6,	// rto
	6,	// at
	4,	// sniper
	2,	// hsniper
	8,	// breacher
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
	"helmet_goggles_soldier",		// breacher
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
DEFINE_KEYFIELD(m_iAutoSquad, FIELD_INTEGER, "soldatautosquad"),
DEFINE_FIELD(m_flNextAutoSquadT, FIELD_TIME),
DEFINE_FIELD(m_flNextCommandCheckT, FIELD_TIME),
DEFINE_FIELD(m_flSkill, FIELD_FLOAT),
DEFINE_FIELD(m_flNerve, FIELD_FLOAT),
DEFINE_FIELD(m_flFear, FIELD_FLOAT),
DEFINE_FIELD(m_flDiscipline, FIELD_FLOAT),
DEFINE_FIELD(m_iszPrimary, FIELD_STRING),
DEFINE_FIELD(m_bActingLeader, FIELD_BOOLEAN),
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
DEFINE_FIELD(m_bInBurst, FIELD_BOOLEAN),
DEFINE_FIELD(m_flBurstEndT, FIELD_TIME),
DEFINE_FIELD(m_flBurstPauseT, FIELD_TIME),
DEFINE_FIELD(m_flNextBackAwayT, FIELD_TIME),
DEFINE_FIELD(m_flNextRelocateT, FIELD_TIME),
DEFINE_FIELD(m_flLastHitT, FIELD_TIME),
DEFINE_FIELD(m_flPushUntilT, FIELD_TIME),
DEFINE_FIELD(m_flNextPushCheckT, FIELD_TIME),
DEFINE_FIELD(m_flNextGrenadeT, FIELD_TIME),
DEFINE_FIELD(m_flNextRallyT, FIELD_TIME),
DEFINE_FIELD(m_flNextMedicCallT, FIELD_TIME),
DEFINE_FIELD(m_flNextVoiceT, FIELD_TIME),
DEFINE_FIELD(m_flNextRadioT, FIELD_TIME),
DEFINE_FIELD(m_flNextIdleTalkT, FIELD_TIME),
DEFINE_FIELD(m_bKillSpoken, FIELD_BOOLEAN),
DEFINE_FIELD(m_flNextChatT, FIELD_TIME),
DEFINE_FIELD(m_flChatReplyT, FIELD_TIME),
DEFINE_FIELD(m_hChatPartner, FIELD_EHANDLE),
DEFINE_FIELD(m_iChatLeft, FIELD_INTEGER),
DEFINE_FIELD(m_iAssaultPhase, FIELD_INTEGER),
DEFINE_FIELD(m_flPhaseStartT, FIELD_TIME),
DEFINE_FIELD(m_flNextPhaseThinkT, FIELD_TIME),
DEFINE_FIELD(m_iBoundTeam, FIELD_INTEGER),
DEFINE_FIELD(m_flNextBoundT, FIELD_TIME),
DEFINE_FIELD(m_vecStrongpoint, FIELD_POSITION_VECTOR),
DEFINE_FIELD(m_nAssaultStart, FIELD_INTEGER),
DEFINE_FIELD(m_bHasSupport, FIELD_BOOLEAN),
END_DATADESC()

//-----------------------------------------------------------------------------
CNPC_Soldat::CNPC_Soldat()
{
	m_iRole = -1;		// -1 = not set by the map: random in Spawn
	m_iVoice = -1;
	m_iAutoSquad = -1;		// -1 = npc_soldat_auto_squad convar
	m_flNextAutoSquadT = m_flNextCommandCheckT = 0.0f;
	m_flSkill = 1.0f;
	m_flNerve = 1.0f;
	m_flFear = 0.4f;
	m_flDiscipline = 0.5f;
	m_iszPrimary = NULL_STRING;
	m_bActingLeader = false;
	m_flMorale = 1.0f;
	m_flSuppr = 0.0f;
	m_flLastThinkT = 0.0f;
	m_flLastSupprHeardT = 0.0f;
	m_bPanicking = m_bPanicFlee = m_bPanicSpoken = false;
	m_iLastProficiency = -1;
	m_flEngageFirstUntil = m_flStartleUntil = m_flNextOrderT = m_flNextWeaponCheckT = m_flPistolNearT = 0.0f;

	m_bInBurst = false;
	m_flBurstEndT = m_flBurstPauseT = 0.0f;
	m_flNextBackAwayT = m_flNextRelocateT = m_flLastHitT = 0.0f;
	m_flPushUntilT = m_flNextPushCheckT = m_flNextGrenadeT = 0.0f;
	m_flNextRallyT = m_flNextMedicCallT = 0.0f;

	m_flNextVoiceT = m_flNextRadioT = m_flNextIdleTalkT = 0.0f;
	m_bKillSpoken = false;
	m_bPrecached = false;

	m_flNextChatT = 0.0f;
	m_flChatReplyT = 0.0f;
	m_hChatPartner = NULL;
	m_iChatLeft = 0;

	m_iAssaultPhase = SOLDAT_PHASE_NONE;
	m_flPhaseStartT = m_flNextPhaseThinkT = m_flNextBoundT = 0.0f;
	m_iBoundTeam = 0;
	m_vecStrongpoint = vec3_origin;
	m_nAssaultStart = 0;
	m_bHasSupport = false;

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

	// stagger the first chat so a freshly spawned group doesn't all open at once
	m_flNextChatT = gpGlobals->curtime + RandomFloat(2.0f, npc_soldat_chatter_max.GetFloat());

	// let the rest of the map's soldats spawn before the first auto-squad look
	m_flNextAutoSquadT = gpGlobals->curtime + RandomFloat(0.5f, 1.5f);

	const SoldatRoleDef_t& R = s_Roles[m_iRole];

	if (GetModelName() == NULL_STRING)
	{
		if (npc_soldat_model.GetString()[0])
			SetModelName(AllocPooledString(npc_soldat_model.GetString()));
		else
			SetModelName(AllocPooledString(CFmtStr("models/taggart/%s/%s.mdl", SoldatCamoFolder(), s_pszRoleModelFile[m_iRole])));
	}

	// the role's weapon, unless the map set spawnequipment
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

	// no enemy: reset the burst cycle and any push so the next fight starts fresh
	if (!GetEnemy())
	{
		m_bInBurst = false;
		m_flBurstPauseT = 0.0f;
		m_flPushUntilT = 0.0f;
	}

	UpdateFeelings(dt);
	UpdateProficiency();
	UpdateWeaponSwap();
	UpdateAutoSquad();
	UpdateCommandSuccession();
	UpdateLeaderOrders();
	UpdateLeaderRally();
	UpdateAssaultPhase();
	UpdateVoice();
	UpdateChatReply();
	UpdateIdleChatter();
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

	// head down while rounds are cracking about (a sniper or marksman keeps his head up to shoot)
	if (m_flSuppr > 0.5f && !IsMoving() && GetState() == NPC_STATE_COMBAT &&
		m_iRole != SOLDAT_ROLE_SNIPER && m_iRole != SOLDAT_ROLE_HSNIPER)
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
	if (!IsLeaderRole() || !npc_soldat_orders.GetBool())
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

//-----------------------------------------------------------------------------
// A leader steadies the men around him: shaken soldiers within 25 m slowly
// get their nerve back, which can pull them out of a panic.
//-----------------------------------------------------------------------------
void CNPC_Soldat::UpdateLeaderRally()
{
	if (!npc_soldat_rally.GetBool() || !IsLeaderRole() || GetState() != NPC_STATE_COMBAT)
		return;

	float now = gpGlobals->curtime;
	if (now < m_flNextRallyT)
		return;
	m_flNextRallyT = now + 1.0f;

	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return;

	bool bRallied = false;
	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (!pS || pS == this || !pS->IsAlive())
			continue;

		if ((pS->GetAbsOrigin() - GetAbsOrigin()).Length() / SOLDAT_M2U > 25.0f)
			continue;

		if (pS->m_flMorale < 0.45f)
		{
			pS->m_flMorale = MIN(1.0f, pS->m_flMorale + 0.08f);
			bRallied = true;
		}
	}

	if (bRallied)
	{
		PlayVoice("hold", false, 4.0f);

		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s: rallying the squad\n", GetDebugName());
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

		// snipers, the machine gun, the marksman, the medic and the radioman hold back
		CNPC_Soldat* pSoldat = dynamic_cast<CNPC_Soldat*>(pMember);
		if (pSoldat)
		{
			// with the doctrine on, soldats take their orders from the assault phase instead
			if (npc_soldat_doctrine.GetBool())
				continue;

			const int r = pSoldat->GetSoldatRole();
			if (r == SOLDAT_ROLE_SNIPER || r == SOLDAT_ROLE_HSNIPER || r == SOLDAT_ROLE_AUTORIFLE ||
				r == SOLDAT_ROLE_MARKSMAN || r == SOLDAT_ROLE_MEDIC || r == SOLDAT_ROLE_RTO)
				continue;
		}

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

	// ...or the radioman steps up: the squad is shaken but keeps its structure
	CNPC_Soldat* pRTO = NULL;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (pS && pS != this && pS->IsAlive() && pS->GetSoldatRole() == SOLDAT_ROLE_RTO)
		{
			pRTO = pS;
			break;
		}
	}

	if (pRTO)
	{
		pRTO->m_bActingLeader = true;
		pRTO->EmitSound("VAC_RU.radio_blip");
		pRTO->PlayVoice("radio_contact", true);

		for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
		{
			CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
			if (pS && pS != this && pS->IsAlive())
				pS->m_flMorale = MAX(0.0f, pS->m_flMorale - 0.08f / pS->m_flNerve);
		}

		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s: leader down, RTO %s takes command\n", GetDebugName(), pRTO->GetDebugName());
		return;
	}

	// no leader and no radioman: the squad is shaken, and the next in the succession list takes command
	CNPC_Soldat* pSuccessor = FindAssaultCommander(this);
	if (pSuccessor)
	{
		pSuccessor->m_bActingLeader = true;

		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s: leader down, %s takes command\n", GetDebugName(), pSuccessor->GetDebugName());
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
	if (IsLeaderRole() && npc_soldat_leader_down.GetBool())
		HandleLeaderDown();

	BaseClass::Event_Killed(info);
}

int CNPC_Soldat::OnTakeDamage_Alive(const CTakeDamageInfo& info)
{
	AddSuppression(0.5f);
	m_flMorale = MAX(0.0f, m_flMorale - 0.12f / m_flNerve);
	m_flLastHitT = gpGlobals->curtime;

	int ret = BaseClass::OnTakeDamage_Alive(info);

	// badly hurt with a medic in the squad: call for him, and he answers
	const float flNow = gpGlobals->curtime;
	if (ret > 0 && IsAlive() && flNow >= m_flNextMedicCallT && m_iRole != SOLDAT_ROLE_MEDIC &&
		(float)GetHealth() / (float)MAX(GetMaxHealth(), 1) < 0.5f)
	{
		CNPC_Soldat* pMedic = FindSquadMedic();
		if (pMedic)
		{
			m_flNextMedicCallT = flNow + 12.0f;
			PlayVoice("medic", true, 2.0f);
			pMedic->PlayVoice("aid", true, 2.0f);
		}
	}

	return ret;
}

CNPC_Soldat* CNPC_Soldat::FindSquadMedic()
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return NULL;

	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (pS && pS != this && pS->IsAlive() && pS->GetSoldatRole() == SOLDAT_ROLE_MEDIC)
			return pS;
	}
	return NULL;
}

void CNPC_Soldat::BoostSquadMorale(float flAmount)
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return;

	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (pS && pS->IsAlive())
			pS->m_flMorale = MIN(1.0f, pS->m_flMorale + flAmount);
	}
}

//-----------------------------------------------------------------------------
// Investigation veto (hook from CNPC_CombineHLABase): a soldat that is following
// the player doesn't wander off to look at noises. Snipers hold their position.
//-----------------------------------------------------------------------------
bool CNPC_Soldat::ShouldInvestigateSound(CSound* pSound, CBaseEntity* pOwner)
{
	if (m_iRole == SOLDAT_ROLE_SNIPER || m_iRole == SOLDAT_ROLE_HSNIPER)
		return false;

	if (!npc_soldat_investigate_following.GetBool() && m_FollowBehavior.GetFollowTarget())
		return false;

	return true;
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

			// the chat is over
			m_flChatReplyT = 0.0f;

			AddLookTarget(vecAt + Vector(0, 0, 40), 0.8f, 1.5f, 0.1f);
			PlayVoice(bClose ? "startle" : "heard", true, 4.0f);

			if (npc_soldat_debug.GetBool())
				DevMsg("npc_soldat %s: startle %.0f m, judging %.2f s\n", GetDebugName(), flDist, flJudge);
		}
	}
}

//-----------------------------------------------------------------------------
// Burst cycle helper. Returns true if he should be HOLDING fire right now.
// On for flOn seconds, then off for flOff seconds, repeating.
//-----------------------------------------------------------------------------
bool CNPC_Soldat::BurstHold(float flOnMin, float flOnMax, float flOffMin, float flOffMax)
{
	const float now = gpGlobals->curtime;

	if (m_bInBurst)
	{
		if (now < m_flBurstEndT)
			return false;

		m_bInBurst = false;
		m_flBurstPauseT = now + RandomFloat(flOffMin, flOffMax);
		return true;
	}

	if (now < m_flBurstPauseT)
		return true;

	m_bInBurst = true;
	m_flBurstEndT = now + RandomFloat(flOnMin, flOnMax);
	return false;
}

bool CNPC_Soldat::InBurstPause() const
{
	return !m_bInBurst && gpGlobals->curtime < m_flBurstPauseT;
}

int CNPC_Soldat::RelocateToCover(float flCooldown)
{
	m_flNextRelocateT = gpGlobals->curtime + flCooldown;
	return SCHED_TAKE_COVER_FROM_ENEMY;
}

int CNPC_Soldat::BackAway()
{
	m_flNextBackAwayT = gpGlobals->curtime + 1.5f;
	return SCHED_BACK_AWAY_FROM_ENEMY;
}

//-----------------------------------------------------------------------------
// Deliberate assault at squad level (after the Russian small-unit model:
// recon-attack teams probe, special infantry supports by fire, assault infantry
// exploits the breach). Mapped onto our roles:
//
//   support element   autorifle, marksman, sniper, hsniper (+ Suppressor)
//                     fix the enemy from where they stand, never push
//   assault element   rifleman, grenadier, AT, breacher
//                     advance in teams of up to 3, one team moving while the
//                     others cover; go to ground if the probe stalls; rush a
//                     breach, a grenade first
//   command/security  leader (runs the phases from cover), RTO, medic
//
// The leader (or acting leader) owns the phase; everybody else reads it.
//   SUPPORT -> PROBE -> (HOLD -> SUPPORT ...) / EXPLOIT -> SUPPORT ...
//-----------------------------------------------------------------------------
static const char* s_pszPhaseNames[] = { "none", "support-by-fire", "probe", "hold", "exploit" };

// Who commands when the leader is dead, or when the squad never had one: the first role
// in this list that is still alive in the squad (ties go to squad order). Edit the order
// to taste. The commander runs the assault phases, the advance orders and the rally.
static const int s_CommandSuccession[] =
{
	SOLDAT_ROLE_LEADER,
	SOLDAT_ROLE_RTO,			// the radioman is already on the net
	SOLDAT_ROLE_RIFLEMAN,		// senior rifleman / deputy
	SOLDAT_ROLE_GRENADIER,
	SOLDAT_ROLE_BREACHER,
	SOLDAT_ROLE_AT,
	SOLDAT_ROLE_MARKSMAN,
	SOLDAT_ROLE_AUTORIFLE,
	SOLDAT_ROLE_MEDIC,
	SOLDAT_ROLE_SNIPER,			// snipers work alone, they lead only when nobody else is left
	SOLDAT_ROLE_HSNIPER,
};

static int SoldatCommandRank(int iRole)
{
	for (int i = 0; i < ARRAYSIZE(s_CommandSuccession); i++)
	{
		if (s_CommandSuccession[i] == iRole)
			return i;
	}
	return ARRAYSIZE(s_CommandSuccession);
}

// the squad's commander: best-ranked living soldat (pExclude = someone who is dying)
CNPC_Soldat* CNPC_Soldat::FindAssaultCommander(CNPC_Soldat* pExclude)
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return NULL;

	CNPC_Soldat* pBest = NULL;
	int iBestRank = 0x7FFFFFFF;

	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (!pS || pS == pExclude || !pS->IsAlive())
			continue;

		const int iRank = SoldatCommandRank(pS->m_iRole);
		if (iRank < iBestRank)
		{
			iBestRank = iRank;
			pBest = pS;
		}
	}
	return pBest;
}

// Every man keeps his own "acting leader" flag in step with the succession list, so the
// leader-only logic (orders, rally, leader-down) works for whoever is in command.
void CNPC_Soldat::UpdateCommandSuccession()
{
	const float now = gpGlobals->curtime;
	if (now < m_flNextCommandCheckT)
		return;
	m_flNextCommandCheckT = now + 1.0f;

	if (m_iRole == SOLDAT_ROLE_LEADER)
	{
		m_bActingLeader = false;
		return;
	}

	const bool bWas = m_bActingLeader;
	m_bActingLeader = (FindAssaultCommander() == this);

	if (m_bActingLeader && !bWas)
	{
		PlayVoice("ack", false, 3.0f);

		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s (%s): takes command of the squad\n", GetDebugName(), s_Roles[SoldatClampI(m_iRole, 0, SOLDAT_ROLE_COUNT - 1)].pszName);
	}
}

//-----------------------------------------------------------------------------
// Auto squad: soldats with no squad of their own group up with nearby soldats, so
// the assault doctrine has a squad to work with. A mapper's squadname always wins
// (we only ever act on a soldat that has no squad), and soldatautosquad 0 opts a
// soldat out completely (he neither joins nor gets joined).
//-----------------------------------------------------------------------------
void CNPC_Soldat::UpdateAutoSquad()
{
	const float now = gpGlobals->curtime;
	if (now < m_flNextAutoSquadT)
		return;
	m_flNextAutoSquadT = now + 2.0f + RandomFloat(0.0f, 0.5f);

	if (!IsAlive() || GetSquad() || !AutoSquadEnabled() || IsInAScript())
		return;

	const float flRange = npc_soldat_auto_squad_range.GetFloat() * SOLDAT_M2U;
	const int   nMax = MAX(2, npc_soldat_auto_squad_max.GetInt());

	CBaseEntity* pList[64];
	int nCount = UTIL_EntitiesInSphere(pList, ARRAYSIZE(pList), GetAbsOrigin(), flRange, FL_NPC);

	CNPC_Soldat* pBestSquadded = NULL;
	CNPC_Soldat* pBestLoner = NULL;
	float flSquadded = FLT_MAX, flLoner = FLT_MAX;

	for (int i = 0; i < nCount; i++)
	{
		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(pList[i]);
		if (!pS || pS == this || !pS->IsAlive() || !pS->AutoSquadEnabled())
			continue;

		// same side only
		if (IRelationType(pS) == D_HT)
			continue;

		const float flDist = GetAbsOrigin().DistToSqr(pS->GetAbsOrigin());

		if (pS->GetSquad())
		{
			if (pS->GetSquad()->NumMembers() < nMax && flDist < flSquadded)
			{
				flSquadded = flDist;
				pBestSquadded = pS;
			}
		}
		else if (flDist < flLoner)
		{
			flLoner = flDist;
			pBestLoner = pS;
		}
	}

	if (pBestSquadded)
	{
		AddToSquad(AllocPooledString(pBestSquadded->GetSquad()->GetName()));

		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s: auto-joined squad %s\n", GetDebugName(), pBestSquadded->GetSquad()->GetName());
	}
	else if (pBestLoner)
	{
		char szName[64];
		Q_snprintf(szName, sizeof(szName), "soldat_squad_%d", entindex());
		string_t iszName = AllocPooledString(szName);

		pBestLoner->AddToSquad(iszName);
		AddToSquad(iszName);

		if (npc_soldat_debug.GetBool())
			DevMsg("npc_soldat %s: started squad %s with %s\n", GetDebugName(), szName, pBestLoner->GetDebugName());
	}
}

void CNPC_Soldat::SetAssaultPhase(int iPhase)
{
	if (iPhase == m_iAssaultPhase)
		return;

	m_iAssaultPhase = iPhase;
	m_flPhaseStartT = gpGlobals->curtime;

	if (iPhase == SOLDAT_PHASE_PROBE)
	{
		m_iBoundTeam = 0;
		m_flNextBoundT = gpGlobals->curtime + npc_soldat_bound_time.GetFloat();
	}

	if (npc_soldat_debug.GetBool())
		DevMsg("npc_soldat %s: assault phase -> %s\n", GetDebugName(), s_pszPhaseNames[SoldatClampI(iPhase, 0, SOLDAT_PHASE_EXPLOIT)]);
}

// which bounding team am I in (0..*pnTeams-1)? -1 if I'm not in the assault element
int CNPC_Soldat::GetAssaultTeam(CNPC_Soldat* pCmd, int* pnTeams)
{
	int n = 0, mine = -1;

	CAI_Squad* pSquad = GetSquad();
	if (pSquad)
	{
		AISquadIter_t iter;
		for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
		{
			CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
			if (!pS || !pS->IsAlive() || !pS->IsAssaultRole())
				continue;
			if (pS == this)
				mine = n;
			n++;
		}
	}

	// teams of up to 3; with no support element at least two teams so one can always cover the other
	int nTeams = MAX(1, (n + 2) / 3);
	if (n >= 2 && !pCmd->m_bHasSupport)
		nTeams = MAX(nTeams, 2);

	*pnTeams = nTeams;
	return mine < 0 ? -1 : mine % nTeams;
}

// Commander only: survey the squad and move the phase along.
void CNPC_Soldat::UpdateAssaultPhase()
{
	// every man checks (cheaply): only whoever the succession list puts in command runs the phases
	const float now = gpGlobals->curtime;
	if (now < m_flNextPhaseThinkT)
		return;
	m_flNextPhaseThinkT = now + 0.5f;

	CBaseEntity* pEnemy = GetEnemy();

	if (!npc_soldat_doctrine.GetBool() || !IsAlive() || m_bPanicking || GetState() != NPC_STATE_COMBAT ||
		!pEnemy || !pEnemy->IsAlive() || !GetSquad() || FindAssaultCommander() != this)
	{
		if (m_iAssaultPhase != SOLDAT_PHASE_NONE)
			SetAssaultPhase(SOLDAT_PHASE_NONE);
		return;
	}

	// where the enemy position is (marked while he can be seen, last known otherwise)
	const bool bSeen = HasCondition(COND_SEE_ENEMY);
	const Vector vecTarget = bSeen ? pEnemy->GetAbsOrigin() : GetEnemyLKP();
	if (bSeen)
		m_vecStrongpoint = vecTarget;

	// survey
	int nAssault = 0, nSupport = 0, nSupportEngaged = 0;
	float flSupprSum = 0.0f;
	float flClosest = FLT_MAX;

	CAI_Squad* pSquad = GetSquad();
	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		if (!p->IsAlive())
			continue;

		if (FClassnameIs(p, "npc_combine_suppressor"))
		{
			nSupport++;
			if (p->GetEnemy())
				nSupportEngaged++;
			continue;
		}

		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (!pS)
			continue;

		if (pS->IsSupportRole())
		{
			nSupport++;
			if (pS->GetEnemy() && pS->m_flSuppr < 0.8f)
				nSupportEngaged++;
		}
		else if (pS->IsAssaultRole())
		{
			nAssault++;
			flSupprSum += pS->m_flSuppr;
			flClosest = MIN(flClosest, (pS->GetAbsOrigin() - vecTarget).Length() / SOLDAT_M2U);
		}
	}

	m_bHasSupport = (nSupport > 0);

	// nobody to send forward: support men just fight where they are
	if (nAssault == 0)
	{
		if (m_iAssaultPhase != SOLDAT_PHASE_NONE)
			SetAssaultPhase(SOLDAT_PHASE_NONE);
		return;
	}

	const float flAvgSuppr = flSupprSum / (float)nAssault;
	const int   nLosses = m_nAssaultStart - nAssault;
	const bool  bBreach = flClosest < npc_soldat_breach_dist.GetFloat();
	const float flInPhase = now - m_flPhaseStartT;

	switch (m_iAssaultPhase)
	{
	case SOLDAT_PHASE_NONE:
		// contact: fix him first
		m_nAssaultStart = nAssault;
		SetAssaultPhase(SOLDAT_PHASE_SUPPORT);
		break;

	case SOLDAT_PHASE_SUPPORT:
		m_nAssaultStart = nAssault;

		if (bBreach)
		{
			// already on top of him
			SetAssaultPhase(SOLDAT_PHASE_EXPLOIT);
			PlayVoice("moving", true, 3.0f);
		}
		else if (flInPhase >= npc_soldat_support_time.GetFloat() && (nSupport == 0 || nSupportEngaged > 0) && flAvgSuppr < 0.5f)
		{
			// the base of fire is working: step off
			SetAssaultPhase(SOLDAT_PHASE_PROBE);
			PlayVoice("attack", true, 3.0f);
		}
		break;

	case SOLDAT_PHASE_PROBE:
		if (bBreach)
		{
			// a man is in: call the rest forward
			SetAssaultPhase(SOLDAT_PHASE_EXPLOIT);
			PlayVoice("moving", true, 3.0f);
		}
		else if (flAvgSuppr > 0.55f || nLosses >= 2)
		{
			// stalled: hold what we have, it becomes the start line for the next attempt
			SetAssaultPhase(SOLDAT_PHASE_HOLD);
			PlayVoice("hold", true, 3.0f);
		}
		else if (now >= m_flNextBoundT)
		{
			m_iBoundTeam++;
			m_flNextBoundT = now + npc_soldat_bound_time.GetFloat();
		}
		break;

	case SOLDAT_PHASE_HOLD:
		// support keeps firing; once things quieten, fix him again and have another go
		if (flInPhase >= npc_soldat_hold_time.GetFloat() && flAvgSuppr < 0.3f)
			SetAssaultPhase(SOLDAT_PHASE_SUPPORT);
		break;

	case SOLDAT_PHASE_EXPLOIT:
		if (flAvgSuppr > 0.7f || nLosses >= 2)
			SetAssaultPhase(SOLDAT_PHASE_HOLD);
		else if (flInPhase >= npc_soldat_exploit_time.GetFloat())
			SetAssaultPhase(SOLDAT_PHASE_SUPPORT);	// enemy still alive: re-assess
		break;
	}
}

// What a man in the ASSAULT element does in the current phase. SCHED_NONE = stock AI
// (which also covers the support element: they keep their own role stance).
int CNPC_Soldat::SelectDoctrineSchedule(CNPC_Soldat* pCmd, bool bSeen, float flDist, bool bHeavy, CBaseEntity* pEnemy)
{
	if (!IsAssaultRole() || bHeavy)
		return SCHED_NONE;

	const float now = gpGlobals->curtime;
	const bool  bReachable = !IsUnreachable(pEnemy);

	switch (pCmd->m_iAssaultPhase)
	{
	case SOLDAT_PHASE_PROBE:
	{
		int nTeams = 1;
		const int iTeam = GetAssaultTeam(pCmd, &nTeams);
		const bool bMyTurn = (iTeam < 0) || (pCmd->m_iBoundTeam % nTeams == iTeam);

		if (bMyTurn)
		{
			if (m_flSuppr < 0.6f && bReachable)
			{
				PlayVoice("moving", false, 4.0f);
				return SCHED_COMBINE_PRESS_ATTACK;
			}
		}
		else if (bSeen && HasCondition(COND_CAN_RANGE_ATTACK1) && OccupyStrategySlotRange(SQUAD_SLOT_ATTACK1, SQUAD_SLOT_ATTACK2))
		{
			// the other team is moving: cover them
			return SCHED_RANGE_ATTACK1;
		}

		if (bSeen && m_flSuppr > 0.5f && now >= m_flNextRelocateT)
			return RelocateToCover(6.0f);
		break;
	}

	case SOLDAT_PHASE_HOLD:
		// go to ground and keep the position
		if (bSeen && m_flSuppr > 0.4f && now >= m_flNextRelocateT)
			return RelocateToCover(6.0f);
		break;

	case SOLDAT_PHASE_EXPLOIT:
		// clear the position: a grenade before going in, then everyone in
		if (m_iNumGrenades > 0 && !bSeen && flDist > 4.0f && flDist < 20.0f &&
			now >= m_flNextGrenadeT && HasCondition(COND_CAN_RANGE_ATTACK2) &&
			OccupyStrategySlot(SQUAD_SLOT_SPECIAL_ATTACK))
		{
			m_flNextGrenadeT = now + RandomFloat(5.0f, 8.0f);
			return SCHED_RANGE_ATTACK2;
		}

		if (m_flSuppr < 0.75f && bReachable)
			return SCHED_COMBINE_PRESS_ATTACK;
		break;

	default:	// SUPPORT: take position, shoot from cover (stock AI)
		break;
	}

	return SCHED_NONE;
}

// Is a squadmate (or a Suppressor) engaged and pinning the enemy down?
bool CNPC_Soldat::SquadHasBaseOfFire()
{
	CAI_Squad* pSquad = GetSquad();
	if (!pSquad)
		return false;

	AISquadIter_t iter;
	for (CAI_BaseNPC* p = pSquad->GetFirstMember(&iter); p; p = pSquad->GetNextMember(&iter))
	{
		if (p == this || !p->IsAlive() || !p->GetEnemy())
			continue;

		if (FClassnameIs(p, "npc_combine_suppressor"))
			return true;

		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(p);
		if (!pS)
			continue;

		const int r = pS->GetSoldatRole();
		if (r == SOLDAT_ROLE_AUTORIFLE || r == SOLDAT_ROLE_MARKSMAN || r == SOLDAT_ROLE_SNIPER || r == SOLDAT_ROLE_HSNIPER)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Keep the stubborn roles from being yanked out of their firing schedules, and
// keep the pause between bursts from being cut short.
//-----------------------------------------------------------------------------
void CNPC_Soldat::BuildScheduleTestBits()
{
	BaseClass::BuildScheduleTestBits();

	if (GetState() != NPC_STATE_COMBAT || !npc_soldat_role_behavior.GetBool())
		return;

	const bool bStubborn = (m_iRole == SOLDAT_ROLE_AUTORIFLE || m_iRole == SOLDAT_ROLE_HSNIPER);
	const bool bPacedShooter = bStubborn || m_iRole == SOLDAT_ROLE_SNIPER || m_iRole == SOLDAT_ROLE_MARKSMAN;

	if (bStubborn)
	{
		const bool bFiring = IsCurSchedule(SCHED_COMBINE_RANGE_ATTACK1, false) ||
			IsCurSchedule(SCHED_COMBINE_SUPPRESS, false) ||
			IsCurSchedule(SCHED_COMBINE_SIGNAL_SUPPRESS, false);

		if (bFiring)
		{
			// he stands his ground: getting shot doesn't make him flinch,
			// and he fires until the clip is actually empty
			ClearCustomInterruptCondition(COND_LIGHT_DAMAGE);
			ClearCustomInterruptCondition(COND_HEAVY_DAMAGE);
			ClearCustomInterruptCondition(COND_LOW_PRIMARY_AMMO);
		}
	}

	if (bPacedShooter && IsCurSchedule(SCHED_COMBAT_FACE, false) && InBurstPause())
	{
		// being able to attack again must not end the pause early
		ClearCustomInterruptCondition(COND_CAN_RANGE_ATTACK1);
		ClearCustomInterruptCondition(COND_CAN_RANGE_ATTACK2);
	}
}

//-----------------------------------------------------------------------------
// Role behaviour. Anything that returns SCHED_NONE falls through to the stock
// Combine AI (grenade dodging, melee, reloading, cover...).
//-----------------------------------------------------------------------------
int CNPC_Soldat::SelectRoleSchedule()
{
	if (!npc_soldat_role_behavior.GetBool() || GetState() != NPC_STATE_COMBAT)
		return SCHED_NONE;

	CBaseEntity* pEnemy = GetEnemy();
	CBaseCombatWeapon* pActive = GetActiveWeapon();
	if (!pEnemy || !pEnemy->IsAlive() || !pActive)
		return SCHED_NONE;

	// stock AI owns these: fire, danger, melee, panic, scripts, dead enemy, enemies he's afraid of
	if (m_bPanicking || IsInAScript() || IsOnFire() ||
		HasCondition(COND_ENEMY_DEAD) ||
		HasCondition(COND_HEAR_DANGER) || HasCondition(COND_HEAR_MOVE_AWAY) ||
		HasCondition(COND_HEAR_PHYSICS_DANGER) ||
		HasCondition(COND_COMBINE_HIT_BY_BUGBAIT) ||
		HasCondition(COND_CAN_MELEE_ATTACK1) ||
		IRelationType(pEnemy) == D_FR)
		return SCHED_NONE;

	// role tactics are for the primary weapon; sidearm / RPG use the stock AI
	if (m_iszPrimary != NULL_STRING && !FClassnameIs(pActive, STRING(m_iszPrimary)))
		return SCHED_NONE;

	// dry: the machine gunner reloads right where he stands, everyone else uses the stock hide-and-reload
	if (HasCondition(COND_NO_PRIMARY_AMMO) || (pActive->UsesClipsForAmmo1() && pActive->Clip1() <= 0))
		return (m_iRole == SOLDAT_ROLE_AUTORIFLE) ? SCHED_RELOAD : SCHED_NONE;

	// a friend in the line of fire: stock AI sorts it out
	if (HasCondition(COND_WEAPON_BLOCKED_BY_FRIEND))
		return SCHED_NONE;

	const float now = gpGlobals->curtime;
	const bool  bSeen = HasCondition(COND_SEE_ENEMY);
	const float flSinceSeen = now - GetEnemyLastTimeSeen();
	const float flDist = (pEnemy->GetAbsOrigin() - GetAbsOrigin()).Length() / SOLDAT_M2U;
	const bool  bHeavy = IsHeavyEnemy(pEnemy);

	// ---- Grenadier: flush hiders / long-range targets with a grenade, eagerly
	if (m_iRole == SOLDAT_ROLE_GRENADIER && m_iNumGrenades > 0 && !bHeavy &&
		now >= m_flNextGrenadeT && HasCondition(COND_CAN_RANGE_ATTACK2) &&
		(!bSeen || flDist > 12.0f) &&
		OccupyStrategySlot(SQUAD_SLOT_SPECIAL_ATTACK))
	{
		m_flNextGrenadeT = now + RandomFloat(6.0f, 10.0f);
		return SCHED_RANGE_ATTACK2;
	}

	// ---- Fire and manoeuvre: movers go forward while a base of fire pins the enemy
	// ---- Squad doctrine: while the leader runs an assault, the phase decides what the assault
	// element does. With no leader (or no phase running yet) the old autonomous push below applies.
	CNPC_Soldat* pCmd = npc_soldat_doctrine.GetBool() ? FindAssaultCommander() : NULL;
	const bool bDoctrine = pCmd && pCmd->GetAssaultPhase() != SOLDAT_PHASE_NONE;
	if (bDoctrine)
	{
		int iDoc = SelectDoctrineSchedule(pCmd, bSeen, flDist, bHeavy, pEnemy);
		if (iDoc != SCHED_NONE)
			return iDoc;
	}

	const bool bMover = (m_iRole == SOLDAT_ROLE_RIFLEMAN || m_iRole == SOLDAT_ROLE_GRENADIER || m_iRole == SOLDAT_ROLE_AT);
	if (!bDoctrine && bMover && npc_soldat_push_enabled.GetBool())
	{
		if (now < m_flPushUntilT)
		{
			if (m_flSuppr > 0.6f || bHeavy || IsUnreachable(pEnemy))
				m_flPushUntilT = 0.0f;		// too hot (or pointless): break off
			else
				return SCHED_COMBINE_PRESS_ATTACK;
		}
		else if (now >= m_flNextPushCheckT)
		{
			m_flNextPushCheckT = now + 2.0f;

			const bool bHiding = (!bSeen && flSinceSeen > 3.0f && flSinceSeen < 20.0f);
			const bool bAtOpen = (m_iRole == SOLDAT_ROLE_AT && bSeen && flDist > 10.0f && flDist < 45.0f);

			if ((bHiding || bAtOpen) && m_flSuppr < 0.3f && !bHeavy && !IsUnreachable(pEnemy) &&
				SquadHasBaseOfFire() &&
				RandomFloat(0.0f, 1.0f) < 0.45f * (1.2f - m_flFear))
			{
				m_flPushUntilT = now + RandomFloat(5.0f, 9.0f);
				m_flNextPushCheckT = m_flPushUntilT + 10.0f;
				PlayVoice("moving", true, 2.0f);

				if (npc_soldat_debug.GetBool())
					DevMsg("npc_soldat %s (%s): pushing\n", GetDebugName(), s_Roles[m_iRole].pszName);

				return SCHED_COMBINE_PRESS_ATTACK;
			}
		}
	}

	// ---- Per-role stance
	switch (m_iRole)
	{
	case SOLDAT_ROLE_AUTORIFLE:
		// he is the base of fire: long bursts at the enemy, and at their last position when they duck
		if (bSeen && flDist < 6.0f && now >= m_flNextBackAwayT)
			return BackAway();

		if ((bSeen && HasCondition(COND_CAN_RANGE_ATTACK1)) || (!bSeen && flSinceSeen <= (bDoctrine ? 10.0f : 6.0f)))
		{
			if (BurstHold(4.0f, 7.0f, 1.2f, 2.0f))
				return SCHED_COMBAT_FACE;
			return SCHED_RANGE_ATTACK1;
		}
		break;

	case SOLDAT_ROLE_MARKSMAN:
		// overwatch: paced shots from where he stands, backs off close contacts
		if (bSeen && flDist < 8.0f && now >= m_flNextBackAwayT)
			return BackAway();

		if (bSeen && m_flSuppr > 0.5f && now >= m_flNextRelocateT)
			return RelocateToCover(8.0f);

		if (bSeen && flDist > 20.0f && HasCondition(COND_CAN_RANGE_ATTACK1))
		{
			if (BurstHold(1.5f, 3.0f, 0.6f, 1.2f))
				return SCHED_COMBAT_FACE;
			return SCHED_RANGE_ATTACK1;
		}
		break;

	case SOLDAT_ROLE_SNIPER:
	case SOLDAT_ROLE_HSNIPER:
	{
		// the sniper moves when found (hit, or rushed); the heavy sniper never moves
		const bool bStatic = (m_iRole == SOLDAT_ROLE_HSNIPER);

		if (!bStatic && now >= m_flNextRelocateT)
		{
			if ((bSeen && flDist < 15.0f) || (now - m_flLastHitT < 0.6f))
				return RelocateToCover(10.0f);
		}

		if (bSeen && HasCondition(COND_CAN_RANGE_ATTACK1))
		{
			const bool bHold = bStatic ? BurstHold(0.8f, 1.5f, 2.5f, 4.0f)
				: BurstHold(0.8f, 1.5f, 1.5f, 3.0f);
			if (bHold)
				return SCHED_COMBAT_FACE;
			return SCHED_RANGE_ATTACK1;
		}

		// lost him: stay put and wait for him to show himself again
		if (!bSeen && flSinceSeen <= 20.0f)
			return SCHED_COMBAT_FACE;
		break;
	}

	case SOLDAT_ROLE_BREACHER:
		// shotgun: get in close, or rush where the enemy was last seen
		// (under a squad assault he goes in with the exploit phase instead)
		if (!bDoctrine && !bHeavy && flDist > 7.0f && m_flSuppr < 0.6f && !IsUnreachable(pEnemy) &&
			(bSeen || flSinceSeen < 12.0f))
			return SCHED_COMBINE_PRESS_ATTACK;
		break;

	case SOLDAT_ROLE_LEADER:
	case SOLDAT_ROLE_RTO:
		// directs the fight from cover rather than the front
		if (bSeen && m_flSuppr > 0.35f && now >= m_flNextRelocateT)
			return RelocateToCover(8.0f);
		break;

	case SOLDAT_ROLE_MEDIC:
		// keeps his head down so he is still there to patch people up
		if (bSeen && m_flSuppr > 0.2f && now >= m_flNextRelocateT)
			return RelocateToCover(6.0f);
		break;

	case SOLDAT_ROLE_AT:
		// no launcher: he doesn't stand in front of armour
		if (bHeavy && !npc_soldat_at_use_rpg.GetBool() && bSeen && now >= m_flNextRelocateT)
			return RelocateToCover(6.0f);
		break;

	default:
		break;
	}

	return SCHED_NONE;
}

//-----------------------------------------------------------------------------
// Combat: a close contact in the open is shot at first (B.ENGAGE_FIRST), then the
// role's own tactics, then the stock Combine AI.
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

	int iRole = SelectRoleSchedule();
	if (iRole != SCHED_NONE)
		return iRole;

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

//-----------------------------------------------------------------------------
// Idle chatter: relaxed soldiers who can see each other talk amongst themselves.
// One opens, the other answers a moment later (turning to look at the speaker),
// and the exchange may go back and forth a few more lines. Gunfire, an enemy,
// panic or a script cuts it off.
//-----------------------------------------------------------------------------
bool CNPC_Soldat::IsRelaxed()
{
	if (!IsAlive() || m_bPanicking || IsInAScript() || IsOnFire())
		return false;

	if (IsCurSchedule(SCHED_INVESTIGATE_SOUND, false))
		return false;

	const NPC_STATE state = GetState();
	if (state != NPC_STATE_IDLE && state != NPC_STATE_ALERT)
		return false;

	if (GetEnemy() || m_flSuppr > 0.1f || gpGlobals->curtime < m_flStartleUntil)
		return false;

	return true;
}

CNPC_Soldat* CNPC_Soldat::FindChatPartner()
{
	const float flRange = npc_soldat_chatter_range.GetFloat() * SOLDAT_M2U;

	CBaseEntity* pList[32];
	int nCount = UTIL_EntitiesInSphere(pList, ARRAYSIZE(pList), GetAbsOrigin(), flRange, FL_NPC);

	CNPC_Soldat* pBest = NULL;
	float flBest = FLT_MAX;

	for (int i = 0; i < nCount; i++)
	{
		CBaseEntity* pEnt = pList[i];
		if (!pEnt || pEnt == this)
			continue;

		CNPC_Soldat* pS = dynamic_cast<CNPC_Soldat*>(pEnt);
		if (!pS || !pS->IsAlive())
			continue;

		// not mid-conversation, and relaxed too
		if (pS->m_flChatReplyT > 0.0f || !pS->IsRelaxed())
			continue;

		// same side only (a soldat set up as an enemy of another must not chat with it)
		if (IRelationType(pS) == D_HT)
			continue;

		if (!FVisible(pS))
			continue;

		float flDist = GetAbsOrigin().DistToSqr(pS->GetAbsOrigin());
		if (flDist < flBest)
		{
			flBest = flDist;
			pBest = pS;
		}
	}

	return pBest;
}

void CNPC_Soldat::UpdateIdleChatter()
{
	if (!npc_soldat_chatter.GetBool() || !npc_soldat_voice.GetBool())
		return;

	const float now = gpGlobals->curtime;
	if (now < m_flNextChatT || m_flChatReplyT > 0.0f)
		return;

	if (!IsRelaxed())
		return;

	CNPC_Soldat* pPartner = FindChatPartner();
	if (!pPartner)
	{
		m_flNextChatT = now + 4.0f;		// nobody to talk to: look again shortly
		return;
	}

	const float flMin = npc_soldat_chatter_min.GetFloat();
	const float flMax = MAX(flMin, npc_soldat_chatter_max.GetFloat());
	m_flNextChatT = now + RandomFloat(flMin, flMax);
	pPartner->m_flNextChatT = MAX(pPartner->m_flNextChatT, m_flNextChatT);

	// the opener: mostly small talk, sometimes an "all quiet"
	PlayVoice(RandomFloat(0.0f, 1.0f) < 0.8f ? "talk" : "clear", true, 1.0f);
	AddLookTarget(pPartner->EyePosition(), 0.6f, 1.0f, 0.1f);

	// he answers a moment later
	pPartner->m_hChatPartner = this;
	pPartner->m_flChatReplyT = now + RandomFloat(1.8f, 3.2f);
	pPartner->m_iChatLeft = RandomInt(0, MAX(0, npc_soldat_chatter_lines.GetInt()));

	if (npc_soldat_debug.GetBool())
		DevMsg("npc_soldat %s: chatting with %s\n", GetDebugName(), pPartner->GetDebugName());
}

void CNPC_Soldat::UpdateChatReply()
{
	if (m_flChatReplyT <= 0.0f || gpGlobals->curtime < m_flChatReplyT)
		return;

	const float now = gpGlobals->curtime;
	m_flChatReplyT = 0.0f;

	CNPC_Soldat* pTo = dynamic_cast<CNPC_Soldat*>(m_hChatPartner.Get());
	if (!pTo || !IsRelaxed() || !pTo->IsRelaxed() || !npc_soldat_chatter.GetBool())
	{
		m_iChatLeft = 0;
		return;
	}

	PlayVoice(RandomFloat(0.0f, 1.0f) < 0.65f ? "talk" : "ack", true, 1.0f);
	AddLookTarget(pTo->EyePosition(), 0.6f, 1.0f, 0.1f);

	// keep it going for a few more lines
	if (m_iChatLeft > 0)
	{
		pTo->m_hChatPartner = this;
		pTo->m_flChatReplyT = now + RandomFloat(2.0f, 4.0f);
		pTo->m_iChatLeft = m_iChatLeft - 1;
	}
	m_iChatLeft = 0;
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
	// any chat stops the moment there is a contact
	m_flChatReplyT = 0.0f;

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

		// the radioman calling it in steadies the squad
		if (m_iRole == SOLDAT_ROLE_RTO)
			BoostSquadMorale(0.08f);
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
		Q_snprintf(tempstr, sizeof(tempstr), "Soldat: %s%s, voice %s, morale %.2f, suppr %.2f%s",
			s_Roles[SoldatClampI(m_iRole, 0, SOLDAT_ROLE_COUNT - 1)].pszName, m_bActingLeader ? " (acting leader)" : "",
			s_pszVoices[SoldatClampI(m_iVoice, 0, (int)ARRAYSIZE(s_pszVoices) - 1)], m_flMorale, m_flSuppr,
			m_bPanicking ? (m_bPanicFlee ? " PANIC(flee)" : " PANIC(cower)") : "");
		EntityText(text_offset, tempstr, 0);
		text_offset++;

		if (IsLeaderRole())
		{
			Q_snprintf(tempstr, sizeof(tempstr), "Assault: %s, bound team %d, support %s",
				s_pszPhaseNames[SoldatClampI(m_iAssaultPhase, 0, SOLDAT_PHASE_EXPLOIT)], m_iBoundTeam, m_bHasSupport ? "yes" : "no");
			EntityText(text_offset, tempstr, 0);
			text_offset++;
		}
	}
	return text_offset;
}