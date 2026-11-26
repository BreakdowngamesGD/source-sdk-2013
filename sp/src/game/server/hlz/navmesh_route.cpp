//=============================================================================
// navmesh_route.cpp
// Builds AI_Waypoint_t routes from Detour paths so CAI_Navigator follows them.
//   - off-mesh connections -> NAV_JUMP waypoints
//   - closed prop doors on the path -> bits_WP_TO_DOOR waypoints (navigator opens them)
//=============================================================================
#include "cbase.h"
#include "ai_basenpc.h"
#include "ai_waypoint.h"
#include "NavMeshManager.h"
#include "NavMeshDynamic.h"
#include "NavMeshBuilder_Node.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"
#include "ai_waypoint.h"
#include "ai_node.h"
#include <vector>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_use("ez2_nav_use", "1", 0, "Route ground NPCs through the navmesh when one is loaded");
static ConVar ez2_nav_max_hull_width("ez2_nav_max_hull_width", "40", 0, "NPCs with a wider hull than this keep using the nodegraph (mesh is baked for one agent size)");

namespace
{
	struct WP_t
	{
		Vector       pos;
		Navigation_t nav;
		int          flags;
		EHANDLE      door;
		WP_t() : pos(vec3_origin), nav(NAV_GROUND), flags(0) {}
	};

	inline void DetourToSrc(const float* p, Vector& o) { o.x = p[0]; o.y = -p[2]; o.z = p[1]; }

	void SnapFloor(Vector& p)
	{
		trace_t tr;
		UTIL_TraceLine(p + Vector(0, 0, 24), p - Vector(0, 0, 64), MASK_NPCSOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &tr);
		if (!tr.startsolid && tr.DidHit())
			p = tr.endpos;
	}
}

bool EZ2_NavShouldHandleNPC(CAI_BaseNPC* pNPC)
{
	if (!pNPC || !ez2_nav_use.GetBool() || !g_EZ2NavMeshManager.ShouldUseNavMesh())
		return false;

	const int caps = pNPC->CapabilitiesGet();
	if (!(caps & bits_CAP_MOVE_GROUND) || (caps & (bits_CAP_MOVE_FLY | bits_CAP_MOVE_SWIM)))
		return false;

	return NAI_Hull::Width(pNPC->GetHullType()) <= ez2_nav_max_hull_width.GetFloat();
}

