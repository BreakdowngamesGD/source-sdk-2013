//=============================================================================
// navmesh_dynamic.cpp
// Runtime state layered over the baked navmesh:
//   - door state   -> per-poly CLEAR / DOOR (cost) / BLOCKED (impassable)
//   - danger zones -> extra path cost near grenades etc.
//   - hint state   -> CAI_Hint enabled/disabled mirrored onto baked hints
//   - prop carves  -> resting physics props carved out of the floor (rebuilds mesh)
//   - failed jumps -> off-mesh jump links an NPC couldn't make are skipped for a while
// Works on both mesh types (full mesh has no baked doors at all).
//=============================================================================
#include "cbase.h"
#include "NavMeshDynamic.h"
#include "ai_basenpc.h"
#include "NavMeshManager.h"
#include "NavMeshBuilder_Node.h"   // EZ2_NAV_AREA_*
//#include "NavMeshBuilder_Full.h"   // EZ2_NavRebuildCarved, EZ2_NavCarve_t
#include "DetourNavMesh.h"
#include "DetourCommon.h"
#include "ai_hint.h"
#include "ai_dynamiclink.h"
#include "ai_link.h"
#include "ai_node.h"
#include "ai_network.h"
#include "basetoggle.h"
#include "props.h"
#include "igamesystem.h"
#include "BasePropDoor.h"
#include "NavMeshCover.h"
#include <vector>
#include <map>
#include <algorithm>
#include "vphysics_interface.h"
// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_door_penalty("ez2_nav_door_penalty", "150", 0, "Path cost (~units of detour) for passing through a closed, openable door");
static ConVar ez2_nav_func_door_blocks("ez2_nav_func_door_blocks", "1", 0, "1 = closed func_door/func_door_rotating are impassable (NPCs can't open them), 0 = treat like openable doors");
static ConVar ez2_nav_prop_carve("ez2_nav_prop_carve", "1", 0, "Carve resting physics props out of the navmesh floor (rebuilds the tile)");
static ConVar ez2_nav_prop_carve_minheight("ez2_nav_prop_carve_minheight", "20", 0, "Resting physics props shorter than this (world AABB height) never count as nav obstacles");
static ConVar ez2_nav_prop_carve_minmass("ez2_nav_prop_carve_minmass", "0", 0, "Resting physics props lighter than this (kg) never count as nav obstacles (0 = no mass filter)");
// Footprints are the exact hull; inflation by the NPC radius happens in EZ2_NavBuildCutoutDetour
static const float PROP_CARVE_SCALE = 1.0f;

namespace
{
	struct DoorRec_t
	{
		EHANDLE          hDoor;
		std::vector<int> polys;
		bool             bMapped;
		DoorRec_t() : bMapped(false) {}
	};

	struct Danger_t
	{
		Vector pos;
		float  radius;
		float  penalty;
		float  expire;
	};

	struct DangerSrc_t { const char* pszClass; float radius; float penalty; };
	static const DangerSrc_t s_DangerSrc[] =
	{
		{ "npc_grenade_frag",  192.0f, 8.0f },
		{ "grenade_ar2",       160.0f, 6.0f },
		{ "prop_combine_ball", 128.0f, 6.0f },
	};

	static const int MAX_DANGERS = 32;
	static const int MAX_FAILED_JUMPS = 64;

	// A jump link (either direction) that an NPC failed or couldn't make
	struct FailedJump_t
	{
		Vector a;
		Vector b;
		float  expire;
	};

	struct PropBox_t
	{
		EHANDLE             hProp;
		Vector              origin;
		QAngle              angles;
		std::vector<Vector> footprint;   // world XY, CCW, projected collision hull, z = base
		float               radius;      // max distance from hull centroid to a hull vertex
		PropBox_t() : origin(vec3_origin), angles(vec3_angle), radius(0.0f) {}
	};

	static dtNavMesh* s_pMesh = NULL;
	static std::vector<unsigned char>  s_PolyState;
	static std::vector<DoorRec_t>      s_Doors;
	static std::vector<Danger_t>       s_ManualDangers;
	static std::vector<Danger_t>       s_AutoDangers;
	static std::vector<FailedJump_t>   s_FailedJumps;
	static std::vector<float>          s_PolyBlockUntil;
	static std::map<CAI_DynamicLink*, std::vector<int> > s_LinkPolys;
	static std::vector<PropBox_t>      s_PropBoxes;
	static float                       s_flNextFast = 0.0f;
	static float                       s_flNextHints = 0.0f;

	inline void DetourToSrc(const float* p, Vector& out)
	{
		out.x = p[0];
		out.y = -p[2];
		out.z = p[1];
	}

	static float Cross2DXY(const Vector& o, const Vector& a, const Vector& b)
	{
		return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
	}

	static bool PointInFootprint(const std::vector<Vector>& fp, const Vector& p)
	{
		const size_t n = fp.size();
		for (size_t i = 0; i < n; ++i)
		{
			if (Cross2DXY(fp[i], fp[(i + 1) % n], p) < 0.0f)
				return false;
		}
		return true;
	}

	static bool SegmentIntersectsFootprintXY(const Vector& a, const Vector& b, const std::vector<Vector>& fp)
	{
		const int n = static_cast<int>(fp.size());
		if (n < 3)
			return false;

		if (PointInFootprint(fp, a) || PointInFootprint(fp, b))
			return true;

		const float dist = a.AsVector2D().DistTo(b.AsVector2D());
		const int steps = clamp(static_cast<int>(ceilf(dist / 16.0f)), 4, 64);
		for (int s = 1; s < steps; ++s)
		{
			const float t = static_cast<float>(s) / static_cast<float>(steps);
			Vector p = a + (b - a) * t;
			if (PointInFootprint(fp, p))
				return true;
		}

		auto ccw = [](const Vector& p1, const Vector& p2, const Vector& p3) -> bool
			{
				return (p3.y - p1.y) * (p2.x - p1.x) > (p2.y - p1.y) * (p3.x - p1.x);
			};
		auto segIntersect = [&](const Vector& p1, const Vector& p2, const Vector& q1, const Vector& q2) -> bool
			{
				return (ccw(p1, q1, q2) != ccw(p2, q1, q2)) && (ccw(p1, p2, q1) != ccw(p1, p2, q2));
			};

		for (int i = 0; i < n; ++i)
		{
			if (segIntersect(a, b, fp[i], fp[(i + 1) % n]))
				return true;
		}

		return false;
	}

