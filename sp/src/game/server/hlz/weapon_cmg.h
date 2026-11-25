//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: CMG (Combine Machine Gun) - the Suppressor's heavy support weapon.
//			Built on the AR2: same animations, tracers and impact effects, but
//			a big clip, no energy ball, loose spread and long NPC bursts.
//
//=============================================================================//

#ifndef WEAPON_CMG_H
#define WEAPON_CMG_H

#include "weapon_ar2.h"

class CWeaponCMG : public CWeaponAR2
{
public:
	DECLARE_CLASS(CWeaponCMG, CWeaponAR2);

	CWeaponCMG();

	DECLARE_SERVERCLASS();
	DECLARE_DATADESC();

	// Player fire: plain machine-gun behavior, skipping the AR2's EZ1 override
	void	PrimaryAttack(void);

	// No energy ball
	void	SecondaryAttack(void) {}

	void	AddViewKick(void);

	// NPC fire
	void	FireNPCPrimaryAttack(CBaseCombatCharacter* pOperator, bool bUseWeaponAngles);
	void	FireNPCSecondaryAttack(CBaseCombatCharacter* pOperator, bool bUseWeaponAngles) {}
	void	Operator_HandleAnimEvent(animevent_t* pEvent, CBaseCombatCharacter* pOperator);

	// Burst length (rounds per trigger pull) and cycle rate for NPCs
	int		GetMinBurst(void);
	int		GetMaxBurst(void);
	float	GetFireRate(void);

	virtual const Vector& GetBulletSpread(void)
	{
		static Vector cone = VECTOR_CONE_4DEGREES;
		return cone;
	}

	const WeaponProficiencyInfo_t* GetProficiencyValues();
};

#endif // WEAPON_CMG_H