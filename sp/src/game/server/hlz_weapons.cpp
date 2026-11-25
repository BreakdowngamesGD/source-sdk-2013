#include "cbase.h"
#include "hlz_weapons.h"
#include "npcevent.h"
#include "ai_basenpc.h"
#include "player.h"
#include "in_buttons.h"
#include "ammodef.h"
#include "basecombatcharacter.h"
#include "vstdlib/random.h"
#include "soundent.h"
#include "gamerules.h"
#include "game.h"

#include "grenade_ar2.h"
#include "rumble_shared.h"
#include "gamestats.h"

extern ConVar sk_plr_dmg_smg1_grenade;

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//=============================================================================
// Shared HLZ Helpers & Base Implementations
//=============================================================================
static void HlzToggleCrossbowZoom(CBaseCombatWeapon* pWeapon, CBasePlayer* pPlayer, bool& bZoomed, int targetFOV = 20)
{
	if (!pPlayer)
		return;

	if (bZoomed)
	{
		if (pPlayer->SetFOV(pWeapon, 0, 0.2f))
			bZoomed = false;
	}
	else
	{
		if (pPlayer->SetFOV(pWeapon, targetFOV, 0.1f))
			bZoomed = true;
	}
}

// Returns true if a bullet was actually fired
static bool HlzFireStandardBullet(CBaseHLCombatWeapon* pWeapon, float flDamage, const Vector& vecSpread, float flFireDelay, Activity actAnim)
{
	CBasePlayer* pPlayer = ToBasePlayer(pWeapon->GetOwner());
	if (!pPlayer)
		return false;

	bool bHasAmmo;
	if (pWeapon->UsesClipsForAmmo1())
		bHasAmmo = (pWeapon->m_iClip1 > 0);
	else
		bHasAmmo = (pPlayer->GetAmmoCount(pWeapon->GetPrimaryAmmoType()) > 0);

	if (!bHasAmmo)
	{
		pWeapon->SendWeaponAnim(ACT_VM_DRYFIRE);
		pWeapon->WeaponSound(EMPTY);
		pWeapon->m_flNextPrimaryAttack = gpGlobals->curtime + 0.25f;
		return false;
	}

	if (pWeapon->UsesClipsForAmmo1())
		pWeapon->m_iClip1 = pWeapon->m_iClip1 - 1;
	else
		pPlayer->RemoveAmmo(1, pWeapon->GetPrimaryAmmoType());

	pWeapon->WeaponSound(SINGLE);
	pPlayer->DoMuzzleFlash();
	pWeapon->SendWeaponAnim(actAnim);
	pPlayer->SetAnimation(PLAYER_ATTACK1);

	FireBulletsInfo_t info;
	info.m_iShots = 1;
	info.m_vecSrc = pPlayer->Weapon_ShootPosition();
	info.m_vecDirShooting = pPlayer->GetAutoaimVector(AUTOAIM_SCALE_DEFAULT);
	info.m_vecSpread = vecSpread;
	info.m_flDistance = MAX_TRACE_LENGTH;
	info.m_iAmmoType = pWeapon->GetPrimaryAmmoType();
	info.m_iTracerFreq = 2;
	info.m_flDamage = flDamage;
	info.m_iPlayerDamage = (int)flDamage;	// players use this one, not m_flDamage
	info.m_pAttacker = pPlayer;

	pPlayer->FireBullets(info);

	pPlayer->SetMuzzleFlashTime(gpGlobals->curtime + 0.5f);
	CSoundEnt::InsertSound(SOUND_COMBAT, pWeapon->GetAbsOrigin(), SOUNDENT_VOLUME_PISTOL, 0.2f, pPlayer);

	pWeapon->m_flNextPrimaryAttack = pWeapon->m_flNextSecondaryAttack = gpGlobals->curtime + flFireDelay;
	pWeapon->SetWeaponIdleTime(gpGlobals->curtime + pWeapon->SequenceDuration());

	return true;
}

//-----------------------------------------------------------------------------
// NPC helpers
//
// An NPC never calls PrimaryAttack(). Its attack animation contains an event
// (EVENT_WEAPON_AR2, EVENT_WEAPON_PISTOL_FIRE, ...) which the engine hands to
// the weapon's Operator_HandleAnimEvent(). That function must then fire the
// bullet itself. These two helpers do the shared work.
//-----------------------------------------------------------------------------