	// True if a link between a and b (either direction) is currently blacklisted
	bool IsJumpRecentlyFailed(const Vector& a, const Vector& b)
	{
		if (s_FailedJumps.empty())
			return false;

		const float r2 = 64.0f * 64.0f;
		const float now = gpGlobals->curtime;
		for (size_t i = 0; i < s_FailedJumps.size(); ++i)
		{
			const FailedJump_t& f = s_FailedJumps[i];
			if (f.expire <= now)
				continue;
			if ((f.a.DistToSqr(a) < r2 && f.b.DistToSqr(b) < r2) ||
				(f.a.DistToSqr(b) < r2 && f.b.DistToSqr(a) < r2))
				return true;
		}
		return false;
	}

	// Closed + openable = DOOR, closed + locked/unopenable = BLOCKED, open/not a door = CLEAR
	int GetDoorBlockState(CBaseEntity* pEnt)
	{
		if (FClassnameIs(pEnt, "prop_door_rotating"))
		{
			CBasePropDoor* pDoor = dynamic_cast<CBasePropDoor*>(pEnt);
			if (!pDoor || !pDoor->IsDoorClosed())
				return EZ2_NAVDYN_CLEAR;
			return pDoor->IsDoorLocked() ? EZ2_NAVDYN_BLOCKED : EZ2_NAVDYN_DOOR;
		}

		CBaseToggle* pToggle = dynamic_cast<CBaseToggle*>(pEnt);
		if (!pToggle || pToggle->m_toggle_state != TS_AT_BOTTOM)
			return EZ2_NAVDYN_CLEAR;

		return ez2_nav_func_door_blocks.GetBool() ? EZ2_NAVDYN_BLOCKED : EZ2_NAVDYN_DOOR;
	}

	// Which polys does the (closed) door footprint overlap? Done once per door.
	void MapDoorPolys(const dtMeshTile* pTile, const Vector& mins, const Vector& maxs, std::vector<int>& out)
	{
		const float pad = 24.0f;
		const float fp[12] =
		{
			mins.x - pad, 0.0f, -(mins.y - pad),
			maxs.x + pad, 0.0f, -(mins.y - pad),
			maxs.x + pad, 0.0f, -(maxs.y + pad),
			mins.x - pad, 0.0f, -(maxs.y + pad)
		};

		for (int i = 0; i < pTile->header->polyCount; ++i)
		{
			const dtPoly* p = &pTile->polys[i];
			if (p->getType() != DT_POLYTYPE_GROUND)
				continue;

			float pv[DT_VERTS_PER_POLYGON * 3];
			float minX = FLT_MAX, maxX = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX, sumY = 0.0f;
			for (int v = 0; v < p->vertCount; ++v)
			{
				const float* src = &pTile->verts[p->verts[v] * 3];
				pv[v * 3 + 0] = src[0];
				pv[v * 3 + 1] = src[1];
				pv[v * 3 + 2] = src[2];
				minX = MIN(minX, src[0]); maxX = MAX(maxX, src[0]);
				minZ = MIN(minZ, src[2]); maxZ = MAX(maxZ, src[2]);
				sumY += src[1];
			}

			const float cy = sumY / (float)p->vertCount;   // Detour Y == Source Z
			if (cy < mins.z - 24.0f || cy > maxs.z)
				continue;

			// Huge polys spanning a doorway would penalise a whole room; skip them
			if ((maxX - minX) > 256.0f || (maxZ - minZ) > 256.0f)
				continue;

			if (dtOverlapPolyPoly2D(fp, 4, pv, p->vertCount))
				out.push_back(i);
		}
	}

	// Polys of the node mesh that were baked as DYNLINK corridors along this link
	void MapLinkPolys(const dtMeshTile* t, const Vector& a, const Vector& b, std::vector<int>& out)
	{
		const float pa[3] = { a.x, a.z, -a.y }, pb[3] = { b.x, b.z, -b.y };
		for (int i = 0; i < t->header->polyCount; ++i)
		{
			const dtPoly* p = &t->polys[i];
			if (p->getType() != DT_POLYTYPE_GROUND || p->getArea() != EZ2_NAV_AREA_DYNLINK)
				continue;

			float c[3] = { 0, 0, 0 };
			for (int v = 0; v < p->vertCount; ++v)
				for (int k = 0; k < 3; ++k)
					c[k] += t->verts[p->verts[v] * 3 + k] / (float)p->vertCount;

			float tt;
			if (dtDistancePtSegSqr2D(c, pa, pb, tt) > 48.0f * 48.0f)
				continue;
			if (fabsf(c[1] - (pa[1] + (pb[1] - pa[1]) * tt)) > 64.0f)
				continue;
			out.push_back(i);
		}
	}

	// Live state of info_node_link / link controllers: OFF (and not NPC-restricted) = impassable
	void UpdateDynamicLinks(const dtMeshTile* pTile)
	{
		if (!g_pBigAINet)
			return;

		for (CAI_DynamicLink* p = CAI_DynamicLink::m_pAllDynamicLinks; p; p = p->m_pNextDynamicLink)
		{
			if (p->m_nLinkState != LINK_OFF || p->m_strAllowUse != NULL_STRING)
				continue;
			if (p->m_nSrcID == NO_NODE || p->m_nDestID == NO_NODE)
				continue;

			std::map<CAI_DynamicLink*, std::vector<int> >::iterator it = s_LinkPolys.find(p);
			if (it == s_LinkPolys.end())
			{
				std::vector<int> polys;
				CAI_Node* a = g_pBigAINet->GetNode(p->m_nSrcID, false);
				CAI_Node* b = g_pBigAINet->GetNode(p->m_nDestID, false);
				if (a && b)
					MapLinkPolys(pTile, a->GetOrigin(), b->GetOrigin(), polys);
				it = s_LinkPolys.insert(std::make_pair(p, polys)).first;
			}

			for (size_t k = 0; k < it->second.size(); ++k)
			{
				const int idx = it->second[k];
				if (idx < (int)s_PolyState.size())
					s_PolyState[idx] = (unsigned char)EZ2_NAVDYN_BLOCKED;
			}
		}
	}

