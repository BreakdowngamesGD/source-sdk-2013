//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Self-contained player & NPC mountable emplacement gun entity
//          (NPCs navigate to the man-point instead of teleporting).
//
//=============================================================================//

#include "cbase.h"
#include "player.h"
#include "ai_basenpc.h"
#include "ai_schedule.h"
#include "in_buttons.h"
#include "ammodef.h"
#include "ndebugoverlay.h"
#include "engine/IEngineSound.h"
#include "tier0/memdbgon.h"

#define EMPLACEMENT_DEFAULT_MODEL "models/props_combine/bunker_gun01.mdl" // Replace with your emplacement .mdl
#define EMPLACEMENT_MUZZLE_ATTACHMENT "muzzle" // Replace with your model's attachment name
#define EMPLACEMENT_USE_DISTANCE 96.0f
#define EMPLACEMENT_MAN_DISTANCE 36.0f
#define EMPLACEMENT_NPC_SEARCH_RADIUS 512.0f

//-----------------------------------------------------------------------------
// Purpose: Self-contained emplacement gun for players and NPCs
//-----------------------------------------------------------------------------
class CPropEmplacement : public CBaseAnimating
{
	DECLARE_CLASS(CPropEmplacement, CBaseAnimating);
	DECLARE_DATADESC();

public:
	CPropEmplacement();

	void Precache() override;
	void Spawn() override;
	void Use(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value) override;
	void Think() override;

	int ObjectCaps() override { return BaseClass::ObjectCaps() | FCAP_IMPULSE_USE; }

	// Hammer inputs
	void InputLock(inputdata_t& inputdata);
	void InputUnlock(inputdata_t& inputdata);
	void InputForceMount(inputdata_t& inputdata);
	void InputForceUnmount(inputdata_t& inputdata);

private:
	bool IsOccupied() const { return m_hUser.Get() != NULL || m_hNPCUser.Get() != NULL; }
	bool IsDirectionInRange(const Vector& vecTargetPos, float* pOutYaw = NULL, float* pOutPitch = NULL) const;

	// Player mounting & control
	bool MountPlayer(CBasePlayer* pPlayer);
	void UnmountPlayer(bool bResetUseEntity = true);
	void UpdatePlayerAimAndFiring(CBasePlayer* pPlayer);

	// NPC manning & control (no teleporting)
	void FindAndAssignNPC();
	bool CanNPCAssign(CAI_BaseNPC* pNPC) const;
	bool AssignNPC(CAI_BaseNPC* pNPC);
	void ReleaseNPC();
	void UpdateNPCGunner(CAI_BaseNPC* pNPC);

	// Shared firing & pose helpers
	void ApplyAimPoseParameters(float flYaw, float flPitch);
	void FireEmplacement(CBaseEntity* pAttacker, const Vector& vecStart, const Vector& vecDir);
	void GetMuzzleTransform(Vector& vecPos, QAngle& vecAngles);
	Vector GetNPCManPoint() const;

private:
	CHandle<CBasePlayer> m_hUser;
	EHANDLE m_hPrevUseEntity;
	MoveType_t m_nPrevPlayerMoveType;

	CHandle<CAI_BaseNPC> m_hNPCUser;
	MoveType_t m_nPrevNPCMoveType;
	bool m_bNPCInRoute;
	float m_flNPCNextScanTime;

	string_t m_iszModelName;
	string_t m_iszFireSound;
	float m_flFireRate;
	float m_flYawRange;
	float m_flPitchUpRange;
	float m_flPitchDownRange;
	int m_iAmmoType;
	int m_iDamage;
	bool m_bLocked;

	float m_flNextFireTime;
	float m_flExitCooldown;

	COutputEvent m_OnMounted;
	COutputEvent m_OnUnmounted;
	COutputEvent m_OnFire;
};

LINK_ENTITY_TO_CLASS(prop_emplacement, CPropEmplacement);

BEGIN_DATADESC(CPropEmplacement)
DEFINE_FIELD(m_hUser, FIELD_EHANDLE),
DEFINE_FIELD(m_hPrevUseEntity, FIELD_EHANDLE),
DEFINE_FIELD(m_nPrevPlayerMoveType, FIELD_INTEGER),

DEFINE_FIELD(m_hNPCUser, FIELD_EHANDLE),
DEFINE_FIELD(m_nPrevNPCMoveType, FIELD_INTEGER),
DEFINE_FIELD(m_bNPCInRoute, FIELD_BOOLEAN),
DEFINE_FIELD(m_flNPCNextScanTime, FIELD_TIME),