// Works out where an NPC's bullet starts and where it is aimed.
// Same logic the stock SMG1 uses. Returns false if the operator isn't an NPC.
static bool HlzGetNPCShootVectors(animevent_t* pEvent, CBaseCombatCharacter* pOperator, Vector& vecSrc, Vector& vecDir)
{
	CAI_BaseNPC* npc = pOperator->MyNPCPointer();
	if (!npc)
		return false;

	// Support old style attachment point firing (event options = attachment name)
	QAngle angDiscard;
	if ((pEvent->options == NULL) || (pEvent->options[0] == '\0') ||
		(!pOperator->GetAttachment(pEvent->options, vecSrc, angDiscard)))
	{
		vecSrc = pOperator->Weapon_ShootPosition();
	}

	// Accuracy comes from the NPC's proficiency (GetProficiencyValues) here,
	// which is why the spread passed to FireBullets below is PRECALCULATED.
	vecDir = npc->GetActualShootTrajectory(vecSrc);
	return true;
}

// Fires the NPC's shot: sound, AI sound, bullets, muzzle flash, uses one round.
static void HlzNPCFire(CBaseCombatWeapon* pWeapon, CBaseCombatCharacter* pOperator,
	const Vector& vecSrc, const Vector& vecDir,
	int nShots, const Vector& vecSpread, int iSoundVolume, int iTracerFreq)
{
	// WeaponSound is public (WeaponSoundRealtime isn't reachable from a free function)
	pWeapon->WeaponSound(SINGLE_NPC);

	CSoundEnt::InsertSound(SOUND_COMBAT | SOUND_CONTEXT_GUNFIRE, pOperator->GetAbsOrigin(),
		iSoundVolume, 0.2, pOperator, SOUNDENT_CHANNEL_WEAPON, pOperator->GetEnemy());

	pOperator->FireBullets(nShots, vecSrc, vecDir, vecSpread, MAX_TRACE_LENGTH,
		pWeapon->GetPrimaryAmmoType(), iTracerFreq);

	pOperator->DoMuzzleFlash();
	pWeapon->m_iClip1 = pWeapon->m_iClip1 - 1;
}

//--- CHLZ_SMG1Base ---
IMPLEMENT_SERVERCLASS_ST(CHLZ_SMG1Base, DT_HLZ_SMG1Base)
END_SEND_TABLE()

//-----------------------------------------------------------------------------
// NPC activity translation for everything built on the SMG1 base (AK-47, AR-M1, M16).
//
// These models have no SMG grip, so NPCs must use exactly what a real weapon_ar2
// uses. Instead of keeping a hand-copied table, we look the activity up in
// CWeaponAR2's own table. GetAR2Acttable()/GetAR2ActtableCount() are two tiny
// accessors you add to weapon_ar2.cpp (see the instructions that came with this file).
//-----------------------------------------------------------------------------
extern acttable_t* GetAR2Acttable();
extern int GetAR2ActtableCount();

Activity CHLZ_SMG1Base::ActivityOverride(Activity baseAct, bool* pRequired)
{
	acttable_t* pTable = GetAR2Acttable();
	int nCount = GetAR2ActtableCount();

	for (int i = 0; i < nCount; i++)
	{
		if (pTable[i].baseAct == baseAct)
		{
			if (pRequired)
				*pRequired = pTable[i].required;
			return (Activity)pTable[i].weaponAct;
		}
	}

	// Not in the AR2 table: whatever the base class would do (usually "unchanged")
	return BaseClass::ActivityOverride(baseAct, pRequired);
}

CHLZ_SMG1Base::CHLZ_SMG1Base()
{
	m_fMinRange1 = 0;
	m_fMaxRange1 = 1500;
	m_bAltFiresUnderwater = false;
}

void CHLZ_SMG1Base::Precache(void)
{
	UTIL_PrecacheOther("grenade_ar2");	// alt-fire grenade
	BaseClass::Precache();
}

const Vector& CHLZ_SMG1Base::GetBulletSpread(void)
{
	static const Vector s_vecSpread = VECTOR_CONE_5DEGREES;
	return s_vecSpread;
}

// Same kick as weapon_smg1 (EZ values)
void CHLZ_SMG1Base::AddViewKick(void)
{
#define	HLZ_SMG_EASY_DAMPEN			2.5f
#define	HLZ_SMG_MAX_VERTICAL_KICK	11.0f
#define	HLZ_SMG_SLIDE_LIMIT			2.0f

	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	DoMachineGunKick(pPlayer, HLZ_SMG_EASY_DAMPEN, HLZ_SMG_MAX_VERTICAL_KICK, m_fFireDuration, HLZ_SMG_SLIDE_LIMIT);
}