	// At rest = asleep (or motion-disabled) and resting on world geometry
	bool PropIsAtRest(CBaseEntity* pEnt)
	{
		IPhysicsObject* pPhys = pEnt->VPhysicsGetObject();
		if (!pPhys)
			return false;
		if (pPhys->IsMoveable() && !pPhys->IsAsleep())
			return false;

		Vector mins, maxs;
		pEnt->CollisionProp()->WorldSpaceAABB(&mins, &maxs);
		const Vector c = pEnt->WorldSpaceCenter();

		trace_t tr;
		UTIL_TraceLine(Vector(c.x, c.y, mins.z + 2.0f), Vector(c.x, c.y, mins.z - 6.0f),
			MASK_NPCSOLID_BRUSHONLY, pEnt, COLLISION_GROUP_NONE, &tr);
		return tr.DidHit() && tr.plane.normal.z > 0.7f;
	}

	// Andrew's monotone chain, XY only, returns CCW hull
	static void HullXY(std::vector<Vector> pts, std::vector<Vector>& hull)
	{
		hull.clear();
		const size_t n = pts.size();
		if (n < 3)
			return;

		std::sort(pts.begin(), pts.end(), [](const Vector& a, const Vector& b)
			{
				return a.x < b.x || (a.x == b.x && a.y < b.y);
			});

		hull.resize(2 * n);
		size_t k = 0;
		for (size_t i = 0; i < n; ++i)
		{
			while (k >= 2 && Cross2DXY(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
			hull[k++] = pts[i];
		}
		for (size_t i = n - 1, t = k + 1; i > 0; --i)
		{
			while (k >= t && Cross2DXY(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0) --k;
			hull[k++] = pts[i - 1];
		}
		hull.resize(k - 1);
	}

	// Footprint = projection of the prop's current collision hull onto the floor, scaled 1.1x.
	// Uses the physics collide when available, otherwise the OBB.
	static bool BuildPropFootprint(CBaseEntity* pEnt, PropBox_t& out)
	{
		const matrix3x4_t& mat = pEnt->EntityToWorldTransform();
		std::vector<Vector> pts;

		IPhysicsObject* pPhys = pEnt->VPhysicsGetObject();
		CPhysCollide* pCollide = pPhys ? const_cast<CPhysCollide*>(pPhys->GetCollide()) : NULL;
		if (pCollide && physcollision)
		{
			Vector* pVerts = NULL;
			const int nVerts = physcollision->CreateDebugMesh(pCollide, &pVerts);
			for (int i = 0; i < nVerts && pVerts; ++i)
			{
				Vector w;
				VectorTransform(pVerts[i], mat, w);
				pts.push_back(w);
			}
			if (pVerts)
				physcollision->DestroyDebugMesh(nVerts, pVerts);
		}

		if (pts.size() < 3)
		{
			const Vector mins = pEnt->CollisionProp()->OBBMins();
			const Vector maxs = pEnt->CollisionProp()->OBBMaxs();
			for (int i = 0; i < 8; ++i)
			{
				const Vector c((i & 1) ? maxs.x : mins.x, (i & 2) ? maxs.y : mins.y, (i & 4) ? maxs.z : mins.z);
				Vector w;
				VectorTransform(c, mat, w);
				pts.push_back(w);
			}
		}

		float zMin = FLT_MAX;
		for (size_t i = 0; i < pts.size(); ++i)
			zMin = MIN(zMin, pts[i].z);

		std::vector<Vector> hull;
		HullXY(pts, hull);
		if (hull.size() < 3)
			return false;

		Vector c(0, 0, 0);
		for (size_t i = 0; i < hull.size(); ++i)
			c += hull[i];
		c /= (float)hull.size();

		out.footprint.clear();
		out.radius = 0.0f;
		for (size_t i = 0; i < hull.size(); ++i)
		{
			Vector p = c + (hull[i] - c) * PROP_CARVE_SCALE;
			p.z = zMin;
			out.footprint.push_back(p);
			out.radius = MAX(out.radius, Vector(p.x - c.x, p.y - c.y, 0.0f).Length());
		}
		return true;
	}

	// True if p is on the floor under a resting prop's footprint
	bool PointInPropBoxes(const Vector& p)
	{
		for (size_t i = 0; i < s_PropBoxes.size(); ++i)
		{
			const std::vector<Vector>& fp = s_PropBoxes[i].footprint;
			if (fp.empty() || p.z < fp[0].z - 24.0f || p.z > fp[0].z + 24.0f)
				continue;
			if (PointInFootprint(fp, p))
				return true;
		}
		return false;
	}

	// Poly is only hard-rejected if it is essentially all under props (centroid, every vertex and every edge midpoint)
	static bool PolyFullyCovered(const dtMeshTile* tile, const dtPoly* poly)
	{
		Vector v[DT_VERTS_PER_POLYGON], cen(0, 0, 0);
		const int n = poly->vertCount;
		for (int i = 0; i < n; ++i)
		{
			DetourToSrc(&tile->verts[poly->verts[i] * 3], v[i]);
			cen += v[i];
		}
		cen /= (float)n;
		if (!PointInPropBoxes(cen))
			return false;
		for (int i = 0; i < n; ++i)
		{
			if (!PointInPropBoxes(v[i]) || !PointInPropBoxes((v[i] + v[(i + 1) % n]) * 0.5f))
				return false;
		}
		return true;
	}

	// Estimated cost of walking a->b through a poly that a prop sits in: roughly the detour length
	static float PropCrossPenalty(const Vector& a, const Vector& b)
	{
		float pen = 0.0f;
		const Vector ab = b - a;
		const float abLen2 = ab.x * ab.x + ab.y * ab.y;
		for (size_t i = 0; i < s_PropBoxes.size(); ++i)
		{
			const PropBox_t& pb = s_PropBoxes[i];
			if (pb.footprint.size() < 3) continue;
			const float z = pb.footprint[0].z;
			if (MAX(a.z, b.z) < z - 24.0f || MIN(a.z, b.z) > z + 24.0f) continue;

			// cheap reject: segment further from the prop centre than its bounding radius
			Vector c(0, 0, 0);
			for (size_t k = 0; k < pb.footprint.size(); ++k) { c.x += pb.footprint[k].x; c.y += pb.footprint[k].y; }
			c.x /= (float)pb.footprint.size(); c.y /= (float)pb.footprint.size();
			float t = (abLen2 > 1e-4f) ? ((c.x - a.x) * ab.x + (c.y - a.y) * ab.y) / abLen2 : 0.0f;
			t = clamp(t, 0.0f, 1.0f);
			const float dx = a.x + ab.x * t - c.x, dy = a.y + ab.y * t - c.y;
			if (dx * dx + dy * dy > (pb.radius + 1.0f) * (pb.radius + 1.0f)) continue;

			if (SegmentIntersectsFootprintXY(a, b, pb.footprint))
				pen += pb.radius * 3.0f;
		}
		return pen;
	}

	// Expands a CCW convex footprint outward by d (clamped miter)
	static void InflateConvex(const std::vector<Vector>& fp, float d, std::vector<Vector>& out)
	{
		const size_t n = fp.size();
		out.resize(n);
		for (size_t i = 0; i < n; ++i)
		{
			const Vector& p0 = fp[(i + n - 1) % n]; const Vector& p1 = fp[i]; const Vector& p2 = fp[(i + 1) % n];
			Vector e0 = p1 - p0, e1 = p2 - p1; e0.z = e1.z = 0;
			VectorNormalize(e0); VectorNormalize(e1);
			Vector m(e0.y + e1.y, -(e0.x + e1.x), 0);                 // sum of outward normals (CCW hull)
			const float ml = VectorNormalize(m);
			const Vector n0(e0.y, -e0.x, 0);
			const float k = (ml > 0.1f) ? d / MAX(DotProduct(m, n0), 0.5f) : d;   // clamped miter
			out[i] = p1 + m * k;
			out[i].z = p1.z;
		}
	}

	// Strict interior test: grazing an edge or touching a vertex counts as clear
	static bool SegmentThroughConvex(const std::vector<Vector>& poly, const Vector& a, const Vector& b, float* pEnterT = NULL)
	{
		const size_t n = poly.size();
		float t0 = 0.0f, t1 = 1.0f;
		const float dx = b.x - a.x, dy = b.y - a.y;
		for (size_t i = 0; i < n; ++i)
		{
			const Vector& p = poly[i]; const Vector& q = poly[(i + 1) % n];
			float ex = q.x - p.x, ey = q.y - p.y;
			const float len = sqrtf(ex * ex + ey * ey);
			if (len < 1e-3f) continue;
			ex /= len; ey /= len;
			const float fa = ex * (a.y - p.y) - ey * (a.x - p.x);     // > 0 inside
			const float fd = ex * dy - ey * dx;
			if (fabsf(fd) < 1e-4f)
			{
				if (fa <= 0.05f) return false;
				continue;
			}
			const float t = -fa / fd;
			if (fd > 0.0f) t0 = MAX(t0, t); else t1 = MIN(t1, t);
			if (t1 - t0 <= 1e-3f) return false;
		}
		if (pEnterT) *pEnterT = t0;
		return true;
	}

	static bool PointOnNavMesh(const Vector& p)
	{
		dtNavMeshQuery* q = g_EZ2NavMeshManager.GetNavMeshQuery();
		if (!q) return true;
		const float c[3] = { p.x, p.z, -p.y }, ext[3] = { 16.0f, 48.0f, 16.0f };
		dtQueryFilter f; dtPolyRef ref = 0; float nearest[3] = { 0, 0, 0 };
		return dtStatusSucceed(q->findNearestPoly(c, ext, &f, &ref, nearest)) && ref &&
			(Square(nearest[0] - c[0]) + Square(nearest[2] - c[2])) < Square(12.0f);
	}

	static Vector FootprintCentroid(const std::vector<Vector>& fp)
	{
		Vector c(0, 0, fp.empty() ? 0.0f : fp[0].z);
		for (size_t k = 0; k < fp.size(); ++k) { c.x += fp[k].x; c.y += fp[k].y; }
		if (!fp.empty()) { c.x /= (float)fp.size(); c.y /= (float)fp.size(); }
		return c;
	}

	static bool PoseMatches(const PropBox_t& b, CBaseEntity* pEnt)
	{
		return b.hProp.Get() == pEnt &&
			b.origin.DistToSqr(pEnt->GetAbsOrigin()) < 0.25f &&
			fabsf(b.angles.x - pEnt->GetAbsAngles().x) < 0.5f &&
			fabsf(b.angles.y - pEnt->GetAbsAngles().y) < 0.5f &&
			fabsf(b.angles.z - pEnt->GetAbsAngles().z) < 0.5f;
	}

	// Rebuilds the box list each tick. Footprints are reused while a prop's pose is unchanged.
	void UpdatePropCutouts()
	{
		std::vector<PropBox_t> next;
		if (ez2_nav_prop_carve.GetBool())
		{
			static const char* s_PropClasses[] =
			{
				"prop_physics", "prop_physics_multiplayer", "prop_physics_override",
				"prop_physics_respawnable", "physics_prop"
			};

			for (int c = 0; c < ARRAYSIZE(s_PropClasses); ++c)
			{
				for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, s_PropClasses[c]); pEnt;
					pEnt = gEntList.FindEntityByClassname(pEnt, s_PropClasses[c]))
				{
					if (pEnt->IsSolidFlagSet(FSOLID_NOT_SOLID) || pEnt->IsEffectActive(EF_NODRAW) ||
						pEnt->GetCollisionGroup() == COLLISION_GROUP_DEBRIS)
						continue;

					Vector pmin, pmax;
					pEnt->CollisionProp()->WorldSpaceAABB(&pmin, &pmax);
					if (pmax.z - pmin.z < ez2_nav_prop_carve_minheight.GetFloat())
						continue;

					IPhysicsObject* pPropPhys = pEnt->VPhysicsGetObject();
					if (pPropPhys && pPropPhys->GetMass() < ez2_nav_prop_carve_minmass.GetFloat())
						continue;

					if (!PropIsAtRest(pEnt))
						continue;

					PropBox_t b;
					bool bReused = false;
					for (size_t k = 0; k < s_PropBoxes.size() && !bReused; ++k)
					{
						if (PoseMatches(s_PropBoxes[k], pEnt))
						{
							b = s_PropBoxes[k];
							bReused = true;
						}
					}

					if (!bReused)
					{
						b.hProp = pEnt;
						b.origin = pEnt->GetAbsOrigin();
						b.angles = pEnt->GetAbsAngles();
						if (!BuildPropFootprint(pEnt, b))
							continue;
					}
					next.push_back(b);
				}
			}
		}
		s_PropBoxes.swap(next);
	}

	void UpdateDoors(dtNavMesh* pMesh)
	{
		const dtMeshTile* pTile = static_cast<const dtNavMesh*>(pMesh)->getTile(0);
		if (!pTile || !pTile->header)
			return;

		if (pMesh != s_pMesh)
		{
			s_pMesh = pMesh;
			s_Doors.clear();
			s_PolyState.assign(pTile->header->polyCount, 0);
			s_PolyBlockUntil.assign(pTile->header->polyCount, 0.0f);
			s_LinkPolys.clear();
			s_FailedJumps.clear();   // new mesh: old link positions may no longer exist
		}

		for (int i = (int)s_Doors.size() - 1; i >= 0; --i)
		{
			if (!s_Doors[i].hDoor.Get())
				s_Doors.erase(s_Doors.begin() + i);
		}

		static const char* s_Classes[] = { "prop_door_rotating", "func_door", "func_door_rotating" };
		for (int c = 0; c < 3; ++c)
		{
			for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, s_Classes[c]); pEnt; pEnt = gEntList.FindEntityByClassname(pEnt, s_Classes[c]))
			{
				bool bKnown = false;
				for (size_t i = 0; i < s_Doors.size(); ++i)
				{
					if (s_Doors[i].hDoor.Get() == pEnt) { bKnown = true; break; }
				}
				if (!bKnown)
				{
					DoorRec_t rec;
					rec.hDoor = pEnt;
					s_Doors.push_back(rec);
				}
			}
		}

