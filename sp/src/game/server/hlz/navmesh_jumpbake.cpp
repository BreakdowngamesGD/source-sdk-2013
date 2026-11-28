//=============================================================================
// navmesh_jumpbake.cpp
// Bake-time jump link generation for the full-geometry navmesh. Needs no nodegraph.
// Samples border edges (ledges/gaps), probes outward for a landing floor, verifies the
// landing lies on the mesh and isn't already easily reachable on foot.
//=============================================================================
#include "cbase.h"
#include "NavMeshBuilder_Full.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include <vector>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_jump_bake("ez2_nav_jump_bake", "1", 0, "Generate jump links from mesh geometry in the full navmesh build");
static ConVar ez2_nav_jump_max_up("ez2_nav_jump_max_up", "64", 0, "Jump bake: max height an NPC can jump up");
static ConVar ez2_nav_jump_max_drop("ez2_nav_jump_max_drop", "160", 0, "Jump bake: max drop height");
static ConVar ez2_nav_jump_max_dist("ez2_nav_jump_max_dist", "160", 0, "Jump bake: max horizontal jump distance");
static ConVar ez2_nav_jump_sample("ez2_nav_jump_sample", "48", 0, "Jump bake: spacing between samples along border edges");
static ConVar ez2_nav_jump_max("ez2_nav_jump_max", "512", 0, "Jump bake: max links to keep");

namespace
{
	inline void DetourToSrc(const float* p, Vector& o) { o.x = p[0]; o.y = -p[2]; o.z = p[1]; }
	inline void SrcToDetour(const Vector& v, float o[3]) { o[0] = v.x; o[1] = v.z; o[2] = -v.y; }

	bool TraceClear(const Vector& a, const Vector& b)
	{
		trace_t tr;
		UTIL_TraceLine(a, b, MASK_NPCWORLDSTATIC, NULL, COLLISION_GROUP_NONE, &tr);
		return !tr.startsolid && tr.fraction >= 1.0f;
	}

	bool FindFloor(const Vector& p, float up, float down, Vector& out)
	{
		trace_t tr;
		UTIL_TraceLine(p + Vector(0, 0, up), p - Vector(0, 0, down), MASK_NPCWORLDSTATIC, NULL, COLLISION_GROUP_NONE, &tr);
		if (tr.startsolid || !tr.DidHit() || tr.plane.normal.z < 0.7f)
			return false;
		out = tr.endpos;
		return true;
	}

	// True if the destination is already reachable on foot without a big detour
	bool EasilyWalkable(dtNavMeshQuery* q, const dtQueryFilter* f, dtPolyRef src, const float* sp, dtPolyRef dst, const float* dp)
	{
		static const int MAXP = 128;
		dtPolyRef path[MAXP];
		int n = 0;
		if (dtStatusFailed(q->findPath(src, dst, sp, dp, f, path, &n, MAXP)) || n <= 0 || path[n - 1] != dst)
			return false;   // partial or none: disconnected

		float sPath[MAXP * 3];
		unsigned char sFlags[MAXP];
		dtPolyRef sRefs[MAXP];
		int sn = 0;
		if (dtStatusFailed(q->findStraightPath(sp, dp, path, n, sPath, sFlags, sRefs, &sn, MAXP)) || sn <= 0)
			return false;

		float walk = 0.0f;
		for (int i = 1; i < sn; ++i)
		{
			const float dx = sPath[i * 3] - sPath[(i - 1) * 3];
			const float dy = sPath[i * 3 + 1] - sPath[(i - 1) * 3 + 1];
			const float dz = sPath[i * 3 + 2] - sPath[(i - 1) * 3 + 2];
			walk += sqrtf(dx * dx + dy * dy + dz * dz);
		}

		const float ex = dp[0] - sp[0], ey = dp[1] - sp[1], ez = dp[2] - sp[2];
		const float direct = sqrtf(ex * ex + ey * ey + ez * ez);
		return walk <= direct * 2.5f + 96.0f;
	}

	bool IsDuplicate(const std::vector<FullNavJumpLink_t>& v, const Vector& a, const Vector& b)
	{
		const float r2 = 48.0f * 48.0f;
		for (size_t i = 0; i < v.size(); ++i)
		{
			if ((v[i].a.DistToSqr(a) < r2 && v[i].b.DistToSqr(b) < r2) ||
				(v[i].a.DistToSqr(b) < r2 && v[i].b.DistToSqr(a) < r2))
				return true;
		}
		return false;
	}
}