bool CHLZ_SMG1Base::Reload(void)
{
	float fCacheTime = m_flNextSecondaryAttack;

	bool fRet = DefaultReload(GetMaxClip1(), GetMaxClip2(), ACT_VM_RELOAD);
	if (fRet)
	{
		// Allow interrupting the reload to fire a grenade
		m_flNextSecondaryAttack = GetOwner()->m_flNextAttack = fCacheTime;
		WeaponSound(RELOAD);
	}
	return fRet;
}

// Copy of weapon_smg1's grenade alt-fire (player only; NPCs never alt-fire)
void CHLZ_SMG1Base::SecondaryAttack(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	// Must have ammo
	if ((pPlayer->GetAmmoCount(m_iSecondaryAmmoType) <= 0) || (pPlayer->GetWaterLevel() == 3))
	{
		SendWeaponAnim(ACT_VM_DRYFIRE);
		BaseClass::WeaponSound(EMPTY);
		m_flNextSecondaryAttack = gpGlobals->curtime + 0.5f;
		return;
	}

	if (m_bInReload)
		m_bInReload = false;

	BaseClass::WeaponSound(WPN_DOUBLE);

	pPlayer->RumbleEffect(RUMBLE_357, 0, RUMBLE_FLAGS_NONE);

	Vector vecSrc = pPlayer->Weapon_ShootPosition();
	Vector vecThrow;
	AngleVectors(pPlayer->EyeAngles() + pPlayer->GetPunchAngle(), &vecThrow);
	VectorScale(vecThrow, 1000.0f, vecThrow);

	QAngle angles;
	VectorAngles(vecThrow, angles);
	CGrenadeAR2* pGrenade = (CGrenadeAR2*)Create("grenade_ar2", vecSrc, angles, pPlayer);
	pGrenade->SetAbsVelocity(vecThrow);
	pGrenade->SetLocalAngularVelocity(RandomAngle(-400, 400));
	pGrenade->SetMoveType(MOVETYPE_FLYGRAVITY, MOVECOLLIDE_FLY_BOUNCE);
	pGrenade->SetThrower(GetOwner());
	pGrenade->SetDamage(sk_plr_dmg_smg1_grenade.GetFloat());

	SendWeaponAnim(ACT_VM_SECONDARYATTACK);

	CSoundEnt::InsertSound(SOUND_COMBAT, GetAbsOrigin(), 1000, 0.2, GetOwner(), SOUNDENT_CHANNEL_WEAPON);

	pPlayer->SetAnimation(PLAYER_ATTACK2);
	pPlayer->RemoveAmmo(1, m_iSecondaryAmmoType);

	m_flNextPrimaryAttack = gpGlobals->curtime + 0.5f;
	m_flNextSecondaryAttack = gpGlobals->curtime + 1.0f;

	pPlayer->SetMuzzleFlashTime(gpGlobals->curtime + 0.5);

	m_iSecondaryAttacks++;
	gamestats->Event_WeaponFired(pPlayer, false, GetClassname());
}

// NPC fire. Because we translate ACT_RANGE_ATTACK1 -> ACT_RANGE_ATTACK_AR2, the
// attack animation emits EVENT_WEAPON_AR2. We also accept EVENT_WEAPON_SMG1 in
// case a model uses that one instead.
void CHLZ_SMG1Base::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	switch (pEvent->event)
	{
	case EVENT_WEAPON_AR2:
	case EVENT_WEAPON_SMG1:
	{
		Vector vecSrc, vecDir;
		if (HlzGetNPCShootVectors(pEvent, pOperator, vecSrc, vecDir))
			HlzNPCFire(this, pOperator, vecSrc, vecDir, 1, VECTOR_CONE_PRECALCULATED, SOUNDENT_VOLUME_MACHINEGUN, 2);
		break;
	}

	default:
		BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
		break;
	}
}

//--- CHLZ_PistolBase ---
IMPLEMENT_SERVERCLASS_ST(CHLZ_PistolBase, DT_HLZ_PistolBase)
END_SEND_TABLE()

