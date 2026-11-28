#include "cbase.h"
#include "NavMeshCover.h"
#include "NavMeshManager.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "engine/IEngineTrace.h"
#include "vphysics_interface.h"
#include "ndebugoverlay.h"
#include "props.h"
#include "doors.h"
#include "BasePropDoor.h"
#include "basetoggle.h"
#include "NavMeshDynamic.h"

// memdbgon must be the last include file in a .cpp file
#include "tier0/memdbgon.h"

namespace
{
	inline void SourceToDetourLocal(const Vector& in, float out[3])
	{
		out[0] = in.x;
		out[1] = in.z;
		out[2] = -in.y;
	}

	inline void DetourToSourceLocal(const float in[3], Vector& out)
	{
		out.x = in[0];
		out.y = -in[2];
		out.z = in[1];
	}

	// Snap a candidate point onto the loaded navmesh and drop to ground
	bool SnapToNavFloor(dtNavMeshQuery* pQuery, const Vector& vecIn, Vector& vecOutFloor)
	{
		if (!pQuery)
			return false;

		float dPos[3], nearest[3];
		SourceToDetourLocal(vecIn, dPos);

		float halfExtents[3] = { 24.0f, 48.0f, 24.0f };
		dtQueryFilter filter;
		dtPolyRef polyRef = 0;

		if (dtStatusFailed(pQuery->findNearestPoly(dPos, halfExtents, &filter, &polyRef, nearest)) || !polyRef)
			return false;

		Vector vecNavPos;
		DetourToSourceLocal(nearest, vecNavPos);

		trace_t tr;
		UTIL_TraceLine(
			vecNavPos + Vector(0, 0, 16.0f),
			vecNavPos - Vector(0, 0, 64.0f),
			MASK_SOLID,
			NULL,
			COLLISION_GROUP_NONE,
			&tr
		);

		vecOutFloor = tr.DidHit() ? tr.endpos : vecNavPos;
		return true;
	}

	// Returns true if an entity is a movable/physics prop (not world or static prop)
	bool IsMovablePropEntity(CBaseEntity* pEnt)
	{
		if (!pEnt || pEnt->IsWorld() || pEnt->entindex() == 0)
			return false;

		MoveType_t mt = pEnt->GetMoveType();
		if (mt == MOVETYPE_VPHYSICS || mt == MOVETYPE_PUSH)
			return true;

		if (FClassnameIs(pEnt, "prop_physics") ||
			FClassnameIs(pEnt, "prop_physics_multiplayer") ||
			FClassnameIs(pEnt, "prop_physics_respawnable") ||
			FClassnameIs(pEnt, "func_physbox"))
		{
			return true;
		}

		IPhysicsObject* pPhys = pEnt->VPhysicsGetObject();
		if (pPhys && pPhys->IsMoveable())
			return true;

		return false;
	}

	// Returns true if an entity is a door, and outputs whether it is currently open
	bool IsDoorEntity(CBaseEntity* pEnt, bool& bOutIsOpen)
	{
		bOutIsOpen = false;
		if (!pEnt)
			return false;

		if (FClassnameIs(pEnt, "prop_door_rotating"))
		{
			CBasePropDoor* pDoor = dynamic_cast<CBasePropDoor*>(pEnt);
			if (!pDoor)
				return false;
			bOutIsOpen = pDoor->IsDoorOpen();
			return true;
		}

		if (FClassnameIs(pEnt, "func_door") || FClassnameIs(pEnt, "func_door_rotating"))
		{
			CBaseToggle* pToggle = dynamic_cast<CBaseToggle*>(pEnt);
			if (!pToggle)
				return false;
			bOutIsOpen = (pToggle->m_toggle_state == TS_AT_TOP);
			return true;
		}

		return false;
	}
	// Trace filter that only detects movable props and doors (ignores static world/brush/static props)
	class CDynamicCoverTraceFilter : public CTraceFilter
	{
	public:
		bool m_bHitMovableProp;
		bool m_bHitOpenDoor;
		bool m_bHitClosedDoor;
		Vector m_vecHitPos;