		std::fill(s_PolyState.begin(), s_PolyState.end(), (unsigned char)0);

		for (size_t i = 0; i < s_Doors.size(); ++i)
		{
			DoorRec_t& rec = s_Doors[i];
			CBaseEntity* pDoor = rec.hDoor.Get();
			if (!pDoor)
				continue;

			const int state = GetDoorBlockState(pDoor);
			if (state == EZ2_NAVDYN_CLEAR)
				continue;

			// Map polys the first time we see the door at rest/closed (bounds are valid then)
			if (!rec.bMapped)
			{
				Vector mins, maxs;
				pDoor->CollisionProp()->WorldSpaceSurroundingBounds(&mins, &maxs);
				MapDoorPolys(pTile, mins, maxs, rec.polys);
				rec.bMapped = true;
			}

			for (size_t k = 0; k < rec.polys.size(); ++k)
			{
				const int idx = rec.polys[k];
				if (idx < (int)s_PolyState.size() && s_PolyState[idx] < state)
					s_PolyState[idx] = (unsigned char)state;
			}
		}
		UpdateDynamicLinks(pTile);
	}

	void UpdateDangers()
	{
		const float now = gpGlobals->curtime;

		for (int i = (int)s_ManualDangers.size() - 1; i >= 0; --i)
		{
			if (s_ManualDangers[i].expire <= now)
				s_ManualDangers.erase(s_ManualDangers.begin() + i);
		}

		// Drop expired jump blacklist entries
		for (int i = (int)s_FailedJumps.size() - 1; i >= 0; --i)
		{
			if (s_FailedJumps[i].expire <= now)
				s_FailedJumps.erase(s_FailedJumps.begin() + i);
		}

		s_AutoDangers.clear();
		for (int c = 0; c < ARRAYSIZE(s_DangerSrc); ++c)
		{
			for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, s_DangerSrc[c].pszClass); pEnt; pEnt = gEntList.FindEntityByClassname(pEnt, s_DangerSrc[c].pszClass))
			{
				if ((int)s_AutoDangers.size() >= MAX_DANGERS)
					return;

				Danger_t d;
				d.pos = pEnt->GetAbsOrigin();
				d.radius = s_DangerSrc[c].radius;
				d.penalty = s_DangerSrc[c].penalty;
				d.expire = 0.0f;
				s_AutoDangers.push_back(d);
			}
		}
	}

	// Mirror CAI_Hint enabled/disabled state onto baked hints (matched by Hammer ID)
	void UpdateHints()
	{
		std::vector<EZ2_NavHint_t>& hints = g_EZ2NavMeshManager.AccessNavHints();
		if (hints.empty())
			return;

		std::map<int, CAI_Hint*> live;
		AIHintIter_t it;
		for (CAI_Hint* p = CAI_HintManager::GetFirstHint(&it); p; p = CAI_HintManager::GetNextHint(&it))
			live[p->GetWCId()] = p;

		for (size_t i = 0; i < hints.size(); ++i)
		{
			EZ2_NavHint_t& h = hints[i];
			if (h.sourceWCNodeID <= 0)
				continue;   // generated cover hints have no source entity

			std::map<int, CAI_Hint*>::const_iterator f = live.find(h.sourceWCNodeID);
			if (f == live.end())
			{
				h.bDisabled = true;   // entity gone
				continue;
			}

			CAI_Hint* pHint = f->second;
			h.bDisabled = pHint->IsDisabled();

			// Follow the entity if it moved (e.g. parented to a physics prop)
			const Vector org = pHint->GetAbsOrigin();
			if (h.vecLiveOrigin == vec3_invalid)
			{
				h.vecLiveOrigin = org;   // first sight: baked position is still valid
				continue;
			}
			if (org.DistToSqr(h.vecLiveOrigin) < 4.0f)
				continue;

			h.vecLiveOrigin = org;

			Vector pos;
			pHint->GetPosition(HULL_HUMAN, &pos);
			trace_t tr;
			UTIL_TraceLine(pos + Vector(0, 0, 16), pos - Vector(0, 0, 64), MASK_NPCSOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &tr);
			h.vecPos = tr.DidHit() ? tr.endpos : pos;
			h.flYaw = pHint->Yaw();
			g_EZ2NavMeshManager.SnapHintToMesh(h);   // polyRef == 0 => skipped by queries until it lands on mesh again
		}
	}

	void ResetAll()
	{
		s_pMesh = NULL;
		s_PolyState.clear();
		s_Doors.clear();
		s_ManualDangers.clear();
		s_AutoDangers.clear();
		s_FailedJumps.clear();
		s_flNextFast = 0.0f;
		s_flNextHints = 0.0f;
		s_PolyBlockUntil.clear();
		s_LinkPolys.clear();
		s_PropBoxes.clear();
	}
} // end anonymous namespace

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------
void EZ2_NavUpdatePropObstacle(CBaseEntity* pProp)
{
	if (!pProp || !gpGlobals)
		return;

	if (dynamic_cast<CPhysicsProp*>(pProp) != NULL)
		return;

	const bool bShouldBlock = !pProp->IsEffectActive(EF_NODRAW) && !pProp->IsSolidFlagSet(FSOLID_NOT_SOLID);
	if (!bShouldBlock)
		return;

	const Vector mins = pProp->WorldAlignMins();
	const Vector maxs = pProp->WorldAlignMaxs();
	const Vector center = pProp->WorldSpaceCenter();
	const Vector ext = (maxs - mins) * 0.5f;
	const float radius = MAX(ext.x, ext.y);
	if (radius < 16.0f)
		return;

	EZ2_NavMarkBlocked(center, radius, 3600.0f);
}

