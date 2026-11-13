//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Standardized Mapbase-style Flashlight Implementation for EZ2 Mod
//
//===========================================================================//

#include "cbase.h"
#include "flashlighteffect.h"
#include "dlight.h"
#include "iefx.h"
#include "iviewrender.h"
#include "view.h"
#include "engine/ivdebugoverlay.h"
#include "tier0/vprof.h"
#include "tier1/KeyValues.h"
#include "toolframework_client.h"

#ifdef HL2_CLIENT_DLL
#include "c_basehlplayer.h"
#endif // HL2_CLIENT_DLL

#if defined( _X360 )
extern ConVar r_flashlightdepthres;
#else
extern ConVar r_flashlightdepthres;
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern ConVar r_flashlightdepthtexture;
extern ConCommand Ez_Nvg_On;

void r_newflashlightCallback_f(IConVar* pConVar, const char* pOldString, float flOldValue);

static ConVar r_newflashlight("r_newflashlight", "1", FCVAR_CHEAT, "", r_newflashlightCallback_f);
static ConVar r_swingflashlight("r_swingflashlight", "1", FCVAR_CHEAT);
static ConVar r_flashlightlockposition("r_flashlightlockposition", "0", FCVAR_CHEAT);
static ConVar r_flashlightfov("r_flashlightfov", "45.0", FCVAR_CHEAT);
static ConVar r_flashlightoffsetx("r_flashlightoffsetx", "10.0", FCVAR_CHEAT);
static ConVar r_flashlightoffsety("r_flashlightoffsety", "-20.0", FCVAR_CHEAT);
static ConVar r_flashlightoffsetz("r_flashlightoffsetz", "24.0", FCVAR_CHEAT);
static ConVar r_flashlightnear("r_flashlightnear", "4.0", FCVAR_CHEAT);
static ConVar r_flashlightfar("r_flashlightfar", "750.0", FCVAR_CHEAT);
static ConVar r_flashlightconstant("r_flashlightconstant", "0.0", FCVAR_CHEAT);
static ConVar r_flashlightlinear("r_flashlightlinear", "100.0", FCVAR_CHEAT);
static ConVar r_flashlightquadratic("r_flashlightquadratic", "0.0", FCVAR_CHEAT);
static ConVar r_flashlightvisualizetrace("r_flashlightvisualizetrace", "0", FCVAR_CHEAT);
static ConVar r_flashlightambient("r_flashlightambient", "0.0", FCVAR_CHEAT);
static ConVar r_flashlightshadowatten("r_flashlightshadowatten", "0.35", FCVAR_CHEAT);
static ConVar r_flashlightladderdist("r_flashlightladderdist", "40.0", FCVAR_CHEAT);

#ifndef MAPBASE
static ConVar mat_slopescaledepthbias_shadowmap("mat_slopescaledepthbias_shadowmap", "16", FCVAR_CHEAT);
static ConVar mat_depthbias_shadowmap("mat_depthbias_shadowmap", "0.0005", FCVAR_CHEAT);
#else
extern ConVarRef mat_slopescaledepthbias_shadowmap;
extern ConVarRef mat_depthbias_shadowmap;
#endif

#ifdef MAPBASE
static ConVar r_flashlighttextureoverride("r_flashlighttextureoverride", "", FCVAR_CHEAT);
#endif