		CDynamicCoverTraceFilter()
			: m_bHitMovableProp(false)
			, m_bHitOpenDoor(false)
			, m_bHitClosedDoor(false)
			, m_vecHitPos(vec3_origin)
		{
		}

		virtual bool ShouldHitEntity(IHandleEntity* pServerEntity, int contentsMask) override
		{
			CBaseEntity* pEnt = EntityFromEntityHandle(pServerEntity);
			if (!pEnt || pEnt->IsWorld() || pEnt->IsPlayer())
				return false;

			bool bDoorOpen = false;
			if (IsDoorEntity(pEnt, bDoorOpen))
			{
				if (bDoorOpen)
					m_bHitOpenDoor = true;
				else
					m_bHitClosedDoor = true;
				return true;
			}

			if (IsMovablePropEntity(pEnt))
			{
				m_bHitMovableProp = true;
				return true;
			}

			return false;
		}
	};

	float MeasureDynamicObstacleHeight(const Vector& vecXY, float flFloorZ, float flMaxCheckHeight = 76.0f)
	{
		CDynamicCoverTraceFilter filter;
		trace_t tr;
		Vector vecTop(vecXY.x, vecXY.y, flFloorZ + flMaxCheckHeight);
		Vector vecBottom(vecXY.x, vecXY.y, flFloorZ + 4.0f);

		UTIL_TraceLine(vecTop, vecBottom, MASK_SHOT, &filter, &tr);
		if (!tr.DidHit() || (!filter.m_bHitMovableProp && !filter.m_bHitClosedDoor))
			return 0.0f;

		if (tr.endpos.z <= flFloorZ + 4.0f)
			return flMaxCheckHeight;

		return clamp(tr.endpos.z - flFloorZ, 0.0f, flMaxCheckHeight);
	}

	bool BlocksDynamicSight(const Vector& vecFrom, const Vector& vecTo, CDynamicCoverTraceFilter& outFilter)
	{
		trace_t tr;
		UTIL_TraceLine(vecFrom, vecTo, MASK_SHOT, &outFilter, &tr);
		return (tr.fraction < 0.98f) && (outFilter.m_bHitMovableProp || outFilter.m_bHitClosedDoor || outFilter.m_bHitOpenDoor);
	}

	bool HasLineOfSight(const Vector& vecFrom, const Vector& vecTo)
	{
		trace_t tr;
		UTIL_TraceLine(vecFrom, vecTo, MASK_SHOT, NULL, COLLISION_GROUP_NONE, &tr);
		return (tr.fraction >= 0.98f);
	}

	static ConVar ez2_nav_cover_strict_los("ez2_nav_cover_strict_los", "1", 0, "Reject baked cover the threat can currently see (live LOS check)");

	bool BlocksLOS(const Vector& vecFrom, const Vector& vecTo)
	{
		trace_t tr;
		UTIL_TraceLine(vecFrom, vecTo, MASK_BLOCKLOS, NULL, COLLISION_GROUP_NONE, &tr);
		return (tr.fraction < 0.98f);
	}

	void SortAndTrimCoverSpots(std::vector<EZ2_CoverSpot_t>& spots, int maxResults)
	{
		for (size_t i = 0; i < spots.size(); ++i)
		{
			size_t bestIdx = i;
			for (size_t j = i + 1; j < spots.size(); ++j)
			{
				if (spots[j].flScore > spots[bestIdx].flScore)
					bestIdx = j;
			}
			if (bestIdx != i)
			{
				EZ2_CoverSpot_t tmp = spots[i];
				spots[i] = spots[bestIdx];
				spots[bestIdx] = tmp;
			}
		}

		// Dedupe: baked and dynamic candidates often land on the same spot.
		// Spots are already sorted best-first, so the best of each cluster survives.
		const float flMinSepSqr = 48.0f * 48.0f;
		std::vector<EZ2_CoverSpot_t> kept;
		for (size_t i = 0; i < spots.size(); ++i)
		{
			bool bDup = false;
			for (size_t k = 0; k < kept.size(); ++k)
			{
				if (kept[k].vecPos.DistToSqr(spots[i].vecPos) < flMinSepSqr)
				{
					bDup = true;
					break;
				}
			}
			if (!bDup)
				kept.push_back(spots[i]);
		}
		spots.swap(kept);

		if (static_cast<int>(spots.size()) > maxResults)
		{
			spots.resize(maxResults);
		}
	}