DEFINE_KEYFIELD(m_iszModelName, FIELD_STRING, "model"),
DEFINE_KEYFIELD(m_iszFireSound, FIELD_SOUNDNAME, "firesound"),
DEFINE_KEYFIELD(m_flFireRate, FIELD_FLOAT, "firerate"),
DEFINE_KEYFIELD(m_flYawRange, FIELD_FLOAT, "yawrange"),
DEFINE_KEYFIELD(m_flPitchUpRange, FIELD_FLOAT, "pitchuprange"),
DEFINE_KEYFIELD(m_flPitchDownRange, FIELD_FLOAT, "pitchdownrange"),
DEFINE_KEYFIELD(m_iDamage, FIELD_INTEGER, "damage"),
DEFINE_KEYFIELD(m_bLocked, FIELD_BOOLEAN, "locked"),

DEFINE_FIELD(m_iAmmoType, FIELD_INTEGER),
DEFINE_FIELD(m_flNextFireTime, FIELD_TIME),
DEFINE_FIELD(m_flExitCooldown, FIELD_TIME),

DEFINE_INPUTFUNC(FIELD_VOID, "Lock", InputLock),
DEFINE_INPUTFUNC(FIELD_VOID, "Unlock", InputUnlock),
DEFINE_INPUTFUNC(FIELD_VOID, "ForceMount", InputForceMount),
DEFINE_INPUTFUNC(FIELD_VOID, "ForceUnmount", InputForceUnmount),