void EZ2_NavCollectDynamicCoverAroundOrigin(const Vector& origin, float radius, std::vector<EZ2_NavDynCoverSpot_t>& outSpots, const Vector* pThreat)
{
	outSpots.clear();

	std::vector<EZ2_CoverSpot_t> rawSpots;
	const Vector vecThreat = pThreat ? *pThreat : origin + Vector(256.0f, 0.0f, 0.0f);
	FindCoverSpotsAroundOrigin(origin, vecThreat, radius, 64, rawSpots, false);

	outSpots.reserve(rawSpots.size());
	for (size_t i = 0; i < rawSpots.size(); ++i)
	{
		const EZ2_CoverSpot_t& src = rawSpots[i];
		EZ2_NavDynCoverSpot_t dst;
		dst.vecPos = src.vecPos;
		dst.vecCoverDir = src.vecCoverNormal;
		dst.flYaw = UTIL_VecToYaw(src.vecCoverNormal);
		dst.flHeight = src.flObstacleHeight;
		dst.bCrouchOnly = (src.kind == EZ2_COVER_LOW_SHORT);
		dst.hSourceEnt = NULL;
		outSpots.push_back(dst);
	}
}

int EZ2_NavGetDynState(int polyIndex)
{
	if (polyIndex < 0)
		return EZ2_NAVDYN_CLEAR;
	if (polyIndex < (int)s_PolyBlockUntil.size() && s_PolyBlockUntil[polyIndex] > gpGlobals->curtime)
		return EZ2_NAVDYN_BLOCKED;
	return (polyIndex < (int)s_PolyState.size()) ? s_PolyState[polyIndex] : EZ2_NAVDYN_CLEAR;
}