// Same table as the stock weapon_pistol
acttable_t CHLZ_PistolBase::m_acttable[] =
{
	{ ACT_IDLE,						ACT_IDLE_PISTOL,				true },
	{ ACT_IDLE_ANGRY,				ACT_IDLE_ANGRY_PISTOL,			true },
	{ ACT_RANGE_ATTACK1,			ACT_RANGE_ATTACK_PISTOL,		true },
	{ ACT_RELOAD,					ACT_RELOAD_PISTOL,				true },
	{ ACT_WALK_AIM,					ACT_WALK_AIM_PISTOL,			true },
	{ ACT_RUN_AIM,					ACT_RUN_AIM_PISTOL,				true },
	{ ACT_GESTURE_RANGE_ATTACK1,	ACT_GESTURE_RANGE_ATTACK_PISTOL,true },
	{ ACT_RELOAD_LOW,				ACT_RELOAD_PISTOL_LOW,			false },
	{ ACT_RANGE_ATTACK1_LOW,		ACT_RANGE_ATTACK_PISTOL_LOW,	false },
	{ ACT_COVER_LOW,				ACT_COVER_PISTOL_LOW,			false },
	{ ACT_RANGE_AIM_LOW,			ACT_RANGE_AIM_PISTOL_LOW,		false },
	{ ACT_GESTURE_RELOAD,			ACT_GESTURE_RELOAD_PISTOL,		false },
	{ ACT_WALK,						ACT_WALK_PISTOL,				false },
	{ ACT_RUN,						ACT_RUN_PISTOL,					false },
};
IMPLEMENT_ACTTABLE(CHLZ_PistolBase);

CHLZ_PistolBase::CHLZ_PistolBase()
{
	m_fMinRange1 = 24;
	m_fMaxRange1 = 1500;
}

float CHLZ_PistolBase::GetFireRate(void)
{
	return 0.15f;
}

void CHLZ_PistolBase::Precache(void)
{
	BaseClass::Precache();
}

void CHLZ_PistolBase::PrimaryAttack(void)
{
	// Same as weapon_pistol: reset punch so rapid fire doesn't drift
	if (CBasePlayer* pOwner = ToBasePlayer(GetOwner()))
		pOwner->ViewPunchReset();

	if (HlzFireStandardBullet(this, 8.0f, GetBulletSpread(), 0.15f, ACT_VM_PRIMARYATTACK))
		AddViewKick();
}

const Vector& CHLZ_PistolBase::GetBulletSpread(void)
{
	static const Vector s_vecSpread = VECTOR_CONE_2DEGREES;
	return s_vecSpread;
}

void CHLZ_PistolBase::AddViewKick(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	QAngle viewPunch;
	viewPunch.x = random->RandomFloat(0.25f, 0.5f);
	viewPunch.y = random->RandomFloat(-0.6f, 0.6f);
	viewPunch.z = 0.0f;
	pPlayer->ViewPunch(viewPunch);
}

void CHLZ_PistolBase::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	switch (pEvent->event)
	{
	case EVENT_WEAPON_PISTOL_FIRE:
	{
		Vector vecSrc, vecDir;
		if (HlzGetNPCShootVectors(pEvent, pOperator, vecSrc, vecDir))
			HlzNPCFire(this, pOperator, vecSrc, vecDir, 1, VECTOR_CONE_PRECALCULATED, SOUNDENT_VOLUME_PISTOL, 2);
		break;
	}

	default:
		BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
		break;
	}
}

//--- CHLZ_357Base ---
IMPLEMENT_SERVERCLASS_ST(CHLZ_357Base, DT_HLZ_357Base)
END_SEND_TABLE()

// A revolver is held like a pistol, so NPCs borrow the pistol pose set
acttable_t CHLZ_357Base::m_acttable[] =
{
	{ ACT_IDLE,						ACT_IDLE_PISTOL,				true },
	{ ACT_IDLE_ANGRY,				ACT_IDLE_ANGRY_PISTOL,			true },
	{ ACT_RANGE_ATTACK1,			ACT_RANGE_ATTACK_PISTOL,		true },
	{ ACT_RELOAD,					ACT_RELOAD_PISTOL,				true },
	{ ACT_WALK_AIM,					ACT_WALK_AIM_PISTOL,			true },
	{ ACT_RUN_AIM,					ACT_RUN_AIM_PISTOL,				true },
	{ ACT_GESTURE_RANGE_ATTACK1,	ACT_GESTURE_RANGE_ATTACK_PISTOL,true },
	{ ACT_RELOAD_LOW,				ACT_RELOAD_PISTOL_LOW,			false },
	{ ACT_RANGE_ATTACK1_LOW,		ACT_RANGE_ATTACK_PISTOL_LOW,	false },
	{ ACT_COVER_LOW,				ACT_COVER_PISTOL_LOW,			false },
	{ ACT_RANGE_AIM_LOW,			ACT_RANGE_AIM_PISTOL_LOW,		false },
	{ ACT_GESTURE_RELOAD,			ACT_GESTURE_RELOAD_PISTOL,		false },
	{ ACT_WALK,						ACT_WALK_PISTOL,				false },
	{ ACT_RUN,						ACT_RUN_PISTOL,					false },
};
IMPLEMENT_ACTTABLE(CHLZ_357Base);