DEFINE_OUTPUT(m_OnMounted, "OnMounted"),
DEFINE_OUTPUT(m_OnUnmounted, "OnUnmounted"),
DEFINE_OUTPUT(m_OnFire, "OnFire"),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CPropEmplacement::CPropEmplacement()
{
	m_hUser = NULL;
	m_hPrevUseEntity = NULL;
	m_nPrevPlayerMoveType = MOVETYPE_WALK;

	m_hNPCUser = NULL;
	m_nPrevNPCMoveType = MOVETYPE_STEP;
	m_bNPCInRoute = false;
	m_flNPCNextScanTime = 0.0f;

	m_iszModelName = NULL_STRING;
	m_iszFireSound = NULL_STRING;
	m_flFireRate = 0.12f;
	m_flYawRange = 60.0f;
	m_flPitchUpRange = 35.0f;
	m_flPitchDownRange = 25.0f;
	m_iAmmoType = -1;
	m_iDamage = 18;
	m_bLocked = false;

	m_flNextFireTime = 0.0f;
	m_flExitCooldown = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose: Precache model and sound resources
//-----------------------------------------------------------------------------
void CPropEmplacement::Precache()
{
	BaseClass::Precache();

	const char* pszModel = (m_iszModelName != NULL_STRING) ? STRING(m_iszModelName) : EMPLACEMENT_DEFAULT_MODEL;
	PrecacheModel(pszModel);

	if (m_iszFireSound != NULL_STRING)
	{
		PrecacheScriptSound(STRING(m_iszFireSound));
	}
	else
	{
		PrecacheScriptSound("FuncTank.Fire");
	}

	m_iAmmoType = GetAmmoDef()->Index("AR2");
	if (m_iAmmoType < 0)
	{
		m_iAmmoType = GetAmmoDef()->Index("SMG1");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Initialize solid model and think loop
//-----------------------------------------------------------------------------
void CPropEmplacement::Spawn()
{
	Precache();

	const char* pszModel = (m_iszModelName != NULL_STRING) ? STRING(m_iszModelName) : EMPLACEMENT_DEFAULT_MODEL;
	SetModel(pszModel);

	SetMoveType(MOVETYPE_NONE);
	SetSolid(SOLID_VPHYSICS);
	VPhysicsInitStatic();

	SetThink(&CPropEmplacement::Think);
	SetNextThink(gpGlobals->curtime + 0.05f);
}

//-----------------------------------------------------------------------------
// Purpose: Compute man-point behind the gun for NPCs to walk to
//-----------------------------------------------------------------------------
Vector CPropEmplacement::GetNPCManPoint() const
{
	Vector vecForward;
	AngleVectors(GetAbsAngles(), &vecForward);
	return GetAbsOrigin() - vecForward * 32.0f;
}

//-----------------------------------------------------------------------------
// Purpose: Check whether a world target point falls within the gun's yaw/pitch arc
//-----------------------------------------------------------------------------
bool CPropEmplacement::IsDirectionInRange(const Vector& vecTargetPos, float* pOutYaw, float* pOutPitch) const
{
	Vector vecOrigin;
	QAngle vecAngles;
	const_cast<CPropEmplacement*>(this)->GetMuzzleTransform(vecOrigin, vecAngles);

	Vector vecToTarget = vecTargetPos - vecOrigin;
	if (vecToTarget.IsLengthLessThan(1.0f))
		return false;

	QAngle angToTarget;
	VectorAngles(vecToTarget, angToTarget);

	QAngle angBase = GetAbsAngles();
	float flDeltaYaw = AngleDiff(angToTarget.y, angBase.y);
	float flPitch = AngleNormalize(angToTarget.x);

	bool bYawOk = (fabsf(flDeltaYaw) <= m_flYawRange);
	bool bPitchOk = (flPitch >= -m_flPitchUpRange && flPitch <= m_flPitchDownRange);

	if (pOutYaw)
	{
		*pOutYaw = clamp(flDeltaYaw, -m_flYawRange, m_flYawRange);
	}
	if (pOutPitch)
	{
		*pOutPitch = clamp(flPitch, -m_flPitchUpRange, m_flPitchDownRange);
	}

	return (bYawOk && bPitchOk);
}

//-----------------------------------------------------------------------------
// Purpose: Handle player +use to mount/unmount (players can take over from an NPC)
//-----------------------------------------------------------------------------
void CPropEmplacement::Use(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value)
{
	CBasePlayer* pPlayer = ToBasePlayer(pActivator);
	if (!pPlayer)
		return;

	if (m_hUser.Get() == pPlayer)
	{
		if (useType == USE_OFF || gpGlobals->curtime >= m_flExitCooldown)
		{
			UnmountPlayer(true);
		}
		return;
	}

	if (useType == USE_OFF || m_bLocked)
		return;

	// If an NPC is assigned or en-route, release it so the player can take over
	if (m_hNPCUser.Get() != NULL)
	{
		ReleaseNPC();
	}

	if (m_hUser.Get() != NULL)
		return;

	float flDistSq = GetAbsOrigin().DistToSqr(pPlayer->GetAbsOrigin());
	if (flDistSq > Square(EMPLACEMENT_USE_DISTANCE * 1.5f))
		return;

	MountPlayer(pPlayer);
}

//-----------------------------------------------------------------------------
// Purpose: Attach player to the emplacement
//-----------------------------------------------------------------------------
bool CPropEmplacement::MountPlayer(CBasePlayer* pPlayer)
{
	if (!pPlayer || !pPlayer->IsAlive() || m_bLocked || IsOccupied())
		return false;

	m_hUser = pPlayer;
	m_hPrevUseEntity = pPlayer->GetUseEntity();
	m_nPrevPlayerMoveType = pPlayer->GetMoveType();

	pPlayer->SetUseEntity(this);
	pPlayer->SetMoveType(MOVETYPE_NONE);
	pPlayer->AddEFlags(EFL_NO_DISSOLVE);
	pPlayer->SetPhysicsFlag(PFLAG_USING, true);

	Vector vecMountPos = GetNPCManPoint();
	QAngle angGun = GetAbsAngles();
	pPlayer->Teleport(&vecMountPos, &angGun, &vec3_origin);
	pPlayer->SnapEyeAngles(angGun);

	m_flExitCooldown = gpGlobals->curtime + 0.35f;
	m_OnMounted.FireOutput(pPlayer, this);
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Release player control
//-----------------------------------------------------------------------------
void CPropEmplacement::UnmountPlayer(bool bResetUseEntity)
{
	CBasePlayer* pPlayer = m_hUser.Get();
	m_hUser = NULL;

	if (pPlayer)
	{
		pPlayer->SetPhysicsFlag(PFLAG_USING, false);
		pPlayer->RemoveEFlags(EFL_NO_DISSOLVE);
		pPlayer->SetMoveType(m_nPrevPlayerMoveType != MOVETYPE_NONE ? m_nPrevPlayerMoveType : MOVETYPE_WALK);

		if (bResetUseEntity && pPlayer->GetUseEntity() == this)
		{
			pPlayer->SetUseEntity(m_hPrevUseEntity.Get());
		}

		Vector vecForward;
		AngleVectors(GetAbsAngles(), &vecForward);
		Vector vecExitPos = GetAbsOrigin() - vecForward * 40.0f;
		pPlayer->Teleport(&vecExitPos, NULL, &vec3_origin);

		m_OnUnmounted.FireOutput(pPlayer, this);
	}

	m_hPrevUseEntity = NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Check if an NPC is eligible to walk to and man this gun
//-----------------------------------------------------------------------------
bool CPropEmplacement::CanNPCAssign(CAI_BaseNPC* pNPC) const
{
	if (!pNPC || !pNPC->IsAlive() || m_bLocked || IsOccupied())
		return false;

	if (pNPC->GetMoveType() == MOVETYPE_NONE)
		return false;

	Vector vecManPoint = GetNPCManPoint();
	if (pNPC->GetAbsOrigin().DistToSqr(vecManPoint) > Square(EMPLACEMENT_NPC_SEARCH_RADIUS))
		return false;

	CBaseEntity* pEnemy = pNPC->GetEnemy();
	if (pEnemy)
	{
		// In a fight: require the enemy to be in the gun's aim cone and visible
		Vector vecEnemyTarget = pEnemy->BodyTarget(GetAbsOrigin(), false);
		if (!IsDirectionInRange(vecEnemyTarget))
			return false;

		if (!pNPC->FVisible(pEnemy))
			return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Scan nearby NPCs and send an eligible one to the man-point
//-----------------------------------------------------------------------------
void CPropEmplacement::FindAndAssignNPC()
{
	if (m_bLocked || IsOccupied())
		return;

	Vector vecManPoint = GetNPCManPoint();
	CAI_BaseNPC* pBestNPC = NULL;
	float flBestDistSq = Square(EMPLACEMENT_NPC_SEARCH_RADIUS);

	CBaseEntity* pEnt = NULL;
	while ((pEnt = gEntList.FindEntityInSphere(pEnt, vecManPoint, EMPLACEMENT_NPC_SEARCH_RADIUS)) != NULL)
	{
		CAI_BaseNPC* pNPC = pEnt->MyNPCPointer();
		if (!pNPC || !CanNPCAssign(pNPC))
			continue;

		float flDistSq = pNPC->GetAbsOrigin().DistToSqr(vecManPoint);
		if (flDistSq < flBestDistSq)
		{
			flBestDistSq = flDistSq;
			pBestNPC = pNPC;
		}
	}

	if (pBestNPC)
	{
		AssignNPC(pBestNPC);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Assign an NPC to walk to the man-point without teleporting
//-----------------------------------------------------------------------------
bool CPropEmplacement::AssignNPC(CAI_BaseNPC* pNPC)
{
	if (!pNPC || !pNPC->IsAlive() || m_bLocked || IsOccupied())
		return false;

	m_hNPCUser = pNPC;
	m_nPrevNPCMoveType = pNPC->GetMoveType();
	m_bNPCInRoute = true;

	Vector vecManPoint = GetNPCManPoint();
	CAI_Navigator* pNav = pNPC->GetNavigator();
	if (!pNav)
		return false;

	AI_NavGoal_t goal(vecManPoint, ACT_RUN, AIN_HULL_TOLERANCE);
	if (!pNav->SetGoal(goal))
	{
		goal.activity = ACT_WALK;
		if (!pNav->SetGoal(goal))
			return false;
	}

	pNav->SetArrivalDirection(GetAbsAngles());
	pNPC->SetSchedule(SCHED_IDLE_STAND);
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Release the assigned or manning NPC
//-----------------------------------------------------------------------------
void CPropEmplacement::ReleaseNPC()
{
	CAI_BaseNPC* pNPC = m_hNPCUser.Get();
	bool bWasManned = (pNPC != NULL && !m_bNPCInRoute);
	m_hNPCUser = NULL;
	m_bNPCInRoute = false;

	if (pNPC)
	{
		if (pNPC->GetMoveType() == MOVETYPE_NONE)
		{
			pNPC->SetMoveType(m_nPrevNPCMoveType != MOVETYPE_NONE ? m_nPrevNPCMoveType : MOVETYPE_STEP);
		}

		pNPC->DoUnholster();
		if (bWasManned)
		{
			m_OnUnmounted.FireOutput(pNPC, this);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Handle NPC walking to man-point, standing at gun, aiming, and firing
//-----------------------------------------------------------------------------
void CPropEmplacement::UpdateNPCGunner(CAI_BaseNPC* pNPC)
{
	if (!pNPC->IsAlive())
	{
		ReleaseNPC();
		return;
	}

	CBaseEntity* pEnemy = pNPC->GetEnemy();
	if (pEnemy && pEnemy->IsAlive())
	{
		Vector vecEnemyTarget = pEnemy->BodyTarget(GetAbsOrigin(), false);
		if (!IsDirectionInRange(vecEnemyTarget))
		{
			ReleaseNPC();
			return;
		}
	}

	Vector vecManPoint = GetNPCManPoint();
	if (m_bNPCInRoute)
	{
		if (pNPC->GetAbsOrigin().DistToSqr(vecManPoint) <= Square(EMPLACEMENT_MAN_DISTANCE))
		{
			// Reached the man-point naturally via navigation: hold position facing the gun's forward arc
			m_bNPCInRoute = false;
			pNPC->SetMoveType(MOVETYPE_NONE);
			QAngle angGun = GetAbsAngles();
			pNPC->SetAbsAngles(angGun);
			pNPC->DoHolster();
			pNPC->SetSchedule(SCHED_IDLE_STAND);
			m_OnMounted.FireOutput(pNPC, this);
		}
		else if (pNPC->GetAbsOrigin().DistToSqr(vecManPoint) > Square(EMPLACEMENT_NPC_SEARCH_RADIUS * 1.25f))
		{
			ReleaseNPC();
		}
		return;
	}

	if (!pEnemy || !pEnemy->IsAlive())
	{
		// No fight going on: stay at the man-point without requiring an enemy in arc
		ApplyAimPoseParameters(0.0f, 0.0f);
		return;
	}

	Vector vecTarget = pEnemy->BodyTarget(GetAbsOrigin(), false);
	float flYaw = 0.0f;
	float flPitch = 0.0f;
	bool bInRange = IsDirectionInRange(vecTarget, &flYaw, &flPitch);
	bool bVisible = pNPC->FVisible(pEnemy);

	if (!bInRange)
	{
		ReleaseNPC();
		return;
	}

	ApplyAimPoseParameters(flYaw, flPitch);

	if (bVisible && gpGlobals->curtime >= m_flNextFireTime)
	{
		Vector vecMuzzlePos;
		QAngle vecMuzzleAng;
		GetMuzzleTransform(vecMuzzlePos, vecMuzzleAng);

		Vector vecDir = vecTarget - vecMuzzlePos;
		VectorNormalize(vecDir);

		FireEmplacement(pNPC, vecMuzzlePos, vecDir);
		m_flNextFireTime = gpGlobals->curtime + MAX(0.02f, m_flFireRate);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Main per-tick update
//-----------------------------------------------------------------------------
void CPropEmplacement::Think()
{
	if (m_bLocked)
	{
		if (m_hUser.Get() != NULL)
		{
			UnmountPlayer(true);
		}
		if (m_hNPCUser.Get() != NULL)
		{
			ReleaseNPC();
		}
	}
	else if (CBasePlayer* pPlayer = m_hUser.Get())
	{
		if (!pPlayer->IsAlive() || !pPlayer->IsConnected())
		{
			UnmountPlayer(true);
		}
		else if ((pPlayer->m_nButtons & IN_USE) && gpGlobals->curtime >= m_flExitCooldown)
		{
			pPlayer->m_nButtons &= ~IN_USE;
			UnmountPlayer(true);
		}
		else
		{
			UpdatePlayerAimAndFiring(pPlayer);
		}
	}
	else if (CAI_BaseNPC* pNPC = m_hNPCUser.Get())
	{
		UpdateNPCGunner(pNPC);
	}
	else if (gpGlobals->curtime >= m_flNPCNextScanTime)
	{
		FindAndAssignNPC();
		m_flNPCNextScanTime = gpGlobals->curtime + 0.35f;
	}

	SetNextThink(gpGlobals->curtime + 0.015f);
}

//-----------------------------------------------------------------------------
// Purpose: Clamp player view angles to gun arc and fire on +attack
//-----------------------------------------------------------------------------
void CPropEmplacement::UpdatePlayerAimAndFiring(CBasePlayer* pPlayer)
{
	QAngle angBase = GetAbsAngles();
	QAngle angEye = pPlayer->EyeAngles();

	float flDeltaYaw = AngleDiff(angEye.y, angBase.y);
	float flDeltaYawClamped = clamp(flDeltaYaw, -m_flYawRange, m_flYawRange);

	float flPitch = AngleNormalize(angEye.x);
	float flPitchClamped = clamp(flPitch, -m_flPitchUpRange, m_flPitchDownRange);

	QAngle angClamped(flPitchClamped, AngleNormalize(angBase.y + flDeltaYawClamped), 0.0f);
	if (fabsf(flDeltaYaw - flDeltaYawClamped) > 0.01f || fabsf(flPitch - flPitchClamped) > 0.01f)
	{
		pPlayer->SnapEyeAngles(angClamped);
	}

	ApplyAimPoseParameters(flDeltaYawClamped, flPitchClamped);

	if ((pPlayer->m_nButtons & IN_ATTACK) && gpGlobals->curtime >= m_flNextFireTime)
	{
		Vector vecMuzzlePos;
		QAngle vecMuzzleAng;
		GetMuzzleTransform(vecMuzzlePos, vecMuzzleAng);

		Vector vecAimDir;
		AngleVectors(angClamped, &vecAimDir);

		FireEmplacement(pPlayer, vecMuzzlePos, vecAimDir);
		m_flNextFireTime = gpGlobals->curtime + MAX(0.02f, m_flFireRate);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Drive optional aim pose parameters on the emplacement model
//-----------------------------------------------------------------------------
void CPropEmplacement::ApplyAimPoseParameters(float flYaw, float flPitch)
{
	int iYawPose = LookupPoseParameter("aim_yaw");
	if (iYawPose >= 0)
	{
		SetPoseParameter(iYawPose, flYaw);
	}

	int iPitchPose = LookupPoseParameter("aim_pitch");
	if (iPitchPose >= 0)
	{
		SetPoseParameter(iPitchPose, flPitch);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Emit sound, tracers, and bullet damage
//-----------------------------------------------------------------------------
void CPropEmplacement::FireEmplacement(CBaseEntity* pAttacker, const Vector& vecStart, const Vector& vecDir)
{
	if (m_iszFireSound != NULL_STRING)
	{
		EmitSound(STRING(m_iszFireSound));
	}
	else
	{
		EmitSound("Weapon_AR2.Single");
	}

	FireBulletsInfo_t info;
	info.m_iShots = 1;
	info.m_vecSrc = vecStart;
	info.m_vecDirShooting = vecDir;
	info.m_vecSpread = VECTOR_CONE_2DEGREES;
	info.m_flDistance = MAX_TRACE_LENGTH;
	info.m_iTracerFreq = 1;
	info.m_iAmmoType = m_iAmmoType;
	info.m_pAttacker = pAttacker;
	info.m_flDamage = (float)m_iDamage;

	FireBullets(info);
	m_OnFire.FireOutput(pAttacker, this);
}

//-----------------------------------------------------------------------------
// Purpose: Resolve muzzle attachment position or fallback offset
//-----------------------------------------------------------------------------
void CPropEmplacement::GetMuzzleTransform(Vector& vecPos, QAngle& vecAngles)
{
	int iAttachment = LookupAttachment(EMPLACEMENT_MUZZLE_ATTACHMENT);
	if (iAttachment > 0)
	{
		GetAttachment(iAttachment, vecPos, vecAngles);
		return;
	}

	vecAngles = GetAbsAngles();
	Vector vecForward, vecUp;
	AngleVectors(vecAngles, &vecForward, NULL, &vecUp);
	vecPos = GetAbsOrigin() + vecForward * 36.0f + vecUp * 24.0f;
}

//-----------------------------------------------------------------------------
// Purpose: Hammer I/O handlers
//-----------------------------------------------------------------------------
void CPropEmplacement::InputLock(inputdata_t& inputdata)
{
	m_bLocked = true;
}

void CPropEmplacement::InputUnlock(inputdata_t& inputdata)
{
	m_bLocked = false;
}

void CPropEmplacement::InputForceMount(inputdata_t& inputdata)
{
	if (m_bLocked || IsOccupied())
		return;

	if (CAI_BaseNPC* pNPC = (inputdata.pActivator ? inputdata.pActivator->MyNPCPointer() : NULL))
	{
		AssignNPC(pNPC);
		return;
	}

	CBasePlayer* pPlayer = ToBasePlayer(inputdata.pActivator);
	if (!pPlayer)
	{
		pPlayer = UTIL_GetLocalPlayer();
	}

	if (pPlayer)
	{
		MountPlayer(pPlayer);
	}
}

void CPropEmplacement::InputForceUnmount(inputdata_t& inputdata)
{
	if (m_hUser.Get() != NULL)
	{
		UnmountPlayer(true);
	}
	if (m_hNPCUser.Get() != NULL)
	{
		ReleaseNPC();
	}
}