bool EZ2_NavPointInPropCutout(const Vector& p)
{
	return PointInPropBoxes(p);
}

void EZ2_NavGetPropCutoutFootprints(std::vector<std::vector<Vector> >& out)
{
	out.clear();
	for (size_t i = 0; i < s_PropBoxes.size(); ++i)
		out.push_back(s_PropBoxes[i].footprint);
}

// 0 = leg clear, 1 = out[] holds waypoints (excluding a, b), -1 = blocked, no way around
int EZ2_NavBuildCutoutDetour(const Vector& a, const Vector& b, float clearance, std::vector<Vector>& out, Vector* pBlockCenter, float* pBlockRadius)
{
	out.clear();
	if (s_PropBoxes.empty())
		return 0;

	int best = -1; float bestT = FLT_MAX; std::vector<Vector> P, bestP;
	for (size_t i = 0; i < s_PropBoxes.size(); ++i)
	{
		const std::vector<Vector>& fp = s_PropBoxes[i].footprint;
		if (fp.size() < 3) continue;
		const float z = fp[0].z;
		if (MAX(a.z, b.z) < z - 24.0f || MIN(a.z, b.z) > z + 36.0f) continue;

		// If an endpoint already sits inside the inflated footprint no wrap-around can clear it,
		// so shrink the inflation for this prop until both endpoints are outside (or give up on it).
		static const float s_Scales[] = { 1.0f, 0.5f, 0.1f };
		bool bUsable = false;
		for (int s = 0; s < ARRAYSIZE(s_Scales) && !bUsable; ++s)
		{
			InflateConvex(fp, clearance * s_Scales[s], P);
			bUsable = !PointInFootprint(P, a) && !PointInFootprint(P, b);
		}
		if (!bUsable) continue;

		float t;
		if (SegmentThroughConvex(P, a, b, &t) && t < bestT) { bestT = t; best = (int)i; bestP = P; }
	}
	if (best < 0) return 0;

	Vector dir = b - a; dir.z = 0;
	VectorNormalize(dir);
	float bestLen = FLT_MAX;
	for (int side = -1; side <= 1; side += 2)
	{
		std::vector<Vector> chain, pts;
		for (size_t k = 0; k < bestP.size(); ++k)
		{
			const Vector r = bestP[k] - a;
			if ((dir.x * r.y - dir.y * r.x) * side > 0.0f)
				chain.push_back(bestP[k]);
		}
		if (chain.empty()) continue;
		std::sort(chain.begin(), chain.end(), [&](const Vector& l, const Vector& r)
			{ return DotProduct(l - a, dir) < DotProduct(r - a, dir); });

		Vector p = a;
		size_t i = 0;
		while (SegmentThroughConvex(bestP, p, b) && i < chain.size() && pts.size() < 8)
		{
			size_t j = i;                                             // fallback: take the next vertex
			for (size_t k = chain.size(); k-- > i; )
				if (!SegmentThroughConvex(bestP, p, chain[k])) { j = k; break; }
			pts.push_back(chain[j]);
			p = chain[j];
			i = j + 1;
		}
		if (SegmentThroughConvex(bestP, p, b)) continue;

		bool bOk = true; float len = 0.0f; Vector prev = a;
		for (size_t k = 0; k < pts.size(); ++k)
		{
			if (!PointOnNavMesh(pts[k])) { bOk = false; break; }
			len += prev.AsVector2D().DistTo(pts[k].AsVector2D());
			prev = pts[k];
		}
		if (!bOk) continue;
		len += prev.AsVector2D().DistTo(b.AsVector2D());
		if (len < bestLen) { bestLen = len; out = pts; }
	}

	if (!out.empty()) return 1;
	if (pBlockCenter) *pBlockCenter = FootprintCentroid(s_PropBoxes[best].footprint);
	if (pBlockRadius) *pBlockRadius = s_PropBoxes[best].radius;
	return -1;
}