	// Pulls baked cover hints from NavMeshManager (used when _node.nav has baked hints)
	int AppendBakedCoverHints(
		dtNavMeshQuery* pQuery,
		const Vector& vecCenter,
		const Vector& vecThreatPos,
		float flSearchRadius,
		std::vector<EZ2_CoverSpot_t>& outSpots
	)
	{
		const std::vector<EZ2_NavHint_t>& hints = g_EZ2NavMeshManager.GetNavHints();
		if (hints.empty())
			return 0;

		int added = 0;
		const float flMaxDistSqr = flSearchRadius * flSearchRadius;
		const Vector vecThreatEye = vecThreatPos + Vector(0, 0, 56.0f);

		for (size_t i = 0; i < hints.size(); ++i)
		{
			const EZ2_NavHint_t& hint = hints[i];

			if (hint.navHintType != EZ2_NAVHINT_COVER_CROUCH &&
				hint.navHintType != EZ2_NAVHINT_COVER_STAND &&
				hint.navHintType != EZ2_NAVHINT_PINCH_OR_FLANK)
			{
				continue;
			}

			if (hint.polyRef == 0 || hint.bDisabled)
				continue;

			// Spot sits behind a locked door / unopenable brush: NPC can't reach it
			if (EZ2_NavGetDynStateByRef(hint.polyRef) == EZ2_NAVDYN_BLOCKED)
				continue;

			// Facing cone: threat must be on the blocking-surface side (generated hints use fovCos 0.5)
			if (hint.flFovCos < 0.999f)
			{
				Vector vecToThreat = vecThreatPos - hint.vecPos;
				vecToThreat.z = 0.0f;
				if (vecToThreat.LengthSqr() > 1.0f)
				{
					vecToThreat.NormalizeInPlace();
					if (DotProduct(UTIL_YawToVector(hint.flYaw), vecToThreat) < hint.flFovCos)
						continue;
				}
			}

			// Skip hints claimed by another entity or on cooldown
			if (hint.hClaimedBy.Get() != NULL)
				continue;

			if (gpGlobals && gpGlobals->curtime < hint.flNextAvailableTime)
				continue;

			float flDistSqr = vecCenter.DistToSqr(hint.vecPos);
			if (flSearchRadius > 0.0f && flDistSqr > flMaxDistSqr)
				continue;

			Vector vecFloor;
			if (!SnapToNavFloor(pQuery, hint.vecPos, vecFloor))
				vecFloor = hint.vecPos;

			// Spot is on the floor under a resting physics prop: occupied, skip
			if (EZ2_NavPointInPropCutout(vecFloor))
				continue;

			Vector vecToThreatXY(vecThreatPos.x - vecFloor.x, vecThreatPos.y - vecFloor.y, 0.0f);
			if (vecToThreatXY.LengthSqr() > 0.01f)
				vecToThreatXY.NormalizeInPlace();
			else
				vecToThreatXY = UTIL_YawToVector(hint.flYaw);

			EZ2_CoverKind_t kind = EZ2_COVER_TALL_WALL_OR_PROP;
			float flObsHeight = 64.0f;
			float flScore = 32.0f * ((hint.flWeight > 0.01f) ? hint.flWeight : 1.0f);

			if (hint.navHintType == EZ2_NAVHINT_COVER_CROUCH)
			{
				kind = EZ2_COVER_LOW_SHORT;
				flObsHeight = 34.0f;
				flScore += 4.0f;
			}
			else if (hint.navHintType == EZ2_NAVHINT_PINCH_OR_FLANK)
			{
				kind = EZ2_COVER_CORNER;
				flObsHeight = 48.0f;
			}

			// Live revalidation: the cover must actually block the threat right now.
			// Corner/peek spots are exempt from rejection, since they're expected to be partly visible.
			const float flEyeH = (kind == EZ2_COVER_LOW_SHORT) ? 32.0f : 62.0f;
			if (BlocksLOS(vecThreatEye, vecFloor + Vector(0, 0, flEyeH)))
			{
				flScore += 10.0f;
			}
			else if (kind != EZ2_COVER_CORNER && ez2_nav_cover_strict_los.GetBool())
			{
				continue;
			}

			float flDistThreat = vecFloor.DistTo(vecThreatPos);
			flScore -= sqrtf(flDistSqr) * 0.025f;
			if (flDistThreat < 140.0f)
				flScore -= 15.0f;

			EZ2_CoverSpot_t spot;
			spot.vecPos = vecFloor;
			spot.vecCoverNormal = vecToThreatXY;
			// Generated crouch/stand hints store yaw toward the wall, so they have no peek direction
			const bool bGenerated = (hint.sourceHintType == 0 && hint.sourceWCNodeID < 0);
			spot.vecPeekDir = (bGenerated && kind != EZ2_COVER_CORNER) ? Vector(vec3_origin) : UTIL_YawToVector(hint.flYaw);
			spot.flObstacleHeight = flObsHeight;
			spot.flScore = flScore;
			spot.kind = kind;

			outSpots.push_back(spot);
			++added;
		}

		return added;
	}