CHLZ_357Base::CHLZ_357Base()
{
	m_fMinRange1 = 24;
	m_fMaxRange1 = 2000;
}

void CHLZ_357Base::Precache(void)
{
	BaseClass::Precache();
}

void CHLZ_357Base::PrimaryAttack(void)
{
	if (HlzFireStandardBullet(this, 40.0f, VECTOR_CONE_1DEGREES, 0.75f, ACT_VM_PRIMARYATTACK))
		AddViewKick();
}

// Same as weapon_357
void CHLZ_357Base::AddViewKick(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	QAngle angles = pPlayer->GetLocalAngles();
	angles.x += random->RandomInt(-1, 1);
	angles.y += random->RandomInt(-1, 1);
	angles.z = 0;
	pPlayer->SnapEyeAngles(angles);

	pPlayer->ViewPunch(QAngle(-8, random->RandomFloat(-2, 2), 0));
}

void CHLZ_357Base::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	switch (pEvent->event)
	{
	case EVENT_WEAPON_PISTOL_FIRE:
	{
		Vector vecSrc, vecDir;
		if (HlzGetNPCShootVectors(pEvent, pOperator, vecSrc, vecDir))
			HlzNPCFire(this, pOperator, vecSrc, vecDir, 1, VECTOR_CONE_PRECALCULATED, SOUNDENT_VOLUME_PISTOL, 2);
		break;
	}

	default:
		BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
		break;
	}
}

//--- CHLZ_ShotgunBase ---
IMPLEMENT_SERVERCLASS_ST(CHLZ_ShotgunBase, DT_HLZ_ShotgunBase)
END_SEND_TABLE()

// Same table as the stock weapon_shotgun.
// If your shotgun model has no shotgun grip either, swap the *_SHOTGUN values
// for the *_RIFLE / AR2 ones exactly like the SMG1 table above.
acttable_t CHLZ_ShotgunBase::m_acttable[] =
{
	{ ACT_IDLE,						ACT_IDLE_SMG1,					true },
	{ ACT_RANGE_ATTACK1,			ACT_RANGE_ATTACK_SHOTGUN,		true },
	{ ACT_RELOAD,					ACT_RELOAD_SHOTGUN,				false },
	{ ACT_WALK,						ACT_WALK_RIFLE,					true },
	{ ACT_IDLE_ANGRY,				ACT_IDLE_ANGRY_SHOTGUN,			true },

	// Readiness activities (not aiming)
	{ ACT_IDLE_RELAXED,				ACT_IDLE_SHOTGUN_RELAXED,		false },
	{ ACT_IDLE_STIMULATED,			ACT_IDLE_SHOTGUN_STIMULATED,	false },
	{ ACT_IDLE_AGITATED,			ACT_IDLE_SHOTGUN_AGITATED,		false },

	{ ACT_WALK_RELAXED,				ACT_WALK_RIFLE_RELAXED,			false },
	{ ACT_WALK_STIMULATED,			ACT_WALK_RIFLE_STIMULATED,		false },
	{ ACT_WALK_AGITATED,			ACT_WALK_AIM_RIFLE,				false },

	{ ACT_RUN_RELAXED,				ACT_RUN_RIFLE_RELAXED,			false },
	{ ACT_RUN_STIMULATED,			ACT_RUN_RIFLE_STIMULATED,		false },
	{ ACT_RUN_AGITATED,				ACT_RUN_AIM_RIFLE,				false },

	// Readiness activities (aiming)
	{ ACT_IDLE_AIM_RELAXED,			ACT_IDLE_SMG1_RELAXED,			false },
	{ ACT_IDLE_AIM_STIMULATED,		ACT_IDLE_AIM_RIFLE_STIMULATED,	false },
	{ ACT_IDLE_AIM_AGITATED,		ACT_IDLE_ANGRY_SMG1,			false },

	{ ACT_WALK_AIM_RELAXED,			ACT_WALK_RIFLE_RELAXED,			false },
	{ ACT_WALK_AIM_STIMULATED,		ACT_WALK_AIM_RIFLE_STIMULATED,	false },
	{ ACT_WALK_AIM_AGITATED,		ACT_WALK_AIM_RIFLE,				false },

	{ ACT_RUN_AIM_RELAXED,			ACT_RUN_RIFLE_RELAXED,			false },
	{ ACT_RUN_AIM_STIMULATED,		ACT_RUN_AIM_RIFLE_STIMULATED,	false },
	{ ACT_RUN_AIM_AGITATED,			ACT_RUN_AIM_RIFLE,				false },

	{ ACT_WALK_AIM,					ACT_WALK_AIM_SHOTGUN,			true },
	{ ACT_WALK_CROUCH,				ACT_WALK_CROUCH_RIFLE,			true },
	{ ACT_WALK_CROUCH_AIM,			ACT_WALK_CROUCH_AIM_RIFLE,		true },
	{ ACT_RUN,						ACT_RUN_RIFLE,					true },
	{ ACT_RUN_AIM,					ACT_RUN_AIM_SHOTGUN,			true },
	{ ACT_RUN_CROUCH,				ACT_RUN_CROUCH_RIFLE,			true },
	{ ACT_RUN_CROUCH_AIM,			ACT_RUN_CROUCH_AIM_RIFLE,		true },

	{ ACT_GESTURE_RANGE_ATTACK1,	ACT_GESTURE_RANGE_ATTACK_SHOTGUN,true },
	{ ACT_RANGE_ATTACK1_LOW,		ACT_RANGE_ATTACK_SHOTGUN_LOW,	true },
	{ ACT_RELOAD_LOW,				ACT_RELOAD_SHOTGUN_LOW,			false },
	{ ACT_GESTURE_RELOAD,			ACT_GESTURE_RELOAD_SHOTGUN,		false },
};
IMPLEMENT_ACTTABLE(CHLZ_ShotgunBase);

