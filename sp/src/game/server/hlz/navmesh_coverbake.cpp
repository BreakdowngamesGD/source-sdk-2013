//=============================================================================
// navmesh_coverbake.cpp
// Bake-time static cover detection for the full-geometry navmesh.
// Samples along navmesh border edges (where walls are) and probes with traces.
//
// Hint conventions (read by AppendBakedCoverHints / FindBestNavHint):
//   COVER_CROUCH / COVER_STAND : yaw = direction TOWARD the blocking surface,
//                                fovCos = 0.5 (only valid if threat is on the wall side)
//   PINCH_OR_FLANK (corner)    : yaw = peek direction (the open lateral side)
//=============================================================================
#include "cbase.h"
#include "NavMeshBuilder_Full.h"
#include "NavMeshManager.h"
#include "DetourNavMesh.h"
#include "engine/IEngineTrace.h"
#include "props.h"
#include "doors.h"
#include "BasePropDoor.h"
#include <vector>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_cover_bake("ez2_nav_cover_bake", "1", 0, "Bake static cover hints into the full navmesh");
static ConVar ez2_nav_cover_sample("ez2_nav_cover_sample", "32", 0, "Cover bake: spacing between samples along navmesh border edges");
static ConVar ez2_nav_cover_spacing("ez2_nav_cover_spacing", "64", 0, "Cover bake: min distance between hints of the same type");
static ConVar ez2_nav_cover_max("ez2_nav_cover_max", "4096", 0, "Cover bake: max hints to keep");

namespace
{
	static const int   NUM_DIRS = 16;
	static const float PROBE_DIST = 48.0f;
	static const float CROUCH_H = 32.0f;
	static const float STAND_H = 62.0f;
	static const float SIDE_STEP = 36.0f;

	class CCoverBakeTraceFilter : public CTraceFilter
	{
	public:
		virtual bool ShouldHitEntity(IHandleEntity* pServerEntity, int contentsMask) override
		{
			CBaseEntity* pEnt = EntityFromEntityHandle(pServerEntity);
			if (!pEnt)
				return true;                         // static prop: not a CBaseEntity
			if (pEnt->IsWorld())
				return true;
			if (pEnt->IsEffectActive(EF_NODRAW) || pEnt->GetMoveParent() != NULL)
				return false;

			// Bone-follower collision belonging to a prop_dynamic
			if (FClassnameIs(pEnt, "phys_bone_follower"))
			{
				CBaseEntity* pOwner = pEnt->GetOwnerEntity();
				return pOwner && dynamic_cast<CDynamicProp*>(pOwner) != NULL && pOwner->GetMoveParent() == NULL;
			}

			if (pEnt->IsSolidFlagSet(FSOLID_NOT_SOLID))
				return false;
			if (dynamic_cast<CPhysicsProp*>(pEnt) != NULL)
				return false;
			return dynamic_cast<CBaseProp*>(pEnt) != NULL;
		}
	};

	inline void DetourToSrc(const float* p, Vector& out)
	{
		out.x = p[0];
		out.y = -p[2];
		out.z = p[1];
	}

	// World + non-physics props only (skip physics props & doors; those are handled at runtime)
	bool ProbeBlocked(const Vector& from, const Vector& to)
	{
		CCoverBakeTraceFilter filter;
		trace_t tr;
		UTIL_TraceLine(from, to, MASK_NPCWORLDSTATIC, &filter, &tr);

		if (tr.startsolid || tr.fraction >= 1.0f)
			return false;

		// Ignore floors/ceilings/slopes; we want vertical-ish obstacles
		return fabsf(tr.plane.normal.z) < 0.7f;
	}

	bool SnapToFloor(const Vector& p, Vector& out)
	{
		CCoverBakeTraceFilter filter;
		trace_t tr;
		UTIL_TraceLine(p + Vector(0, 0, 16.0f), p - Vector(0, 0, 64.0f), MASK_NPCWORLDSTATIC, &filter, &tr);
		if (tr.startsolid)
			return false;
		out = tr.DidHit() ? tr.endpos : p;
		return true;
	}

	bool TooCloseToExisting(const std::vector<EZ2_NavHint_t>& hints, const Vector& p, EZ2_NavHintType_t type, float minDistSqr)
	{
		for (size_t i = 0; i < hints.size(); ++i)
		{
			if (hints[i].navHintType == type && hints[i].vecPos.DistToSqr(p) < minDistSqr)
				return true;
		}
		return false;
	}