	// Classifies one candidate floor spot against live obstacles (physics props / doors)
	void EvaluateDynamicCandidate(dtNavMeshQuery* pQuery, const Vector& vecFloor, const Vector& vecCenter,
		const Vector& vecThreatPos, std::vector<EZ2_CoverSpot_t>& outSpots)
	{
		const Vector vecThreatEye = vecThreatPos + Vector(0, 0, 56.0f);
		const Vector vecStandEye = vecFloor + Vector(0, 0, 62.0f);
		const Vector vecCrouchEye = vecFloor + Vector(0, 0, 32.0f);

		CDynamicCoverTraceFilter standFilter;
		CDynamicCoverTraceFilter crouchFilter;
		const bool bStandDynBlocked = BlocksDynamicSight(vecThreatEye, vecStandEye, standFilter);
		const bool bCrouchDynBlocked = BlocksDynamicSight(vecThreatEye, vecCrouchEye, crouchFilter);

		Vector vecToThreatXY(vecThreatPos.x - vecFloor.x, vecThreatPos.y - vecFloor.y, 0.0f);
		if (vecToThreatXY.LengthSqr() < 0.01f)
			vecToThreatXY = Vector(1.0f, 0.0f, 0.0f);
		else
			vecToThreatXY.NormalizeInPlace();

		const Vector vecProbePos = vecFloor + vecToThreatXY * 28.0f;
		const float flObsHeight = MeasureDynamicObstacleHeight(vecProbePos, vecFloor.z, 76.0f);

		const Vector vecRight(-vecToThreatXY.y, vecToThreatXY.x, 0.0f);
		Vector vecLeftFloor, vecRightFloor;
		const bool bLeftClear = SnapToNavFloor(pQuery, vecFloor - vecRight * 36.0f, vecLeftFloor) &&
			HasLineOfSight(vecThreatEye, vecLeftFloor + Vector(0, 0, 56.0f));
		const bool bRightClear = SnapToNavFloor(pQuery, vecFloor + vecRight * 36.0f, vecRightFloor) &&
			HasLineOfSight(vecThreatEye, vecRightFloor + Vector(0, 0, 56.0f));

		EZ2_CoverKind_t kind = EZ2_COVER_NONE;
		Vector vecPeekDir(vec3_origin);
		float flScore = 0.0f;

		const bool bHasOpenDoor = standFilter.m_bHitOpenDoor || crouchFilter.m_bHitOpenDoor;
		if (bHasOpenDoor && (bLeftClear || bRightClear))
		{
			kind = EZ2_COVER_CORNER;
			vecPeekDir = bRightClear ? vecRight : -vecRight;
			flScore += 22.0f;
		}
		else if (bStandDynBlocked && bCrouchDynBlocked)
		{
			if (bLeftClear || bRightClear)
			{
				kind = EZ2_COVER_CORNER;
				vecPeekDir = bRightClear ? vecRight : -vecRight;
				flScore += 30.0f;
			}
			else
			{
				kind = EZ2_COVER_TALL_WALL_OR_PROP;
				flScore += 22.0f;
			}
		}
		else if (bCrouchDynBlocked && !bStandDynBlocked && flObsHeight >= 22.0f && flObsHeight <= 46.0f)
		{
			kind = EZ2_COVER_LOW_SHORT;
			flScore += 25.0f;
		}
		else if (flObsHeight >= 24.0f && (bLeftClear || bRightClear))
		{
			kind = EZ2_COVER_CORNER;
			vecPeekDir = bRightClear ? vecRight : -vecRight;
			flScore += 18.0f;
		}

		if (kind == EZ2_COVER_NONE)
			return;

		flScore -= vecFloor.DistTo(vecCenter) * 0.03f;
		if (vecFloor.DistTo(vecThreatPos) < 140.0f)
			flScore -= 15.0f;

		EZ2_CoverSpot_t spot;
		spot.vecPos = vecFloor;
		spot.vecCoverNormal = vecToThreatXY;
		spot.vecPeekDir = vecPeekDir;
		spot.flObstacleHeight = flObsHeight;
		spot.flScore = flScore;
		spot.kind = kind;
		outSpots.push_back(spot);
	}