AI_Waypoint_t* EZ2_NavBuildRoute(CAI_BaseNPC* pNPC, const Vector& vStart, const Vector& vEnd, float goalTolerance)
{
	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();
	dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	if (!pQuery || !pMesh || !pNPC)
		return NULL;

	const int caps = pNPC->CapabilitiesGet();
	float sp[3] = { vStart.x, vStart.z, -vStart.y };
	float ep[3] = { vEnd.x, vEnd.z, -vEnd.y };
	const float ext[3] = { 48.0f, 96.0f, 48.0f };

	dtQueryFilter snapFilter;   // plain: lets endpoints snap onto door polys
	CEZ2_NavFilter filter((caps & bits_CAP_DOORS_GROUP) != 0, (caps & bits_CAP_MOVE_JUMP) != 0);

	dtPolyRef startRef = 0, endRef = 0;
	float nStart[3], nEnd[3];
	if (dtStatusFailed(pQuery->findNearestPoly(sp, ext, &snapFilter, &startRef, nStart)) || !startRef)
		return NULL;
	if (dtStatusFailed(pQuery->findNearestPoly(ep, ext, &snapFilter, &endRef, nEnd)) || !endRef)
		return NULL;

	static const int MAX_POLYS = 256, MAX_STRAIGHT = 64;
	dtPolyRef polyPath[MAX_POLYS];
	int polyCount = 0;
	if (dtStatusFailed(pQuery->findPath(startRef, endRef, nStart, nEnd, &filter, polyPath, &polyCount, MAX_POLYS)) || polyCount <= 0)
		return NULL;

	// Partial path: only accept if it ends within tolerance of the goal
	float goalPos[3];
	dtVcopy(goalPos, nEnd);
	const bool bPartial = (polyPath[polyCount - 1] != endRef);
	if (bPartial)
	{
		float closest[3];
		bool over = false;
		if (dtStatusFailed(pQuery->closestPointOnPoly(polyPath[polyCount - 1], ep, closest, &over)))
			return NULL;
		const float dx = closest[0] - ep[0], dz = closest[2] - ep[2];
		if (sqrtf(dx * dx + dz * dz) > MAX(goalTolerance, 24.0f))
			return NULL;
		dtVcopy(goalPos, closest);
	}

	float sPath[MAX_STRAIGHT * 3];
	unsigned char sFlags[MAX_STRAIGHT];
	dtPolyRef sRefs[MAX_STRAIGHT];
	int n = 0;
	if (dtStatusFailed(pQuery->findStraightPath(nStart, goalPos, polyPath, polyCount, sPath, sFlags, sRefs, &n, MAX_STRAIGHT, 0)) || n <= 0)
		return NULL;

	// ---- straight path -> waypoint list (point 0 is our own position) ----
	std::vector<WP_t> wps;
	for (int i = 1; i < n; ++i)
	{
		WP_t w;
		DetourToSrc(&sPath[i * 3], w.pos);
		w.flags = bits_WP_TO_DETOUR;
		if (sFlags[i - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)
		{
			w.nav = NAV_JUMP;   // a waypoint's type is how you get TO it
			w.flags |= bits_WP_DONT_SIMPLIFY;
		}
		if (sFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)
			w.flags |= bits_WP_DONT_SIMPLIFY;   // jump take-off point
		SnapFloor(w.pos);
		wps.push_back(w);
	}

	if (wps.empty())
	{
		WP_t w;
		w.pos = vEnd;
		wps.push_back(w);
	}

	// ---- door waypoints: walk up to the door, then the navigator opens it ----
	std::vector< std::pair<int, WP_t> > inserts;
	std::vector<EHANDLE> doneDoors;
	for (int d = 0; d < polyCount; ++d)
	{
		const int idx = (int)pMesh->decodePolyIdPoly(polyPath[d]);
		if (EZ2_NavGetDynState(idx) != EZ2_NAVDYN_DOOR)
			continue;

		CBaseEntity* pDoor = EZ2_NavGetDoorForPoly(idx);
		if (!pDoor || !FClassnameIs(pDoor, "prop_door_rotating"))
			continue;

		bool bDup = false;
		for (size_t k = 0; k < doneDoors.size(); ++k)
			if (doneDoors[k].Get() == pDoor) { bDup = true; break; }
		if (bDup)
			continue;
		doneDoors.push_back(pDoor);

		const dtMeshTile* t = NULL; const dtPoly* p = NULL;
		if (dtStatusFailed(pMesh->getTileAndPolyByRef(polyPath[d], &t, &p)))
			continue;
		float c[3] = { 0, 0, 0 };
		for (int v = 0; v < p->vertCount; ++v)
			for (int a = 0; a < 3; ++a)
				c[a] += t->verts[p->verts[v] * 3 + a] / (float)p->vertCount;
		Vector vecC;
		DetourToSrc(c, vecC);

		// where in the waypoint list does the path cross this poly?
		int ins = (int)wps.size() - 1;
		for (int j = 1; j < n; ++j)
		{
			int pos = INT_MAX;
			for (int k = d; k < polyCount; ++k)
				if (polyPath[k] == sRefs[j]) { pos = k; break; }
			if (pos > d) { ins = j - 1; break; }
		}
		ins = clamp(ins, 0, (int)wps.size() - 1);

		const Vector prev = (ins > 0) ? wps[ins - 1].pos : vStart;
		Vector dir = prev - vecC;
		dir.z = 0;
		if (VectorNormalize(dir) < 1.0f)
			dir = Vector(1, 0, 0);

		WP_t w;
		w.pos = vecC + dir * 48.0f;
		SnapFloor(w.pos);
		w.flags = bits_WP_TO_DOOR | bits_WP_DONT_SIMPLIFY;
		w.door = pDoor;
		inserts.push_back(std::make_pair(ins, w));
	}
	for (int k = (int)inserts.size() - 1; k >= 0; --k)
		wps.insert(wps.begin() + inserts[k].first, inserts[k].second);

	// ---- goal ----
	WP_t& last = wps.back();
	if (!bPartial && last.nav == NAV_GROUND)
		last.pos = vEnd;
	last.flags = bits_WP_TO_GOAL | (last.nav == NAV_JUMP ? bits_WP_DONT_SIMPLIFY : 0);

	// ---- chain ----
	AI_Waypoint_t* pHead = NULL;
	AI_Waypoint_t* pTail = NULL;
	for (size_t i = 0; i < wps.size(); ++i)
	{
		AI_Waypoint_t* p = new AI_Waypoint_t(wps[i].pos, 0.0f, wps[i].nav, wps[i].flags, NO_NODE);
		p->m_hData = wps[i].door;
		if (pTail) pTail->SetNext(p); else pHead = p;
		pTail = p;
	}
	return pHead;
}