CHLZ_ShotgunBase::CHLZ_ShotgunBase()
{
	m_fMinRange1 = 24;
	m_fMaxRange1 = 900;
}

void CHLZ_ShotgunBase::Precache(void)
{
	BaseClass::Precache();
}

void CHLZ_ShotgunBase::PrimaryAttack(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer || m_iClip1 <= 0)
		return;

	WeaponSound(SINGLE);
	pPlayer->DoMuzzleFlash();
	SendWeaponAnim(ACT_VM_PRIMARYATTACK);
	pPlayer->SetAnimation(PLAYER_ATTACK1);

	m_iClip1--;

	FireBulletsInfo_t info;
	info.m_iShots = 7;
	info.m_vecSrc = pPlayer->Weapon_ShootPosition();
	info.m_vecDirShooting = pPlayer->GetAutoaimVector(AUTOAIM_SCALE_DEFAULT);
	info.m_vecSpread = VECTOR_CONE_10DEGREES;
	info.m_flDistance = MAX_TRACE_LENGTH;
	info.m_iAmmoType = GetPrimaryAmmoType();
	info.m_flDamage = 8.0f;
	info.m_iPlayerDamage = 8;	// players use this one, not m_flDamage
	info.m_pAttacker = pPlayer;

	pPlayer->FireBullets(info);
	AddViewKick();

	m_flNextPrimaryAttack = m_flNextSecondaryAttack = gpGlobals->curtime + 0.75f;
}

void CHLZ_ShotgunBase::SecondaryAttack(void)
{
	PrimaryAttack();
}

// Same as weapon_shotgun's single-shot punch
void CHLZ_ShotgunBase::AddViewKick(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	pPlayer->ViewPunch(QAngle(random->RandomFloat(-2, -1), random->RandomFloat(-2, 2), 0));
}

void CHLZ_ShotgunBase::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	switch (pEvent->event)
	{
	case EVENT_WEAPON_SHOTGUN_FIRE:
	{
		Vector vecSrc, vecDir;
		if (HlzGetNPCShootVectors(pEvent, pOperator, vecSrc, vecDir))
		{
			// 7 pellets to match the player version. PRECALCULATED would send every
			// pellet down the same line, so pass a real cone to spread them.
			HlzNPCFire(this, pOperator, vecSrc, vecDir, 7, VECTOR_CONE_10DEGREES, SOUNDENT_VOLUME_SHOTGUN, 0);
		}
		break;
	}

	default:
		BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
		break;
	}
}