	// Candidates placed directly behind (and to either side of) each resting prop / door, relative to the threat
	void AddPropAnchoredCandidates(dtNavMeshQuery* pQuery, const Vector& vecCenter, const Vector& vecThreatPos,
		float flSearchRadius, std::vector<EZ2_CoverSpot_t>& outSpots)
	{
		int nProps = 0;
		for (CBaseEntity* pEnt = gEntList.FindEntityInSphere(NULL, vecCenter, flSearchRadius); pEnt && nProps < 32;
			pEnt = gEntList.FindEntityInSphere(pEnt, vecCenter, flSearchRadius))
		{
			bool bOpen = false;
			if (!IsMovablePropEntity(pEnt) && !IsDoorEntity(pEnt, bOpen))
				continue;
			if (pEnt->IsEffectActive(EF_NODRAW) || pEnt->IsSolidFlagSet(FSOLID_NOT_SOLID))
				continue;

			// A prop that's still tumbling makes bad cover
			IPhysicsObject* pPhys = pEnt->VPhysicsGetObject();
			if (pPhys && pPhys->IsMoveable() && !pPhys->IsAsleep())
				continue;

			Vector mins, maxs;
			pEnt->CollisionProp()->WorldSpaceAABB(&mins, &maxs);
			if (maxs.z - mins.z < 20.0f)
				continue;

			const Vector c = pEnt->WorldSpaceCenter();
			const float flHalf = MAX(maxs.x - mins.x, maxs.y - mins.y) * 0.5f;

			Vector away(c.x - vecThreatPos.x, c.y - vecThreatPos.y, 0.0f);
			if (VectorNormalize(away) < 0.01f)
				continue;

			const Vector perp(-away.y, away.x, 0.0f);
			const Vector behind = c + away * (flHalf + 24.0f);
			const Vector cands[3] = { behind, behind + perp * (flHalf * 0.8f), behind - perp * (flHalf * 0.8f) };

			++nProps;
			for (int i = 0; i < 3; ++i)
			{
				Vector vecFloor;
				if (SnapToNavFloor(pQuery, cands[i], vecFloor))
					EvaluateDynamicCandidate(pQuery, vecFloor, vecCenter, vecThreatPos, outSpots);
			}
		}
	}
}

