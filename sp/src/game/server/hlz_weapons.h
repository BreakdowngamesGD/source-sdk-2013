#ifndef HLZ_WEAPONS_H
#define HLZ_WEAPONS_H
#ifdef _WIN32
#pragma once
#endif

#include "basehlcombatweapon.h"
#include "weapon_ar2.h"
#include "weapon_rpg.h"


//=============================================================================
// Shared HLZ Base Classes & Helpers (usable by any weapon in hlz_weapons.*)
//
// NPC SUPPORT: every base below now has
//   1) DECLARE_ACTTABLE()           -> which animations an NPC plays while holding it
//   2) Operator_HandleAnimEvent()   -> what happens when the NPC's attack anim fires
//   3) burst / fire-rate values     -> how the NPC's shot regulator paces it
//=============================================================================
class CHLZ_SMG1Base : public CHLSelectFireMachineGun
{
	DECLARE_CLASS(CHLZ_SMG1Base, CHLSelectFireMachineGun);
public:
	DECLARE_SERVERCLASS();
	// NPC poses: forwards to the real AR2's activity table (see .cpp)
	Activity ActivityOverride(Activity baseAct, bool* pRequired) override;

	CHLZ_SMG1Base();
	void Precache(void) override;
	const Vector& GetBulletSpread(void) override;
	void SecondaryAttack(void) override;   // SMG1 grenade
	bool Reload(void) override;
	void AddViewKick(void) override;

	// NPC
	void Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator) override;
	int GetMinBurst(void) override { return 2; }
	int GetMaxBurst(void) override { return 5; }
};

class CHLZ_SMG2Base : public CHLSelectFireMachineGun
{
	DECLARE_CLASS(CHLZ_SMG2Base, CHLSelectFireMachineGun);
public:
	DECLARE_SERVERCLASS();
	CHLZ_SMG2Base();
	void Precache(void) override;
	const Vector& GetBulletSpread(void) override;
	void AddViewKick(void) override;
};


class CHLZ_PistolBase : public CHLMachineGun
{
	DECLARE_CLASS(CHLZ_PistolBase, CHLMachineGun);
public:
	DECLARE_SERVERCLASS();
	DECLARE_ACTTABLE();		// NPC pistol poses

	CHLZ_PistolBase();
	float GetFireRate(void) override;
	void Precache(void) override;
	void PrimaryAttack(void) override;
	const Vector& GetBulletSpread(void) override;
	void AddViewKick(void) override;

	// NPC
	void Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator) override;
	int GetMinBurst(void) override { return 1; }
	int GetMaxBurst(void) override { return 3; }
};

class CHLZ_357Base : public CBaseHLCombatWeapon
{
	DECLARE_CLASS(CHLZ_357Base, CBaseHLCombatWeapon);
public:
	DECLARE_SERVERCLASS();
	DECLARE_ACTTABLE();		// NPC pistol poses (revolver is held like a pistol)

	CHLZ_357Base();
	void Precache(void) override;
	void PrimaryAttack(void) override;
	float WeaponAutoAimScale(void) override { return 0.6f; }
	void AddViewKick(void) override;

	// NPC
	void Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator) override;
	float GetFireRate(void) override { return 0.75f; }
};

class CHLZ_ShotgunBase : public CBaseHLCombatWeapon
{
	DECLARE_CLASS(CHLZ_ShotgunBase, CBaseHLCombatWeapon);
public:
	DECLARE_SERVERCLASS();
	DECLARE_ACTTABLE();		// NPC shotgun poses

	CHLZ_ShotgunBase();
	void Precache(void) override;
	void PrimaryAttack(void) override;
	void SecondaryAttack(void) override;
	void AddViewKick(void) override;

	// NPC
	void Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator) override;
	float GetFireRate(void) override { return 0.75f; }
};

//=============================================================================
// Section 1: CMG / Combine Machine Gun (Ported from weapon_cmg.h/.cpp)
//=============================================================================
class CWeaponCMG : public CWeaponAR2
{
public:
	DECLARE_CLASS(CWeaponCMG, CWeaponAR2);

	CWeaponCMG();

	DECLARE_SERVERCLASS();
	DECLARE_DATADESC();