// Legacy single-point API, kept so any other callers still link: returns the first wrap-around waypoint
bool EZ2_NavFindCutoutDetour(const Vector& a, const Vector& b, float clearance, Vector& outPt)
{
	std::vector<Vector> pts;
	if (EZ2_NavBuildCutoutDetour(a, b, clearance, pts, NULL, NULL) > 0 && !pts.empty())
	{
		outPt = pts[0];
		return true;
	}
	return false;
}

CBaseEntity* EZ2_NavGetDoorForPoly(int polyIndex)
{
	for (size_t i = 0; i < s_Doors.size(); ++i)
	{
		CBaseEntity* pDoor = s_Doors[i].hDoor.Get();
		if (!pDoor || !s_Doors[i].bMapped || GetDoorBlockState(pDoor) != EZ2_NAVDYN_DOOR)
			continue;
		for (size_t k = 0; k < s_Doors[i].polys.size(); ++k)
			if (s_Doors[i].polys[k] == polyIndex)
				return pDoor;
	}
	return NULL;
}

void EZ2_NavMarkBlocked(const Vector& pos, float radius, float duration)
{
	dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();
	const dtMeshTile* pTile = pMesh ? static_cast<const dtNavMesh*>(pMesh)->getTile(0) : NULL;
	if (!pQuery || !pTile || !pTile->header)
		return;

	if ((int)s_PolyBlockUntil.size() != pTile->header->polyCount)
		s_PolyBlockUntil.assign(pTile->header->polyCount, 0.0f);

	const float c[3] = { pos.x, pos.z, -pos.y };
	const float ext[3] = { radius, 48.0f, radius };
	dtQueryFilter f;
	dtPolyRef refs[16];
	int n = 0;
	if (dtStatusFailed(pQuery->queryPolygons(c, ext, &f, refs, &n, 16)))
		return;

	for (int i = 0; i < n; ++i)
	{
		const int idx = (int)pMesh->decodePolyIdPoly(refs[i]);
		if (idx < (int)s_PolyBlockUntil.size())
			s_PolyBlockUntil[idx] = gpGlobals->curtime + duration;
	}
}

// Blacklists one jump link (matched in either direction) so path queries skip it until it expires.
// This works on the link itself rather than the polys near it, so ground routes to the same spot stay open.
void EZ2_NavMarkJumpFailed(const Vector& a, const Vector& b, float duration)
{
	if (!gpGlobals)
		return;

	const float now = gpGlobals->curtime;

	// Refresh an existing entry instead of stacking duplicates
	const float r2 = 64.0f * 64.0f;
	for (size_t i = 0; i < s_FailedJumps.size(); ++i)
	{
		FailedJump_t& f = s_FailedJumps[i];
		if ((f.a.DistToSqr(a) < r2 && f.b.DistToSqr(b) < r2) ||
			(f.a.DistToSqr(b) < r2 && f.b.DistToSqr(a) < r2))
		{
			f.expire = MAX(f.expire, now + duration);
			return;
		}
	}

	for (int i = (int)s_FailedJumps.size() - 1; i >= 0; --i)
	{
		if (s_FailedJumps[i].expire <= now)
			s_FailedJumps.erase(s_FailedJumps.begin() + i);
	}

	if ((int)s_FailedJumps.size() >= MAX_FAILED_JUMPS)
		s_FailedJumps.erase(s_FailedJumps.begin());

	FailedJump_t f;
	f.a = a;
	f.b = b;
	f.expire = now + duration;
	s_FailedJumps.push_back(f);
}

int EZ2_NavGetDynStateByRef(unsigned int polyRef)
{
	dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	if (!pMesh || !polyRef)
		return EZ2_NAVDYN_CLEAR;
	return EZ2_NavGetDynState((int)pMesh->decodePolyIdPoly((dtPolyRef)polyRef));
}

void EZ2_NavAddDanger(const Vector& pos, float radius, float duration, float penalty)
{
	if ((int)s_ManualDangers.size() >= MAX_DANGERS)
		s_ManualDangers.erase(s_ManualDangers.begin());

	Danger_t d;
	d.pos = pos;
	d.radius = MAX(radius, 1.0f);
	d.penalty = penalty;
	d.expire = gpGlobals->curtime + duration;
	s_ManualDangers.push_back(d);
}