//=============================================================================
// Section 1: CMG / Combine Machine Gun (Ported from weapon_cmg.cpp)
//=============================================================================
ConVar sk_npc_dmg_cmg("sk_npc_dmg_cmg", "0", FCVAR_REPLICATED, "NPC damage per CMG bullet. 0 = use the AR2 ammo definition.");
ConVar sk_weapon_cmg_npc_burst_min("sk_weapon_cmg_npc_burst_min", "8");
ConVar sk_weapon_cmg_npc_burst_max("sk_weapon_cmg_npc_burst_max", "16");
ConVar sk_weapon_cmg_npc_fire_rate("sk_weapon_cmg_npc_fire_rate", "0.075");

BEGIN_DATADESC(CWeaponCMG)
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST(CWeaponCMG, DT_WeaponCMG)
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS(weapon_cmg, CWeaponCMG);
PRECACHE_WEAPON_REGISTER(weapon_cmg);

CWeaponCMG::CWeaponCMG()
{
	m_fMinRange1 = 0;
	m_fMaxRange1 = 3072;
}

void CWeaponCMG::PrimaryAttack(void)
{
	// Skip CWeaponAR2's EZ1 override and run CHLMachineGun's standard machine-gun fire
	CHLMachineGun::PrimaryAttack();
}

void CWeaponCMG::AddViewKick(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	QAngle viewPunch;
	viewPunch.x = random->RandomFloat(-0.6f, -1.2f);
	viewPunch.y = random->RandomFloat(-0.4f, 0.4f);
	viewPunch.z = 0.0f;
	pPlayer->ViewPunch(viewPunch);
}

void CWeaponCMG::FireNPCPrimaryAttack(CBaseCombatCharacter* pOperator, bool bUseWeaponAngles)
{
	Vector vecShootOrigin, vecShootDir;
	CAI_BaseNPC* npc = pOperator->MyNPCPointer();
	ASSERT(npc != NULL);

	if (bUseWeaponAngles)
	{
		QAngle angShootDir;
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

	int iDamage = sk_npc_dmg_cmg.GetInt();
	if (iDamage <= 0)
	{
		static ConVarRef s_sk_npc_dmg_ar2("sk_npc_dmg_ar2");
		iDamage = s_sk_npc_dmg_ar2.IsValid() ? s_sk_npc_dmg_ar2.GetInt() : 8;
	}
	pOperator->FireBullets(1, vecShootOrigin, vecShootDir, VECTOR_CONE_4DEGREES, MAX_TRACE_LENGTH, m_iPrimaryAmmoType, 2, entindex(), 0, iDamage);
	pOperator->DoMuzzleFlash();
	m_iClip1 = m_iClip1 - 1;
}

void CWeaponCMG::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	switch (pEvent->event)
	{
	case EVENT_WEAPON_AR2:
	case EVENT_WEAPON_SMG1:
		FireNPCPrimaryAttack(pOperator, false);
		break;

	case EVENT_WEAPON_AR2_GRENADE:
		// The CMG has no energy ball. Swallow the event so CWeaponAR2 doesn't fire one.
		break;

	default:
		BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
		break;
	}
}

int CWeaponCMG::WeaponRangeAttack2Condition(float flDot, float flDist)
{
	return COND_NONE;
}

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
	return proficiencyTable;
}

//=============================================================================
// Section 2: Nikita Launcher
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponNikitaLauncher, DT_WeaponNikitaLauncher)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_nikita_launcher, CWeaponNikitaLauncher);
PRECACHE_WEAPON_REGISTER(weapon_nikita_launcher);

//=============================================================================
// Section 3: AR-M1
//=============================================================================
BEGIN_DATADESC(CWeaponARM1)
DEFINE_FIELD(m_bInZoom, FIELD_BOOLEAN),
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST(CWeaponARM1, DT_WeaponARM1)
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS(weapon_ar_m1, CWeaponARM1);
PRECACHE_WEAPON_REGISTER(weapon_ar_m1);

CWeaponARM1::CWeaponARM1()
{
	m_bInZoom = false;
}

void CWeaponARM1::SecondaryAttack(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	bool bZoom = m_bInZoom;
	HlzToggleCrossbowZoom(this, pPlayer, bZoom, 20);
	m_bInZoom = bZoom;
	m_flNextSecondaryAttack = gpGlobals->curtime + 0.25f;
}

void CWeaponARM1::ItemPostFrame(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (m_bInZoom && pPlayer && (pPlayer->GetFOV() == 0))
	{
		m_bInZoom = false;
	}
	BaseClass::ItemPostFrame();
}

