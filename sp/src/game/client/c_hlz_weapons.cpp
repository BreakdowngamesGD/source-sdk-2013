#include "cbase.h"
#include "c_weapon__stubs.h"
#include "c_basehlcombatweapon.h"
#include "c_baseplayer.h"
#include "iclientmode.h"
#include "hud.h"
#include "hudelement.h"
#include "vgui_controls/Panel.h"
#include "vgui/ISurface.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//=============================================================================
// Client Base Stubs (matching CHLZ_* server base classes)
//=============================================================================
STUB_WEAPON_CLASS(hlz_smg1_base, HLZ_SMG1Base, C_HLSelectFireMachineGun);
STUB_WEAPON_CLASS(hlz_pistol_base, HLZ_PistolBase, C_HLMachineGun);
STUB_WEAPON_CLASS(hlz_357_base, HLZ_357Base, C_BaseHLCombatWeapon);
STUB_WEAPON_CLASS(hlz_shotgun_base, HLZ_ShotgunBase, C_BaseHLCombatWeapon);

//=============================================================================
// Client Weapon Registrations (weapon_cmg moved from c_weapon__stubs_hl2.cpp)
//=============================================================================
STUB_WEAPON_CLASS(weapon_cmg, WeaponCMG, C_HLMachineGun);
STUB_WEAPON_CLASS(weapon_nikita_launcher, WeaponNikitaLauncher, C_BaseHLCombatWeapon);
STUB_WEAPON_CLASS(weapon_ak47, WeaponAK47, C_HLSelectFireMachineGun);
STUB_WEAPON_CLASS(weapon_vp70, WeaponVP70, C_HLMachineGun);
STUB_WEAPON_CLASS(weapon_fort12, WeaponFort12, C_HLMachineGun);
STUB_WEAPON_CLASS(weapon_desert_eagle, WeaponDesertEagle, C_BaseHLCombatWeapon);
STUB_WEAPON_CLASS(weapon_m16, WeaponM16, C_HLSelectFireMachineGun);
STUB_WEAPON_CLASS(weapon_combine_shotty, WeaponCombineShotty, C_BaseHLCombatWeapon);
STUB_WEAPON_CLASS(weapon_xm8, WeaponXM8, C_HLMachineGun);            // server base is CWeaponAR2
STUB_WEAPON_CLASS(weapon_ar_m1, WeaponARM1, C_HLSelectFireMachineGun);  // same as ak47/m16

//=============================================================================
// XM8 Client Class & Code-Drawn Green Zoom Filter
//=============================================================================
class CHudXM8GreenFilter : public vgui::Panel, public CHudElement
{
	DECLARE_CLASS_SIMPLE(CHudXM8GreenFilter, vgui::Panel);
public:
	CHudXM8GreenFilter(const char* pElementName) : CHudElement(pElementName), BaseClass(NULL, "XM8GreenFilter")
	{
		vgui::Panel* pParent = g_pClientMode ? g_pClientMode->GetViewport() : NULL;
		SetParent(pParent);
		SetPaintBackgroundEnabled(false);
		SetMouseInputEnabled(false);
	}

	void ApplySchemeSettings(vgui::IScheme* pScheme) override
	{
		BaseClass::ApplySchemeSettings(pScheme);
		SetBounds(0, 0, ScreenWidth(), ScreenHeight());
	}

	bool ShouldDraw() override
	{
		C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
		if (!pPlayer || !pPlayer->IsAlive())
			return false;

		C_BaseCombatWeapon* pWeapon = pPlayer->GetActiveWeapon();
		if (!pWeapon || !FStrEq(pWeapon->GetClassname(), "weapon_xm8"))
			return false;

		return pPlayer->GetFOV() < pPlayer->GetDefaultFOV();
	}

	void Paint() override
	{
		int w, h;
		GetSize(w, h);
		vgui::surface()->DrawSetColor(40, 255, 80, 55);
		vgui::surface()->DrawFilledRect(0, 0, w, h);
	}
};

DECLARE_HUDELEMENT(CHudXM8GreenFilter);