	// Player fire: plain machine-gun behavior, skipping the AR2's EZ1 override
	void PrimaryAttack(void);

	// No energy ball
	void SecondaryAttack(void) {}

	void AddViewKick(void);

	// NPC fire
	void FireNPCPrimaryAttack(CBaseCombatCharacter* pOperator, bool bUseWeaponAngles);
	void FireNPCSecondaryAttack(CBaseCombatCharacter* pOperator, bool bUseWeaponAngles) {}
	void Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator);

	// NPCs must never think they can alt-fire (the AR2 version would say yes)
	int WeaponRangeAttack2Condition(float flDot, float flDist) override;

	// Burst length (rounds per trigger pull) and cycle rate for NPCs
	int GetMinBurst(void);
	int GetMaxBurst(void);
	float GetFireRate(void);

	virtual const Vector& GetBulletSpread(void)
	{
		static Vector cone = VECTOR_CONE_4DEGREES;
		return cone;
	}

	const WeaponProficiencyInfo_t* GetProficiencyValues();
};

//=============================================================================
// Section 2: Nikita Launcher (RPG copy, no changes)
//=============================================================================
class CWeaponNikitaLauncher : public CWeaponRPG
{
	DECLARE_CLASS(CWeaponNikitaLauncher, CWeaponRPG);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 3: AR-M1 (SMG1-style base + crossbow-style zoom)
//=============================================================================
class CWeaponARM1 : public CHLZ_SMG1Base
{
	DECLARE_CLASS(CWeaponARM1, CHLZ_SMG1Base);
public:
	DECLARE_SERVERCLASS();
	DECLARE_DATADESC();

	CWeaponARM1();
	void SecondaryAttack(void) override;
	void ItemPostFrame(void) override;
	bool Holster(CBaseCombatWeapon* pSwitchingTo) override;

private:
	bool m_bInZoom;
};

//=============================================================================
// Section 4: AK-47 (SMG1-style base, no code changes)
//=============================================================================
class CWeaponAK47 : public CHLZ_SMG1Base
{
	DECLARE_CLASS(CWeaponAK47, CHLZ_SMG1Base);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 5: VP70 (Pistol-style base, no code changes)
//=============================================================================
class CWeaponVP70 : public CHLZ_PistolBase
{
	DECLARE_CLASS(CWeaponVP70, CHLZ_PistolBase);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 6: Fort-12 (Pistol-style base, no code changes)
//=============================================================================
class CWeaponFort12 : public CHLZ_PistolBase
{
	DECLARE_CLASS(CWeaponFort12, CHLZ_PistolBase);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 7: Desert Eagle (357-style base, no code changes)
//=============================================================================
class CWeaponDesertEagle : public CHLZ_357Base
{
	DECLARE_CLASS(CWeaponDesertEagle, CHLZ_357Base);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 8: M16 (SMG1-style base, no code changes)
//=============================================================================
class CWeaponM16 : public CHLZ_SMG1Base
{
	DECLARE_CLASS(CWeaponM16, CHLZ_SMG1Base);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 9: Combine Shotty (Shotgun-style base, no code changes)
//=============================================================================
class CWeaponCombineShotty : public CHLZ_ShotgunBase
{
	DECLARE_CLASS(CWeaponCombineShotty, CHLZ_ShotgunBase);
public:
	DECLARE_SERVERCLASS();
};

//=============================================================================
// Section 10: XM8 (AR2 copy, no alt-fire + crossbow zoom + green filter)
//=============================================================================
class CWeaponXM8 : public CWeaponAR2
{
	DECLARE_CLASS(CWeaponXM8, CWeaponAR2);
public:
	DECLARE_SERVERCLASS();
	DECLARE_DATADESC();

	CWeaponXM8();
	void SecondaryAttack(void) override;
	void ItemPostFrame(void) override;
	bool Holster(CBaseCombatWeapon* pSwitchingTo) override;
	void PrimaryAttack(void) override;

	void AddViewKick(void) override;

	// NPC: inherits the AR2 pose table and fire code; just remove the energy ball
	void Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator) override;
	int WeaponRangeAttack2Condition(float flDot, float flDist) override;

private:
	float m_flLastKickTime;
	bool m_bInZoom;
};

#endif // HLZ_WEAPONS_H