void FindCoverSpotsAroundOrigin(
	const Vector& vecCenter,
	const Vector& vecThreatPos,
	float flSearchRadius,
	int maxResults,
	std::vector<EZ2_CoverSpot_t>& outSpots,
	bool bIncludeBaked
)
{
	outSpots.clear();

	if (!g_EZ2NavMeshManager.IsReady() || maxResults <= 0)
		return;

	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();
	if (!pQuery)
		return;

	// 1. Baked cover hints
	const int bakedCount = bIncludeBaked
		? AppendBakedCoverHints(pQuery, vecCenter, vecThreatPos, flSearchRadius, outSpots) : 0;

	// 2a. Dynamic: spots anchored on nearby physics props / doors
	AddPropAnchoredCandidates(pQuery, vecCenter, vecThreatPos, flSearchRadius, outSpots);

	// 2b. Dynamic: coarse grid (catches open doorways etc.)
	const int radialSteps = (bakedCount > 0) ? 2 : 3;
	const int angleSteps = 12;
	for (int rIdx = 1; rIdx <= radialSteps; ++rIdx)
	{
		const float radius = (flSearchRadius * rIdx) / static_cast<float>(radialSteps);
		for (int aIdx = 0; aIdx < angleSteps; ++aIdx)
		{
			const float angle = (2.0f * M_PI * aIdx) / static_cast<float>(angleSteps);
			const Vector vecCandidate(vecCenter.x + cosf(angle) * radius, vecCenter.y + sinf(angle) * radius, vecCenter.z);

			Vector vecFloor;
			if (SnapToNavFloor(pQuery, vecCandidate, vecFloor))
				EvaluateDynamicCandidate(pQuery, vecFloor, vecCenter, vecThreatPos, outSpots);
		}
	}

	SortAndTrimCoverSpots(outSpots, maxResults);
}

CON_COMMAND(ez2_nav_test_cover, "Tests FindCoverSpotsAroundOrigin using player pos as center and aim point as threat")
{
	CBasePlayer* pPlayer = UTIL_GetCommandClient();
	if (!pPlayer)
	{
		Warning("[EZ2 NavMesh] No player found for ez2_nav_test_cover.\n");
		return;
	}

	if (!g_EZ2NavMeshManager.IsReady())
	{
		Warning("[EZ2 NavMesh] No navmesh is currently loaded/ready.\n");
		return;
	}

	float flRadius = (args.ArgC() >= 2) ? atof(args[1]) : 512.0f;
	int maxResults = (args.ArgC() >= 3) ? atoi(args[2]) : 8;

	Vector vecEye = pPlayer->EyePosition();
	Vector vecForward;
	pPlayer->EyeVectors(&vecForward);

	trace_t tr;
	UTIL_TraceLine(vecEye, vecEye + vecForward * 4096.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr);
	Vector vecThreat = tr.DidHit() ? tr.endpos : (vecEye + vecForward * 512.0f);

	std::vector<EZ2_CoverSpot_t> spots;
	FindCoverSpotsAroundOrigin(pPlayer->GetAbsOrigin(), vecThreat, flRadius, maxResults, spots);

	Msg("[EZ2 NavMesh] Found %d cover spot(s) within %.0f units (threat: %.0f, %.0f, %.0f):\n",
		static_cast<int>(spots.size()), flRadius, vecThreat.x, vecThreat.y, vecThreat.z);

	for (size_t i = 0; i < spots.size(); ++i)
	{
		const EZ2_CoverSpot_t& s = spots[i];
		const char* pszKind = "NONE";
		int r = 200, g = 200, b = 200;

		if (s.kind == EZ2_COVER_LOW_SHORT)
		{
			pszKind = "LOW_SHORT";
			r = 255; g = 200; b = 0;
		}
		else if (s.kind == EZ2_COVER_TALL_WALL_OR_PROP)
		{
			pszKind = "TALL_WALL_OR_PROP";
			r = 0; g = 180; b = 255;
		}
		else if (s.kind == EZ2_COVER_CORNER)
		{
			pszKind = "CORNER";
			r = 80; g = 255; b = 120;
		}

		Msg("  [%d] %s | Score: %.1f | ObsH: %.1f | Pos: (%.0f, %.0f, %.0f)\n",
			static_cast<int>(i), pszKind, s.flScore, s.flObstacleHeight, s.vecPos.x, s.vecPos.y, s.vecPos.z);

		NDebugOverlay::Box(s.vecPos, Vector(-10, -10, 0), Vector(10, 10, s.flObstacleHeight > 0.0f ? s.flObstacleHeight : 36.0f), r, g, b, 110, 10.0f);

		if (s.vecPeekDir.LengthSqr() > 0.01f)
		{
			NDebugOverlay::Line(
				s.vecPos + Vector(0, 0, 24.0f),
				s.vecPos + Vector(0, 0, 24.0f) + s.vecPeekDir * 32.0f,
				255, 255, 255, true, 10.0f
			);
		}
	}
}