void r_newflashlightCallback_f(IConVar* pConVar, const char* pOldString, float flOldValue)
{
	if (engine->GetDXSupportLevel() < 70)
	{
		r_newflashlight.SetValue(0);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Alternate constructor for Entropy : Zero matching Mapbase textures
//-----------------------------------------------------------------------------
CFlashlightEffect::CFlashlightEffect(int nEntIndex, flashlighttype type)
{
	m_FlashlightHandle = CLIENTSHADOW_INVALID_HANDLE;
	m_nEntIndex = nEntIndex;

	m_bIsOn = false;
	m_pPointLight = NULL;
	if (engine->GetDXSupportLevel() < 70)
	{
		r_newflashlight.SetValue(0);
	}

	m_iFlashLightType = type;

#ifdef MAPBASE
	if (r_flashlighttextureoverride.GetString()[0] != '\0')
	{
		m_FlashlightTexture.Init(r_flashlighttextureoverride.GetString(), TEXTURE_GROUP_OTHER, true);
	}
	else
#endif
		if (g_pMaterialSystemHardwareConfig->SupportsBorderColor())
			m_FlashlightTexture.Init("effects/flashlight_border", TEXTURE_GROUP_OTHER, true);
		else
			m_FlashlightTexture.Init("effects/flashlight001", TEXTURE_GROUP_OTHER, true);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CFlashlightEffect::~CFlashlightEffect()
{
	LightOff();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::TurnOn()
{
	m_bIsOn = true;
	m_flDistMod = 1.0f;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::TurnOff()
{
	if (m_bIsOn)
	{
		m_bIsOn = false;
		LightOff();
	}
}

// Custom trace filter that skips the player and the view model.
class CTraceFilterSkipPlayerAndViewModel : public CTraceFilter
{
public:
	virtual bool ShouldHitEntity(IHandleEntity* pServerEntity, int contentsMask)
	{
		C_BaseEntity* pEntity = EntityFromEntityHandle(pServerEntity);
		if (!pEntity)
			return true;

		if ((dynamic_cast<C_BaseViewModel*>(pEntity) != NULL) ||
			(dynamic_cast<C_BasePlayer*>(pEntity) != NULL) ||
			pEntity->GetCollisionGroup() == COLLISION_GROUP_DEBRIS ||
			pEntity->GetCollisionGroup() == COLLISION_GROUP_INTERACTIVE_DEBRIS)
		{
			return false;
		}

		return true;
	}
};

extern ConVar cl_stunstick_flashlight_distance;
extern ConVar cl_stunstick_flashlight_intensity;

//-----------------------------------------------------------------------------
// Purpose: Restored standard HL2/Mapbase projected light calculations
//-----------------------------------------------------------------------------
void CFlashlightEffect::UpdateLightNew(const Vector& vecPos, const Vector& vecForward, const Vector& vecRight, const Vector& vecUp)
{
	float fov, flashlightFar;

	if (m_iFlashLightType == MUZZLEFLASH)
	{
		fov = 179;
		flashlightFar = cl_stunstick_flashlight_distance.GetFloat();
	}
	else
	{
		fov = r_flashlightfov.GetFloat();
		flashlightFar = r_flashlightfar.GetFloat();
	}

	VPROF_BUDGET("CFlashlightEffect::UpdateLightNew", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);

	FlashlightState_t state;

	bool bPlayerOnLadder = (C_BasePlayer::GetLocalPlayer()->GetMoveType() == MOVETYPE_LADDER);

	const float flEpsilon = 0.1f;
	const float flDistCutoff = 128.0f;
	const float flDistDrag = 0.2;

	CTraceFilterSkipPlayerAndViewModel traceFilter;
	float flOffsetY = r_flashlightoffsety.GetFloat();

	if (r_swingflashlight.GetBool())
	{
		Vector vecSwingLight = vecPos + vecForward * -12.0f;
		if (vecSwingLight.z > vecPos.z)
		{
			flOffsetY += (vecSwingLight.z - vecPos.z);
		}
	}

	Vector vOrigin = vecPos + flOffsetY * vecUp;

	if (!bPlayerOnLadder)
	{
		trace_t pmOriginTrace;
		UTIL_TraceHull(vecPos, vOrigin, Vector(-4, -4, -4), Vector(4, 4, 4), MASK_SOLID & ~(CONTENTS_HITBOX), &traceFilter, &pmOriginTrace);

		if (pmOriginTrace.DidHit())
		{
			vOrigin = vecPos;
		}
	}
	else
	{
		vOrigin = vecPos;
	}

	int iMask = MASK_OPAQUE_AND_NPCS;
	iMask &= ~CONTENTS_HITBOX;
	iMask |= CONTENTS_WINDOW;

	Vector vTarget = vecPos + vecForward * flashlightFar;

	Vector vDir = vTarget - vOrigin;
	Vector vRight = vecRight;
	Vector vUp = vecUp;
	VectorNormalize(vDir);
	VectorNormalize(vRight);
	VectorNormalize(vUp);

	vUp -= DotProduct(vDir, vUp) * vDir;
	VectorNormalize(vUp);
	vRight -= DotProduct(vDir, vRight) * vDir;
	VectorNormalize(vRight);
	vRight -= DotProduct(vUp, vRight) * vUp;
	VectorNormalize(vRight);

	AssertFloatEquals(DotProduct(vDir, vRight), 0.0f, 1e-3);
	AssertFloatEquals(DotProduct(vDir, vUp), 0.0f, 1e-3);
	AssertFloatEquals(DotProduct(vRight, vUp), 0.0f, 1e-3);

	trace_t pmDirectionTrace;
	UTIL_TraceHull(vOrigin, vTarget, Vector(-4, -4, -4), Vector(4, 4, 4), iMask, &traceFilter, &pmDirectionTrace);

	if (r_flashlightvisualizetrace.GetBool() == true)
	{
		debugoverlay->AddBoxOverlay(pmDirectionTrace.endpos, Vector(-4, -4, -4), Vector(4, 4, 4), QAngle(0, 0, 0), 0, 0, 255, 16, 0);
		debugoverlay->AddLineOverlay(vOrigin, pmDirectionTrace.endpos, 255, 0, 0, false, 0);
	}

	float flDist = (pmDirectionTrace.endpos - vOrigin).Length();
	if (flDist < flDistCutoff)
	{
		float flPullBackDist = bPlayerOnLadder ? r_flashlightladderdist.GetFloat() : flDistCutoff - flDist;
		m_flDistMod = Lerp(flDistDrag, m_flDistMod, flPullBackDist);

		if (!bPlayerOnLadder)
		{
			trace_t pmBackTrace;
			UTIL_TraceHull(vOrigin, vOrigin - vDir * (flPullBackDist - flEpsilon), Vector(-4, -4, -4), Vector(4, 4, 4), iMask, &traceFilter, &pmBackTrace);
			if (pmBackTrace.DidHit())
			{
				float flMaxDist = (pmBackTrace.endpos - vOrigin).Length() - flEpsilon;
				if (m_flDistMod > flMaxDist)
					m_flDistMod = flMaxDist;
			}
		}
	}
	else
	{
		m_flDistMod = Lerp(flDistDrag, m_flDistMod, 0.0f);
	}
	vOrigin = vOrigin - vDir * m_flDistMod;

	state.m_vecLightOrigin = vOrigin;

	BasisToQuaternion(vDir, vRight, vUp, state.m_quatOrientation);

	state.m_fQuadraticAtten = r_flashlightquadratic.GetFloat();

	bool bFlicker = false;

#ifdef HL2_EPISODIC
	C_BaseHLPlayer* pPlayer = (C_BaseHLPlayer*)C_BasePlayer::GetLocalPlayer();
	if (pPlayer)
	{
		float flBatteryPower = (pPlayer->m_HL2Local.m_flFlashBattery >= 0.0f) ? (pPlayer->m_HL2Local.m_flFlashBattery) : pPlayer->m_HL2Local.m_flSuitPower;
		if (flBatteryPower <= 10.0f)
		{
			float flScale;
			if (flBatteryPower >= 0.0f)
			{
				flScale = (flBatteryPower <= 4.5f) ? SimpleSplineRemapVal(flBatteryPower, 4.5f, 0.0f, 1.0f, 0.0f) : 1.0f;
			}
			else
			{
				flScale = SimpleSplineRemapVal(flBatteryPower, 10.0f, 4.8f, 1.0f, 0.0f);
			}

			flScale = clamp(flScale, 0.0f, 1.0f);

			if (flScale < 0.35f)
			{
				float flFlicker = cosf(gpGlobals->curtime * 6.0f) * sinf(gpGlobals->curtime * 15.0f);
				if (flFlicker > 0.25f && flFlicker < 0.75f)
				{
					state.m_fLinearAtten = r_flashlightlinear.GetFloat() * flScale;
				}
				else
				{
					state.m_fLinearAtten = 0.0f;
				}
			}
			else
			{
				float flNoise = cosf(gpGlobals->curtime * 7.0f) * sinf(gpGlobals->curtime * 25.0f);
				state.m_fLinearAtten = r_flashlightlinear.GetFloat() * flScale + 1.5f * flNoise;
			}

			state.m_fHorizontalFOVDegrees = fov - (16.0f * (1.0f - flScale));
			state.m_fVerticalFOVDegrees = fov - (16.0f * (1.0f - flScale));

			bFlicker = true;
		}
	}
#endif // HL2_EPISODIC

	if (bFlicker == false)
	{
		state.m_fLinearAtten = r_flashlightlinear.GetFloat();
		state.m_fHorizontalFOVDegrees = fov;
		state.m_fVerticalFOVDegrees = fov;
	}

	state.m_fConstantAtten = r_flashlightconstant.GetFloat();

	state.m_Color[0] = m_iFlashLightType == MUZZLEFLASH ? cl_stunstick_flashlight_intensity.GetFloat() : 1.0f;
	state.m_Color[1] = m_iFlashLightType == MUZZLEFLASH ? cl_stunstick_flashlight_intensity.GetFloat() : 1.0f;
	state.m_Color[2] = m_iFlashLightType == MUZZLEFLASH ? cl_stunstick_flashlight_intensity.GetFloat() : 1.0f;

	state.m_Color[3] = r_flashlightambient.GetFloat();
	state.m_NearZ = r_flashlightnear.GetFloat() + m_flDistMod;
	state.m_FarZ = flashlightFar;

	// Shadows completely restored for default flashlights/headlamps matching standard global rules
	state.m_bEnableShadows = r_flashlightdepthtexture.GetBool();
	state.m_flShadowMapResolution = r_flashlightdepthres.GetInt();

	state.m_pSpotlightTexture = m_FlashlightTexture;
	state.m_nSpotlightTextureFrame = 0;

	state.m_flShadowAtten = r_flashlightshadowatten.GetFloat();
	state.m_flShadowSlopeScaleDepthBias = mat_slopescaledepthbias_shadowmap.GetFloat();
	state.m_flShadowDepthBias = mat_depthbias_shadowmap.GetFloat();

	if (m_FlashlightHandle == CLIENTSHADOW_INVALID_HANDLE)
	{
		m_FlashlightHandle = g_pClientShadowMgr->CreateFlashlight(state);
	}
	else
	{
		if (!r_flashlightlockposition.GetBool())
		{
			g_pClientShadowMgr->UpdateFlashlightState(m_FlashlightHandle, state);
		}
	}

	g_pClientShadowMgr->UpdateProjectedTexture(m_FlashlightHandle, true);
	LightOffOld();

#ifndef NO_TOOLFRAMEWORK
	if (clienttools->IsInRecordingMode())
	{
		KeyValues* msg = new KeyValues("FlashlightState");
		msg->SetFloat("time", gpGlobals->curtime);
		msg->SetInt("entindex", m_nEntIndex);
		msg->SetInt("flashlightHandle", m_FlashlightHandle);
		msg->SetPtr("flashlightState", &state);
		ToolFramework_PostToolMessage(HTOOLHANDLE_INVALID, msg);
		msg->deleteThis();
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::UpdateLightOld(const Vector& vecPos, const Vector& vecDir, int nDistance)
{
	if (!m_pPointLight || (m_pPointLight->key != m_nEntIndex))
	{
		m_pPointLight = effects->CL_AllocDlight(m_nEntIndex);
		m_pPointLight->flags = 0.0f;
		m_pPointLight->radius = 80;
	}

	VectorCopy(vecDir, m_pPointLight->m_Direction);

	Vector end;
	end = vecPos + nDistance * vecDir;

	trace_t pm;
	CTraceFilterSkipPlayerAndViewModel traceFilter;
	UTIL_TraceLine(vecPos, end, MASK_ALL, &traceFilter, &pm);
	VectorCopy(pm.endpos, m_pPointLight->origin);

	float falloff = pm.fraction * nDistance;
	if (falloff < 500)
		falloff = 1.0;
	else
		falloff = 500.0 / falloff;

	falloff *= falloff;

	m_pPointLight->radius = 80;
	m_pPointLight->color.r = m_pPointLight->color.g = m_pPointLight->color.b = 255 * falloff;
	m_pPointLight->color.exponent = 0;
	m_pPointLight->die = gpGlobals->curtime + 0.2f;

	render->TouchLight(m_pPointLight);
	LightOffNew();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::UpdateLight(const Vector& vecPos, const Vector& vecDir, const Vector& vecRight, const Vector& vecUp, int nDistance)
{
	if (!m_bIsOn)
	{
		return;
	}
	if (r_newflashlight.GetBool())
	{
		UpdateLightNew(vecPos, vecDir, vecRight, vecUp);
	}
	else
	{
		UpdateLightOld(vecPos, vecDir, nDistance);
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::LightOffNew()
{
#ifndef NO_TOOLFRAMEWORK
	if (clienttools->IsInRecordingMode())
	{
		KeyValues* msg = new KeyValues("FlashlightState");
		msg->SetFloat("time", gpGlobals->curtime);
		msg->SetInt("entindex", m_nEntIndex);
		msg->SetInt("flashlightHandle", m_FlashlightHandle);
		msg->SetPtr("flashlightState", NULL);
		ToolFramework_PostToolMessage(HTOOLHANDLE_INVALID, msg);
		msg->deleteThis();
	}
#endif

	if (m_FlashlightHandle != CLIENTSHADOW_INVALID_HANDLE)
	{
		g_pClientShadowMgr->DestroyFlashlight(m_FlashlightHandle);
		m_FlashlightHandle = CLIENTSHADOW_INVALID_HANDLE;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::LightOffOld()
{
	if (m_pPointLight && (m_pPointLight->key == m_nEntIndex))
	{
		m_pPointLight->die = gpGlobals->curtime;
		m_pPointLight = NULL;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFlashlightEffect::LightOff()
{
	LightOffOld();
	LightOffNew();
}

#ifdef EZ
static ConVar r_headlightfov("r_headlightfov", "90.0", FCVAR_CHEAT);
static ConVar r_headlightfar("r_headlightfar", "1250.0", FCVAR_CHEAT);
#endif

CHeadlightEffect::CHeadlightEffect()
{
}

CHeadlightEffect::~CHeadlightEffect()
{
}

void CHeadlightEffect::UpdateLight(const Vector& vecPos, const Vector& vecDir, const Vector& vecRight, const Vector& vecUp, int nDistance)
{
	if (IsOn() == false)
		return;

	FlashlightState_t state;
	Vector basisX, basisY, basisZ;
	basisX = vecDir;
	basisY = vecRight;
	basisZ = vecUp;
	VectorNormalize(basisX);
	VectorNormalize(basisY);
	VectorNormalize(basisZ);

	BasisToQuaternion(basisX, basisY, basisZ, state.m_quatOrientation);

	state.m_vecLightOrigin = vecPos;

#ifdef EZ
	state.m_fHorizontalFOVDegrees = r_headlightfov.GetFloat();
	state.m_fVerticalFOVDegrees = r_headlightfov.GetFloat();
#else
	state.m_fHorizontalFOVDegrees = 45.0f;
	state.m_fVerticalFOVDegrees = 30.0f;
#endif
	state.m_fQuadraticAtten = r_flashlightquadratic.GetFloat();
	state.m_fLinearAtten = r_flashlightlinear.GetFloat();
	state.m_fConstantAtten = r_flashlightconstant.GetFloat();
	// Always plain white, like stock HL2
	state.m_Color[0] = 1.0f;
	state.m_Color[1] = 1.0f;
	state.m_Color[2] = 1.0f;
	state.m_Color[3] = r_flashlightambient.GetFloat();
	state.m_NearZ = r_flashlightnear.GetFloat();
#ifdef EZ
	state.m_FarZ = r_headlightfar.GetFloat();
#else
	state.m_FarZ = r_flashlightfar.GetFloat();
#endif
	state.m_bEnableShadows = true;
	state.m_pSpotlightTexture = m_FlashlightTexture;
	state.m_nSpotlightTextureFrame = 0;

	if (GetFlashlightHandle() == CLIENTSHADOW_INVALID_HANDLE)
	{
		SetFlashlightHandle(g_pClientShadowMgr->CreateFlashlight(state));
	}
	else
	{
		g_pClientShadowMgr->UpdateFlashlightState(GetFlashlightHandle(), state);
	}

	g_pClientShadowMgr->UpdateProjectedTexture(GetFlashlightHandle(), true);
}

#ifdef EZ2
static ConVar r_turretlightbrightness("r_turretlightbrightness", "5.0", FCVAR_CHEAT);
static ConVar r_turretlightfar("r_turretlightfar", "500", FCVAR_CHEAT);
static ConVar r_turretlightnear("r_turretlightnear", "1.0", FCVAR_CHEAT);
static ConVar r_turretlightconstant("r_turretlightconstant", "0.0", FCVAR_CHEAT);
static ConVar r_turretlightlinear("r_turretlightlinear", "0.0", FCVAR_CHEAT);
static ConVar r_turretlightquadratic("r_turretlightquadratic", "100.0", FCVAR_CHEAT);
static ConVar r_turretlightfov("r_turretlightfov", "60.0", FCVAR_CHEAT);
static ConVar r_turretlightshadowsenabled("r_turretlightshadowsenabled", "0", FCVAR_CHEAT);
static ConVar r_turretlightshadowatten("r_turretlightshadowatten", "1.0", FCVAR_CHEAT);
static ConVar r_turretlighttexture("r_turretlighttexture", "effects/turret_light", FCVAR_CHEAT);

CTurretLightEffect::CTurretLightEffect()
{
	m_FlashlightTexture.Init(r_turretlighttexture.GetString(), TEXTURE_GROUP_OTHER, true);
	m_flBrightnessScale = 1.0f;
	m_flFarZ = r_turretlightfar.GetFloat();
	m_flFOV = r_turretlightfov.GetFloat();

	m_Color[0] = 1.0f;
	m_Color[1] = 0.0f;
	m_Color[2] = 0.0f;
}

CTurretLightEffect::~CTurretLightEffect()
{
}

void CTurretLightEffect::UpdateLight(const Vector& vecPos, const Vector& vecDir, const Vector& vecRight, const Vector& vecUp, int nDistance)
{
	if (IsOn() == false)
		return;

	FlashlightState_t state;
	Vector basisX, basisY, basisZ;
	basisX = vecDir;
	basisY = vecRight;
	basisZ = vecUp;
	VectorNormalize(basisX);
	VectorNormalize(basisY);
	VectorNormalize(basisZ);

	BasisToQuaternion(basisX, basisY, basisZ, state.m_quatOrientation);

	state.m_vecLightOrigin = vecPos;

	state.m_fHorizontalFOVDegrees = m_flFOV;
	state.m_fVerticalFOVDegrees = m_flFOV;
	state.m_fQuadraticAtten = r_turretlightquadratic.GetFloat();
	state.m_fLinearAtten = r_turretlightlinear.GetFloat();
	state.m_fConstantAtten = r_turretlightconstant.GetFloat();

	float brightness = (m_flBrightnessScale != 1.0f) ? r_turretlightbrightness.GetFloat() * m_flBrightnessScale : r_turretlightbrightness.GetFloat();
	state.m_Color[0] = m_Color[0] * brightness;
	state.m_Color[1] = m_Color[1] * brightness;
	state.m_Color[2] = m_Color[2] * brightness;
	state.m_Color[3] = r_flashlightambient.GetFloat();

	state.m_NearZ = r_turretlightnear.GetFloat();
	state.m_FarZ = m_flFarZ;

	state.m_bEnableShadows = r_turretlightshadowsenabled.GetBool();
	state.m_pSpotlightTexture = m_FlashlightTexture;
	state.m_nSpotlightTextureFrame = 0;

	state.m_flShadowAtten = r_turretlightshadowatten.GetFloat();
	state.m_flShadowSlopeScaleDepthBias = mat_slopescaledepthbias_shadowmap.GetFloat();
	state.m_flShadowDepthBias = mat_depthbias_shadowmap.GetFloat();

	if (GetFlashlightHandle() == CLIENTSHADOW_INVALID_HANDLE)
	{
		SetFlashlightHandle(g_pClientShadowMgr->CreateFlashlight(state));
	}
	else
	{
		g_pClientShadowMgr->UpdateFlashlightState(GetFlashlightHandle(), state);
	}

	g_pClientShadowMgr->UpdateProjectedTexture(GetFlashlightHandle(), true);
}

//-----------------------------------------------------------------------------
// Crab Synth light effect
//-----------------------------------------------------------------------------
static ConVar r_crabsynthlightbrightness("r_crabsynthlightbrightness", "255.0", FCVAR_CHEAT);
static ConVar r_crabsynthlightfar("r_crabsynthlightfar", "400", FCVAR_CHEAT);
static ConVar r_crabsynthlightnear("r_crabsynthlightnear", "2.0", FCVAR_CHEAT);
static ConVar r_crabsynthlightconstant("r_crabsynthlightconstant", "0.0", FCVAR_CHEAT);
static ConVar r_crabsynthlightlinear("r_crabsynthlightlinear", "0.0", FCVAR_CHEAT);
static ConVar r_crabsynthlightquadratic("r_crabsynthlightquadratic", "100.0", FCVAR_CHEAT);
static ConVar r_crabsynthlightshadowatten("r_crabsynthlightshadowatten", "1.0", FCVAR_CHEAT);
static ConVar r_crabsynthlighttexture("r_crabsynthlighttexture", "effects/crabsynthlight", FCVAR_CHEAT);

CCrabSynthLightEffect::CCrabSynthLightEffect()
{
	m_FlashlightTexture.Init(r_crabsynthlighttexture.GetString(), TEXTURE_GROUP_OTHER, true);
	m_flBrightnessScale = 1.0f;

	m_Color[0] = 1.0f;
	m_Color[1] = 1.0f;
	m_Color[2] = 1.0f;
}

CCrabSynthLightEffect::~CCrabSynthLightEffect()
{
}

void CCrabSynthLightEffect::UpdateLight(const Vector& vecPos, const Vector& vecDir, const Vector& vecRight, const Vector& vecUp, int nDistance)
{
	if (IsOn() == false)
		return;

	FlashlightState_t state;
	Vector basisX, basisY, basisZ;
	basisX = vecDir;
	basisY = vecRight;
	basisZ = vecUp;
	VectorNormalize(basisX);
	VectorNormalize(basisY);
	VectorNormalize(basisZ);

	BasisToQuaternion(basisX, basisY, basisZ, state.m_quatOrientation);

	state.m_vecLightOrigin = vecPos;

	state.m_fHorizontalFOVDegrees = m_flHorzFOV;
	state.m_fVerticalFOVDegrees = m_flFOV;
	state.m_fQuadraticAtten = r_crabsynthlightquadratic.GetFloat();
	state.m_fLinearAtten = r_crabsynthlightlinear.GetFloat();
	state.m_fConstantAtten = r_crabsynthlightconstant.GetFloat();

	float brightness = (m_flBrightnessScale != 1.0f) ? r_crabsynthlightbrightness.GetFloat() * m_flBrightnessScale : r_crabsynthlightbrightness.GetFloat();
	state.m_Color[0] = m_Color[0] * brightness;
	state.m_Color[1] = m_Color[1] * brightness;
	state.m_Color[2] = m_Color[2] * brightness;
	state.m_Color[3] = r_flashlightambient.GetFloat();

	state.m_NearZ = r_crabsynthlightnear.GetFloat();
	state.m_FarZ = r_crabsynthlightfar.GetFloat();

	state.m_bEnableShadows = m_bShadowsEnabled;
	state.m_pSpotlightTexture = m_FlashlightTexture;
	state.m_nSpotlightTextureFrame = 0;

	state.m_flShadowAtten = r_crabsynthlightshadowatten.GetFloat();
	state.m_flShadowSlopeScaleDepthBias = mat_slopescaledepthbias_shadowmap.GetFloat();
	state.m_flShadowDepthBias = mat_depthbias_shadowmap.GetFloat();

	if (GetFlashlightHandle() == CLIENTSHADOW_INVALID_HANDLE)
	{
		SetFlashlightHandle(g_pClientShadowMgr->CreateFlashlight(state));
	}
	else
	{
		g_pClientShadowMgr->UpdateFlashlightState(GetFlashlightHandle(), state);
	}

	g_pClientShadowMgr->UpdateProjectedTexture(GetFlashlightHandle(), true);
}
#endif