int BakeFullMeshJumpLinks(const dtNavMesh* pNavMesh, std::vector<FullNavJumpLink_t>& out)
{
	if (!ez2_nav_jump_bake.GetBool() || !pNavMesh)
		return 0;

	dtNavMeshQuery* pQuery = dtAllocNavMeshQuery();
	if (!pQuery)
		return 0;
	if (dtStatusFailed(pQuery->init(pNavMesh, 4096)))
	{
		dtFreeNavMeshQuery(pQuery);
		return 0;
	}

	const float maxUp = ez2_nav_jump_max_up.GetFloat();
	const float maxDrop = ez2_nav_jump_max_drop.GetFloat();
	const float maxDist = ez2_nav_jump_max_dist.GetFloat();
	const float step = MAX(ez2_nav_jump_sample.GetFloat(), 16.0f);
	const int maxLinks = ez2_nav_jump_max.GetInt();
	const float ext[3] = { 16.0f, 24.0f, 16.0f };

	dtQueryFilter filter;
	const size_t before = out.size();

	for (int t = 0; t < pNavMesh->getMaxTiles() && (int)(out.size() - before) < maxLinks; ++t)
	{
		const dtMeshTile* pTile = pNavMesh->getTile(t);
		if (!pTile || !pTile->header)
			continue;

		const dtPolyRef base = pNavMesh->getPolyRefBase(pTile);

		for (int i = 0; i < pTile->header->polyCount; ++i)
		{
			const dtPoly* pPoly = &pTile->polys[i];
			if (pPoly->getType() != DT_POLYTYPE_GROUND || pPoly->vertCount < 3)
				continue;

			const int nv = pPoly->vertCount;
			Vector verts[DT_VERTS_PER_POLYGON];
			Vector centroid(0, 0, 0);
			for (int v = 0; v < nv; ++v)
			{
				DetourToSrc(&pTile->verts[pPoly->verts[v] * 3], verts[v]);
				centroid += verts[v];
			}
			centroid /= (float)nv;

			const dtPolyRef srcRef = base | (dtPolyRef)i;

			for (int j = 0; j < nv; ++j)
			{
				if (pPoly->neis[j] != 0)
					continue;   // only border edges

				const Vector& a = verts[j];
				const Vector& b = verts[(j + 1) % nv];

				Vector e = b - a;
				e.z = 0.0f;
				const float len = e.Length();
				if (len < 1.0f)
					continue;

				Vector dir(e.y / len, -e.x / len, 0.0f);   // outward normal in XY
				const Vector mid = (a + b) * 0.5f;
				if (DotProduct(dir, mid - centroid) < 0.0f)
					dir = -dir;

				const int n = MAX(1, (int)(len / step));
				for (int k = 0; k < n; ++k)
				{
					if ((int)(out.size() - before) >= maxLinks)
						break;

					Vector p = a + (b - a) * ((k + 0.5f) / (float)n);
					Vector toC = centroid - p;
					toC.z = 0.0f;
					const float dC = VectorNormalize(toC);
					p += toC * MIN(6.0f, dC);   // nudge onto the poly

					float sp[3];
					SrcToDetour(p, sp);

					for (float d = 24.0f; d <= maxDist; d += 16.0f)
					{
						const Vector probe = p + dir * d;

						// Wall ahead: nothing further out is reachable from here
						if (!TraceClear(p + Vector(0, 0, 40.0f), probe + Vector(0, 0, 40.0f)))
							break;

						Vector land;
						if (!FindFloor(probe, maxUp + 8.0f, maxDrop + 8.0f, land))
							continue;

						const float dz = land.z - p.z;
						if (dz > maxUp || dz < -maxDrop)
							continue;

						float lp[3], nearest[3];
						SrcToDetour(land, lp);
						dtPolyRef dstRef = 0;
						if (dtStatusFailed(pQuery->findNearestPoly(lp, ext, &filter, &dstRef, nearest)) || !dstRef || dstRef == srcRef)
							continue;

						const float hx = nearest[0] - lp[0], hz = nearest[2] - lp[2];
						if (hx * hx + hz * hz > 12.0f * 12.0f || fabsf(nearest[1] - lp[1]) > 20.0f)
							continue;   // floor there isn't on the mesh

						if (EasilyWalkable(pQuery, &filter, srcRef, sp, dstRef, nearest))
							continue;

						Vector landOnMesh;
						DetourToSrc(nearest, landOnMesh);
						if (IsDuplicate(out, p, landOnMesh))
							break;

						FullNavJumpLink_t link;
						link.a = p;
						link.b = landOnMesh;
						link.bBidir = (-dz <= maxUp);   // can it be jumped back up?
						out.push_back(link);
						break;
					}
				}
			}
		}
	}

	dtFreeNavMeshQuery(pQuery);
	Msg("[EZ2 NavMesh] Generated %d jump links from geometry\n", (int)(out.size() - before));
	return (int)(out.size() - before);
}