//-----------------------------------------------------------------------------
// Query filter
//-----------------------------------------------------------------------------
CEZ2_NavFilter::CEZ2_NavFilter(bool bCanOpenDoors, bool bCanJump, CAI_BaseNPC* pNPC, bool bCanClimb)
	: m_bCanOpenDoors(bCanOpenDoors)
	, m_bCanJump(bCanJump)
	, m_pNPC(pNPC)
	, m_bCanClimb(bCanClimb)
{
	setAreaCost(EZ2_NAV_AREA_DYNLINK, 3.0f);
	setAreaCost(EZ2_NAV_AREA_JUMP, 2.0f);
	setAreaCost(EZ2_NAV_AREA_CLIMB, 2.0f);   // matches CAI_Navigator::MovementCost doubling jump/climb
}

bool CEZ2_NavFilter::passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const
{
	if (!dtQueryFilter::passFilter(ref, tile, poly))
		return false;

	const bool bOffMesh = (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION);
	const bool bClimb = bOffMesh && poly->getArea() == EZ2_NAV_AREA_CLIMB;

	if (bClimb && !m_bCanClimb)
		return false;
	if (!bClimb && !m_bCanJump && (bOffMesh || poly->getArea() == EZ2_NAV_AREA_JUMP))
		return false;

	if (bOffMesh)
	{
		const int idx = (int)(poly - tile->polys) - tile->header->offMeshBase;
		if (idx >= 0 && idx < tile->header->offMeshConCount)
		{
			const dtOffMeshConnection* con = &tile->offMeshCons[idx];
			Vector a, b;
			DetourToSrc(&con->pos[0], a);
			DetourToSrc(&con->pos[3], b);

			// Failed ladders share the blacklist with failed jumps
			if (IsJumpRecentlyFailed(a, b))
				return false;

			// Jump reach limits only apply to jumps, not ladders
			if (!bClimb && m_pNPC && !m_pNPC->IsJumpLegal(a, b, b) && !m_pNPC->IsJumpLegal(b, a, a))
				return false;
		}
	}

	const int state = EZ2_NavGetDynState((int)(poly - tile->polys));
	if (state == EZ2_NAVDYN_BLOCKED)
		return false;
	if (state == EZ2_NAVDYN_DOOR && !m_bCanOpenDoors)
		return false;

	if (!s_PropBoxes.empty() && !bOffMesh && PolyFullyCovered(tile, poly))
		return false;

	return true;
}

float CEZ2_NavFilter::getCost(const float* pa, const float* pb,
	const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
	const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly,
	const dtPolyRef nextRef, const dtMeshTile* nextTile, const dtPoly* nextPoly) const
{
	float cost = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef, nextTile, nextPoly);

	if (curTile && curPoly && EZ2_NavGetDynState((int)(curPoly - curTile->polys)) == EZ2_NAVDYN_DOOR)
		cost += ez2_nav_door_penalty.GetFloat();

	if (!s_ManualDangers.empty() || !s_AutoDangers.empty())
	{
		const float mid[3] = { (pa[0] + pb[0]) * 0.5f, (pa[1] + pb[1]) * 0.5f, (pa[2] + pb[2]) * 0.5f };
		Vector vecMid;
		DetourToSrc(mid, vecMid);
		const float len = dtVdist(pa, pb);

		for (int pass = 0; pass < 2; ++pass)
		{
			const std::vector<Danger_t>& list = pass ? s_AutoDangers : s_ManualDangers;
			for (size_t i = 0; i < list.size(); ++i)
			{
				const float d = vecMid.DistTo(list[i].pos);
				if (d < list[i].radius)
					cost += len * list[i].penalty * (1.0f - d / list[i].radius);
			}
		}
	}

	if (!s_PropBoxes.empty() && curPoly && curPoly->getType() != DT_POLYTYPE_OFFMESH_CONNECTION)
	{
		Vector sa, sb;
		DetourToSrc(pa, sa);
		DetourToSrc(pb, sb);
		cost += PropCrossPenalty(sa, sb);
	}

	return cost;
}

//-----------------------------------------------------------------------------
// Per-frame refresh
//-----------------------------------------------------------------------------
class CEZ2_NavDynamicSystem : public CAutoGameSystemPerFrame
{
public:
	CEZ2_NavDynamicSystem() : CAutoGameSystemPerFrame("EZ2_NavDynamic") {}

	virtual void LevelShutdownPostEntity() OVERRIDE { ResetAll(); }

	virtual void FrameUpdatePostEntityThink() OVERRIDE
	{
		if (!g_EZ2NavMeshManager.IsReady())
			return;

		const float now = gpGlobals->curtime;

		if (now >= s_flNextFast)
		{
			s_flNextFast = now + 0.25f;
			// Carve first: a rebuild replaces the tile, so door and link polys must be mapped after it
			UpdatePropCutouts();
			UpdateDoors(g_EZ2NavMeshManager.GetNavMesh());
			UpdateDangers();
		}

		if (now >= s_flNextHints)
		{
			s_flNextHints = now + 1.0f;
			UpdateHints();
		}
	}
};

static CEZ2_NavDynamicSystem s_EZ2NavDynamicSystem;

CON_COMMAND(ez2_nav_danger_add, "Adds a temporary danger zone at the crosshair: ez2_nav_danger_add [radius] [seconds] [penalty]")
{
	CBasePlayer* pPlayer = UTIL_GetCommandClient();
	if (!pPlayer)
		pPlayer = UTIL_GetLocalPlayer();
	if (!pPlayer)
		return;

	Vector vecForward;
	pPlayer->EyeVectors(&vecForward);

	trace_t tr;
	UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + vecForward * 4096.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr);

	const float radius = (args.ArgC() >= 2) ? atof(args[1]) : 192.0f;
	const float secs = (args.ArgC() >= 3) ? atof(args[2]) : 20.0f;
	const float penalty = (args.ArgC() >= 4) ? atof(args[3]) : 8.0f;

	EZ2_NavAddDanger(tr.endpos, radius, secs, penalty);
	Msg("[EZ2 NavMesh] Danger zone at (%.0f, %.0f, %.0f) r=%.0f for %.0fs\n", tr.endpos.x, tr.endpos.y, tr.endpos.z, radius, secs);
}

CON_COMMAND(ez2_nav_jump_blacklist_clear, "Clears the list of jump links NPCs have failed")
{
	s_FailedJumps.clear();
}