bool CWeaponARM1::Holster(CBaseCombatWeapon* pSwitchingTo)
{
	if (m_bInZoom)
	{
		if (CBasePlayer* pPlayer = ToBasePlayer(GetOwner()))
			pPlayer->SetFOV(this, 0, 0.1f);
		m_bInZoom = false;
	}
	return BaseClass::Holster(pSwitchingTo);
}

//=============================================================================
// Section 4: AK-47
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponAK47, DT_WeaponAK47)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_ak47, CWeaponAK47);
PRECACHE_WEAPON_REGISTER(weapon_ak47);

//=============================================================================
// Section 5: VP70
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponVP70, DT_WeaponVP70)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_vp70, CWeaponVP70);
PRECACHE_WEAPON_REGISTER(weapon_vp70);

//=============================================================================
// Section 6: Fort-12
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponFort12, DT_WeaponFort12)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_fort12, CWeaponFort12);
PRECACHE_WEAPON_REGISTER(weapon_fort12);

//=============================================================================
// Section 7: Desert Eagle
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponDesertEagle, DT_WeaponDesertEagle)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_desert_eagle, CWeaponDesertEagle);
PRECACHE_WEAPON_REGISTER(weapon_desert_eagle);

//=============================================================================
// Section 8: M16
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponM16, DT_WeaponM16)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_m16, CWeaponM16);
PRECACHE_WEAPON_REGISTER(weapon_m16);

//=============================================================================
// Section 9: Combine Shotty
//=============================================================================
IMPLEMENT_SERVERCLASS_ST(CWeaponCombineShotty, DT_WeaponCombineShotty)
END_SEND_TABLE()
LINK_ENTITY_TO_CLASS(weapon_combine_shotty, CWeaponCombineShotty);
PRECACHE_WEAPON_REGISTER(weapon_combine_shotty);

//=============================================================================
// Section 10: XM8
//=============================================================================
BEGIN_DATADESC(CWeaponXM8)
DEFINE_FIELD(m_bInZoom, FIELD_BOOLEAN),
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST(CWeaponXM8, DT_WeaponXM8)
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS(weapon_xm8, CWeaponXM8);
PRECACHE_WEAPON_REGISTER(weapon_xm8);

CWeaponXM8::CWeaponXM8()
{
	m_bInZoom = false;
	m_flLastKickTime = -1.0f;
}

void CWeaponXM8::PrimaryAttack(void)
{
	int iClipBefore = m_iClip1;

	BaseClass::PrimaryAttack();

	// Only kick if a round was actually used (no kick on dry fire)
	if (m_iClip1 < iClipBefore)
		AddViewKick();	// guarded, so no double kick if the base already did it
}

void CWeaponXM8::AddViewKick(void)
{
	// Only kick once per frame
	if (m_flLastKickTime == gpGlobals->curtime)
		return;

	m_flLastKickTime = gpGlobals->curtime;
	BaseClass::AddViewKick();	// the AR2 kick
}

void CWeaponXM8::SecondaryAttack(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (!pPlayer)
		return;

	bool bZoom = m_bInZoom;
	HlzToggleCrossbowZoom(this, pPlayer, bZoom, 20);
	m_bInZoom = bZoom;
	m_flNextSecondaryAttack = gpGlobals->curtime + 0.25f;
}

void CWeaponXM8::ItemPostFrame(void)
{
	CBasePlayer* pPlayer = ToBasePlayer(GetOwner());
	if (m_bInZoom && pPlayer && (pPlayer->GetFOV() == 0))
	{
		m_bInZoom = false;
	}
	BaseClass::ItemPostFrame();
}

bool CWeaponXM8::Holster(CBaseCombatWeapon* pSwitchingTo)
{
	if (m_bInZoom)
	{
		if (CBasePlayer* pPlayer = ToBasePlayer(GetOwner()))
			pPlayer->SetFOV(this, 0, 0.1f);
		m_bInZoom = false;
	}
	return BaseClass::Holster(pSwitchingTo);
}

// NPC: the AR2 base already handles EVENT_WEAPON_AR2 (normal shots) and has the
// AR2 pose table, so the only job here is to stop NPCs throwing energy balls.
void CWeaponXM8::Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator)
{
	if (pEvent->event == EVENT_WEAPON_AR2_GRENADE)
		return;

	BaseClass::Operator_HandleAnimEvent(pEvent, pOperator);
}

int CWeaponXM8::WeaponRangeAttack2Condition(float flDot, float flDist)
{
	return COND_NONE;
}