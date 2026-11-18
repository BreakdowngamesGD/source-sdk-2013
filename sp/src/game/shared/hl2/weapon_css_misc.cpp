//=============================================================================//
//
// Purpose: CS:S weapons recreated from scratch in Source SDK 2013 for usage in a Half-Life 2 setting.
//
// Author: Blixibon
//
//=============================================================================//

#include "cbase.h"
#include "weapon_css_base.h"
#ifdef CLIENT_DLL
#include "c_te_effect_dispatch.h"
#else
#include "te_effect_dispatch.h"
#endif
//-----------------------------------------------------------------------------
// CWeapon_CSS_HL2_M249
//-----------------------------------------------------------------------------
class CWeapon_CSS_HL2_M249 : public CBase_CSS_HL2_Rifle
{
public:
	DECLARE_CLASS(CWeapon_CSS_HL2_M249, CBase_CSS_HL2_Rifle);
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();
	DECLARE_DATADESC();

	CWeapon_CSS_HL2_M249(void);

	virtual CSS_HL2_WeaponActClass		GetCSSWeaponActClass() { return CSSHL2_WEAPON_AR1; }

	virtual float GetViewKickBase() { return 0.0f; }

	// AR2-style Combine tracers, NPC-fired shots only.
	// Players keep the weapon's default tracer.
	virtual const char* GetTracerType(void)
	{
		CBaseCombatCharacter* pOwner = GetOwner();
		if (pOwner && pOwner->IsNPC())
			return "AR2Tracer";

		return BaseClass::GetTracerType();
	}

	// AR2-style impact effect, NPC-fired shots only
	virtual void DoImpactEffect(trace_t& tr, int nDamageType)
	{
		CBaseCombatCharacter* pOwner = GetOwner();
		if (pOwner && pOwner->IsNPC())
		{
			CEffectData data;
			data.m_vOrigin = tr.endpos + (tr.plane.normal * 1.0f);
			data.m_vNormal = tr.plane.normal;
			DispatchEffect("AR2Impact", data);
		}

		BaseClass::DoImpactEffect(tr, nDamageType);
	}

	virtual const Vector& GetBulletSpread(void)
	{
		static const Vector cone = VECTOR_CONE_5DEGREES;
		return cone;
	}

	virtual int	GetMinBurst() { return 7; }
	virtual int	GetMaxBurst() { return 10; }

	virtual float GetFireRate(void) { return 0.08f; }
};

IMPLEMENT_NETWORKCLASS_DT( CWeapon_CSS_HL2_M249, DT_Weapon_CSS_HL2_M249 )
END_NETWORK_TABLE()

LINK_ENTITY_TO_CLASS( weapon_css_m249, CWeapon_CSS_HL2_M249 );
#if PRECACHE_REGISTER_CSS_WEAPONS == 1
PRECACHE_WEAPON_REGISTER( weapon_css_m249 );
#endif

BEGIN_DATADESC( CWeapon_CSS_HL2_M249 )
END_DATADESC()

#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA( CWeapon_CSS_HL2_M249 )
END_PREDICTION_DATA()
#endif

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CWeapon_CSS_HL2_M249::CWeapon_CSS_HL2_M249( void )
{
	m_fMinRange1		= 24;
	m_fMaxRange1		= 1500;
	m_fMinRange2		= 24;
	m_fMaxRange2		= 200;

	m_bFiresUnderwater	= true;
}
