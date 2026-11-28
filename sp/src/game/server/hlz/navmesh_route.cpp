//=============================================================================
// navmesh_route.cpp
// Builds AI_Waypoint_t routes from Detour paths so CAI_Navigator follows them.
//   - off-mesh jump connections   -> NAV_JUMP waypoints
//   - off-mesh ladder connections -> ground waypoint at the base, NAV_CLIMB chain, ground at the top
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
#include "ai_node.h"
#include <vector>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_use("ez2_nav_use", "1", 0, "Route ground NPCs through the navmesh when one is loaded");
static ConVar ez2_nav_max_hull_width("ez2_nav_max_hull_width", "0", 0, "If > 0, overrides the baked-agent size check with a fixed max hull width (0 = compare against the baked agent)");
static ConVar ez2_nav_hull_slack("ez2_nav_hull_slack", "8", 0, "Units an NPC hull may exceed the baked agent width/height and still use the navmesh");

namespace
{
	struct WP_t
	{
		Vector       pos;
		float        yaw;
		Navigation_t nav;
		int          flags;
		EHANDLE      door;
		WP_t() : pos(vec3_origin), yaw(0.0f), nav(NAV_GROUND), flags(0) {}
	};

	// One entry per off-mesh connection on a path, in path order
	struct OffInfo_t
	{
		int  climbIdx;    // -1 = jump
		bool bReverse;    // traversed from endpoint b to endpoint a
	};

	inline void DetourToSrc(const float* p, Vector& o) { o.x = p[0]; o.y = -p[2]; o.z = p[1]; }

	void SnapFloor(Vector& p)
	{
		trace_t tr;
		UTIL_TraceLine(p + Vector(0, 0, 24), p - Vector(0, 0, 64), MASK_NPCSOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &tr);
		if (!tr.startsolid && tr.DidHit())
			p = tr.endpos;
	}

	void CollectOffMesh(const dtNavMesh* pMesh, const dtPolyRef* path, int count, std::vector<OffInfo_t>& out)
	{
		out.clear();
		for (int d = 1; d < count; ++d)
		{
			const dtMeshTile* t = NULL;
			const dtPoly* p = NULL;
			if (dtStatusFailed(pMesh->getTileAndPolyByRef(path[d], &t, &p)) || p->getType() != DT_POLYTYPE_OFFMESH_CONNECTION)
				continue;

			const dtOffMeshConnection* con = pMesh->getOffMeshConnectionByRef(path[d]);
			if (!con)
				continue;

			OffInfo_t o;
			o.climbIdx = (con->userId & EZ2_NAV_CLIMB_USERID_FLAG) ? (int)(con->userId & ~EZ2_NAV_CLIMB_USERID_FLAG) : -1;
			o.bReverse = false;

			float s[3], e[3];
			if (dtStatusSucceed(pMesh->getOffMeshConnectionPolyEndPoints(path[d - 1], path[d], s, e)))
				o.bReverse = dtVdistSqr(s, &con->pos[3]) < dtVdistSqr(s, &con->pos[0]);

			out.push_back(o);
		}
	}
}

bool EZ2_NavShouldHandleNPC(CAI_BaseNPC* pNPC)
{
	if (!pNPC || !ez2_nav_use.GetBool() || !g_EZ2NavMeshManager.ShouldUseNavMesh())
		return false;

	const int caps = pNPC->CapabilitiesGet();
	if (!(caps & bits_CAP_MOVE_GROUND) || (caps & (bits_CAP_MOVE_FLY | bits_CAP_MOVE_SWIM)))
		return false;

	const Hull_t hull = pNPC->GetHullType();
	const float width = NAI_Hull::Width(hull);

	const float fixedMax = ez2_nav_max_hull_width.GetFloat();
	if (fixedMax > 0.0f)
		return width <= fixedMax;

	// Compare against the agent the mesh was actually baked for
	const dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	const dtMeshTile* pTile = pMesh ? pMesh->getTileAt(0, 0, 0) : NULL;
	if (!pTile || !pTile->header)
		return false;

	const float slack = ez2_nav_hull_slack.GetFloat();
	return width <= pTile->header->walkableRadius * 2.0f + slack &&
		NAI_Hull::Height(hull) <= pTile->header->walkableHeight + slack;
}