	void ProcessSample(const Vector& p, std::vector<EZ2_NavHint_t>& hints, float minDistSqr, int maxHints)
	{
		bool bHaveCrouch = false, bHaveStand = false, bHaveCorner = false;
		Vector crouchDir, standDir, cornerPeek;

		const Vector crouchOrg = p + Vector(0, 0, CROUCH_H);
		const Vector standOrg = p + Vector(0, 0, STAND_H);

		for (int i = 0; i < NUM_DIRS; ++i)
		{
			const float ang = (2.0f * M_PI * i) / (float)NUM_DIRS;
			const Vector d(cosf(ang), sinf(ang), 0.0f);

			const bool bCrouchHit = ProbeBlocked(crouchOrg, crouchOrg + d * PROBE_DIST);
			const bool bStandHit = ProbeBlocked(standOrg, standOrg + d * PROBE_DIST);

			if (bCrouchHit && !bStandHit)
			{
				if (!bHaveCrouch) { bHaveCrouch = true; crouchDir = d; }
				continue;
			}

			if (bCrouchHit && bStandHit)
			{
				if (!bHaveStand) { bHaveStand = true; standDir = d; }

				// Corner: wall ahead, and stepping sideways opens the line past its edge
				if (!bHaveCorner)
				{
					const Vector perp(-d.y, d.x, 0.0f);
					for (int s = 1; s >= -1; s -= 2)
					{
						const Vector q = standOrg + perp * (SIDE_STEP * s);
						if (!ProbeBlocked(standOrg, q) && !ProbeBlocked(q, q + d * PROBE_DIST))
						{
							bHaveCorner = true;
							cornerPeek = perp * (float)s;
							break;
						}
					}
				}
			}
		}

		// One hint per sample. Priority: crouch > corner > stand
		EZ2_NavHintType_t type;
		Vector yawDir;
		float fovCos = 0.5f;

		if (bHaveCrouch) { type = EZ2_NAVHINT_COVER_CROUCH;   yawDir = crouchDir; }
		else if (bHaveCorner) { type = EZ2_NAVHINT_PINCH_OR_FLANK; yawDir = cornerPeek; fovCos = 1.0f; }
		else if (bHaveStand) { type = EZ2_NAVHINT_COVER_STAND;    yawDir = standDir; }
		else
			return;

		if (TooCloseToExisting(hints, p, type, minDistSqr))
			return;

		if ((int)hints.size() >= maxHints)
			return;

		EZ2_NavHint_t h;
		h.vecPos = p;
		h.flYaw = UTIL_VecToYaw(yawDir);
		h.flFovCos = fovCos;
		h.flRadius = 0.0f;
		h.flWeight = 1.0f;
		h.navHintType = type;
		h.sourceHintType = 0;     // HINT_NONE: generated, not from a CAI_Hint
		h.sourceWCNodeID = -1;
		hints.push_back(h);
	}
}

int BakeFullMeshCoverHints(const dtNavMesh* pNavMesh, std::vector<EZ2_NavHint_t>& outHints)
{
	outHints.clear();

	if (!ez2_nav_cover_bake.GetBool() || !pNavMesh)
		return 0;

	const float sampleStep = MAX(ez2_nav_cover_sample.GetFloat(), 8.0f);
	const float minDist = ez2_nav_cover_spacing.GetFloat();
	const float minDistSqr = minDist * minDist;
	const int maxHints = ez2_nav_cover_max.GetInt();

	for (int t = 0; t < pNavMesh->getMaxTiles(); ++t)
	{
		const dtMeshTile* pTile = pNavMesh->getTile(t);
		if (!pTile || !pTile->header)
			continue;

		for (int i = 0; i < pTile->header->polyCount; ++i)
		{
			const dtPoly* pPoly = &pTile->polys[i];
			if (pPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
				continue;

			const int nv = pPoly->vertCount;
			if (nv < 3)
				continue;

			Vector verts[DT_VERTS_PER_POLYGON];
			Vector centroid(0, 0, 0);
			for (int v = 0; v < nv; ++v)
			{
				DetourToSrc(&pTile->verts[pPoly->verts[v] * 3], verts[v]);
				centroid += verts[v];
			}
			centroid /= (float)nv;

			for (int j = 0; j < nv; ++j)
			{
				// Only border edges (no neighbour poly): these run along walls/ledges
				if (pPoly->neis[j] != 0)
					continue;

				const Vector& a = verts[j];
				const Vector& b = verts[(j + 1) % nv];

				const float len = (b - a).Length2D();
				const int n = MAX(1, (int)(len / sampleStep));

				for (int k = 0; k < n; ++k)
				{
					Vector p = a + (b - a) * ((k + 0.5f) / (float)n);

					// Nudge a little toward the poly interior so we're safely on the mesh
					Vector toC = centroid - p;
					toC.z = 0.0f;
					const float dC = VectorNormalize(toC);
					p += toC * MIN(8.0f, dC);

					Vector floorPos;
					if (!SnapToFloor(p, floorPos))
						continue;

					ProcessSample(floorPos, outHints, minDistSqr, maxHints);

					if ((int)outHints.size() >= maxHints)
						return (int)outHints.size();
				}
			}
		}
	}

	return (int)outHints.size();
}