static AI_Waypoint_t* BuildRouteInternal(CAI_BaseNPC* pNPC, const Vector& vStart, const Vector& vEnd, float goalTolerance, bool bAllowJump, int nRetry = 0)
{
	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();
	dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	if (!pQuery || !pMesh || !pNPC) return NULL;

	const int caps = pNPC->CapabilitiesGet();
	CAI_Navigator* pNav = pNPC->GetNavigator();
	if (pNav && pNav->ShouldUseGroundOnlyNav())
		bAllowJump = false;
	const bool bCanJump = bAllowJump && (caps & bits_CAP_MOVE_JUMP) != 0;
	const bool bCanClimb = (caps & bits_CAP_MOVE_CLIMB) != 0;

	float sp[3] = { vStart.x, vStart.z, -vStart.y };
	float ep[3] = { vEnd.x, vEnd.z, -vEnd.y };
	const float ext[3] = { 48.0f, 96.0f, 48.0f };

	dtQueryFilter snapFilter;
	CEZ2_NavFilter filter((caps & bits_CAP_DOORS_GROUP) != 0, bCanJump, pNPC, bCanClimb);

	dtPolyRef startRef = 0, endRef = 0;
	float nStart[3], nEnd[3];
	if (dtStatusFailed(pQuery->findNearestPoly(sp, ext, &snapFilter, &startRef, nStart)) || !startRef)
		return NULL;
	if (dtStatusFailed(pQuery->findNearestPoly(ep, ext, &snapFilter, &endRef, nEnd)) || !endRef)
		return NULL;

	static const int MAX_POLYS = 256, MAX_STRAIGHT = 64;
	dtPolyRef polyPath[MAX_POLYS];
	int polyCount = 0;
	float goalPos[3];
	bool bPartial = false;
	float sPath[MAX_STRAIGHT * 3];
	unsigned char sFlags[MAX_STRAIGHT];
	dtPolyRef sRefs[MAX_STRAIGHT];
	int n = 0;
	std::vector<OffInfo_t> offs;

	for (int attempt = 0; ; ++attempt)
	{
		polyCount = 0;
		n = 0;

		if (dtStatusFailed(pQuery->findPath(startRef, endRef, nStart, nEnd, &filter, polyPath, &polyCount, MAX_POLYS)) || polyCount <= 0)
			return NULL;

		// Partial path: only accept if it ends within tolerance of the goal
		dtVcopy(goalPos, nEnd);
		bPartial = (polyPath[polyCount - 1] != endRef);
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

		if (dtStatusFailed(pQuery->findStraightPath(nStart, goalPos, polyPath, polyCount, sPath, sFlags, sRefs, &n, MAX_STRAIGHT, 0)) || n <= 0)
			return NULL;

		CollectOffMesh(pMesh, polyPath, polyCount, offs);

		if (!bCanJump || !pNav)
			break;

		// Verify every jump on this path is actually possible before handing it to the NPC (ladders are not jumps)
		bool bBadJump = false;
		int offSeen = 0;
		for (int i = 1; i < n; ++i)
		{
			if (!(sFlags[i - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION))
				continue;

			const int k = offSeen++;
			if (k < (int)offs.size() && offs[k].climbIdx >= 0)
				continue;

			Vector a, b;
			DetourToSrc(&sPath[(i - 1) * 3], a);
			DetourToSrc(&sPath[i * 3], b);
			if (pNav->CanNPCMakeJump(a, b))
				continue;

			EZ2_NavMarkJumpFailed(a, b, 30.0f);   // path query will now route around this link
			bBadJump = true;
			break;
		}

		if (!bBadJump)
			break;

		// Too many bad jumps in one query: stop considering jumps entirely
		if (attempt >= 3)
			return BuildRouteInternal(pNPC, vStart, vEnd, goalTolerance, false);
	}

	// ---- straight path -> waypoint list (point 0 is our own position) ----

	const std::vector<EZ2_NavClimb_t>& climbs = g_EZ2NavMeshManager.GetNavClimbs();
	std::vector<WP_t> wps;
	std::vector<int> spToWp(n, 0);   // straight-path index -> first waypoint emitted for it
	int arriveSeen = 0;

	for (int i = 1; i < n; ++i)
	{
		spToWp[i] = (int)wps.size();

		int offIdx = -1;
		if (sFlags[i - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)
			offIdx = arriveSeen++;

		const bool bArriveViaOffMesh = (offIdx >= 0);
		const bool bIsOffMeshEndpoint = (sFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0;
		const OffInfo_t* pOff = (bArriveViaOffMesh && offIdx < (int)offs.size()) ? &offs[offIdx] : NULL;

		WP_t w;
		DetourToSrc(&sPath[i * 3], w.pos);
		w.flags = bits_WP_TO_DETOUR;

		if (pOff && pOff->climbIdx >= 0)
		{
			// Ladder: the previous waypoint is the ground spot at the near end. Climb nodes, then ground at the far end.
			bool bChain = false;
			if (pOff->climbIdx < (int)climbs.size())
			{
				const EZ2_NavClimb_t& c = climbs[pOff->climbIdx];
				const int nPts = (int)c.points.size();
				for (int p = 0; p < nPts; ++p)
				{
					const EZ2_NavClimbPoint_t& cp = c.points[pOff->bReverse ? (nPts - 1 - p) : p];
					WP_t cw;
					cw.pos = cp.pos;
					cw.yaw = cp.yaw;
					cw.nav = NAV_CLIMB;
					cw.flags = bits_WP_TO_DETOUR | bits_WP_DONT_SIMPLIFY;
					wps.push_back(cw);
				}
				bChain = (nPts > 0);
			}

			// No climb data for this ladder: climb straight to the far end rather than walk through the wall
			if (!bChain)
				w.nav = NAV_CLIMB;
			w.flags |= bits_WP_DONT_SIMPLIFY;
		}
		else if (bArriveViaOffMesh)
		{
			w.nav = NAV_JUMP;   // a waypoint's type is how you get TO it
			w.flags |= bits_WP_DONT_SIMPLIFY;
		}

		if (bIsOffMeshEndpoint)
			w.flags |= bits_WP_DONT_SIMPLIFY;   // jump take-off / ladder base / off-mesh endpoint

		if (!bArriveViaOffMesh && !bIsOffMeshEndpoint)
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
			if (pos > d) { ins = spToWp[j]; break; }
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

	// ---- goal (set before prop detours so the final leg to vEnd is also checked) ----
	WP_t& last = wps.back();
	if (!bPartial && last.nav == NAV_GROUND)
		last.pos = vEnd;
	last.flags = bits_WP_TO_GOAL | ((last.nav == NAV_JUMP || last.nav == NAV_CLIMB) ? bits_WP_DONT_SIMPLIFY : 0);

	// ---- detours around resting physics props (supports multiple props/corners per leg) ----
	{
		const float clearance = NAI_Hull::Width(pNPC->GetHullType()) * 0.5f + 4.0f;
		Vector prev = vStart;
		size_t i = 0;
		int legDetours = 0;
		while (i < wps.size())
		{
			std::vector<Vector> pts;
			Vector blockC = vec3_origin; float blockR = 0.0f;
			const int r = (wps[i].nav == NAV_GROUND && !(wps[i].flags & bits_WP_TO_DOOR) && legDetours < 8)
				? EZ2_NavBuildCutoutDetour(prev, wps[i].pos, clearance, pts, &blockC, &blockR) : 0;

			if (r < 0)
			{
				// Prop completely walls this leg off: block it briefly and repath
				if (nRetry < 2)
				{
					EZ2_NavMarkBlocked(blockC, blockR + clearance, 3.0f);
					return BuildRouteInternal(pNPC, vStart, vEnd, goalTolerance, bAllowJump, nRetry + 1);
				}
				// give up on detouring this leg; local nav will have to deal with it
			}
			else if (r > 0)
			{
				for (size_t k = 0; k < pts.size(); ++k)
				{
					SnapFloor(pts[k]);
					WP_t d;
					d.pos = pts[k];
					d.flags = bits_WP_TO_DETOUR | bits_WP_DONT_SIMPLIFY;
					wps.insert(wps.begin() + i + k, d);
				}
				prev = wps[i + pts.size() - 1].pos;
				i += pts.size();
				++legDetours;
				continue;   // re-test the remainder of the leg (handles multiple props)
			}

			prev = wps[i].pos;
			++i;
			legDetours = 0;
		}
	}

	// ---- chain ----
	AI_Waypoint_t* pHead = NULL;
	AI_Waypoint_t* pTail = NULL;
	for (size_t i = 0; i < wps.size(); ++i)
	{
		AI_Waypoint_t* p = new AI_Waypoint_t(wps[i].pos, wps[i].yaw, wps[i].nav, wps[i].flags, NO_NODE);
		p->m_hData = wps[i].door;
		if (pTail) pTail->SetNext(p); else pHead = p;
		pTail = p;
	}
	return pHead;
}

AI_Waypoint_t* EZ2_NavBuildRoute(CAI_BaseNPC* pNPC, const Vector& vStart, const Vector& vEnd, float goalTolerance)
{
	return BuildRouteInternal(pNPC, vStart, vEnd, goalTolerance, true);
}

AI_Waypoint_t* EZ2_NavBuildGroundOnlyRoute(CAI_BaseNPC* pNPC, const Vector& vStart, const Vector& vEnd, float goalTolerance)
{
	return BuildRouteInternal(pNPC, vStart, vEnd, goalTolerance, false);
}