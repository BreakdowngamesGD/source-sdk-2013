//=============================================================================
// navmesh_debugdraw.cpp
//
// Purpose: In-game visualization of the loaded Detour navmesh (Gmod-style).
//
//   ez2_nav_draw 1          draw polys near the player
//   ez2_nav_draw_radius N   only draw polys within N units (default 1500)
//   ez2_nav_draw_fill N     fill alpha 0-255 (0 = outlines only)
//   ez2_nav_draw_zoffset N  lift drawing off the floor (default 4)
//   ez2_nav_draw_maxpolys N safety cap on polys drawn per frame
//   ez2_nav_path_aim        path from you to where you're looking (drawn in red)
//   ez2_nav_path_clear      clear the drawn path
//
// Colors: green = ground, yellow = dynamic/OFF link area (node mesh),
//         magenta = off-mesh jump connection, red = test path / blocked,
//         magenta footprint = resting prop cutout (scaled 1.1x on the floor).
//
// Needs a listen server / singleplayer (server overlays are not shown on
// dedicated servers).
//=============================================================================

#include "cbase.h"

#include "NavMeshManager.h"
#include "NavMeshBuilder_Node.h"   // EZ2_NAV_AREA_* ids
#include "NavMeshDynamic.h"

#include "ndebugoverlay.h"
#include "igamesystem.h"
#include "player.h"

#include <vector>

#include <float.h>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_draw("ez2_nav_draw", "0", 0, "Draw the loaded navmesh in game");
static ConVar ez2_nav_draw_radius("ez2_nav_draw_radius", "1500", 0, "Only draw navmesh polys within this distance of the player");
static ConVar ez2_nav_draw_fill("ez2_nav_draw_fill", "40", 0, "Fill alpha (0-255). 0 = outlines only");
static ConVar ez2_nav_draw_zoffset("ez2_nav_draw_zoffset", "4", 0, "Vertical offset so the overlay doesn't z-fight the floor");
static ConVar ez2_nav_draw_maxpolys("ez2_nav_draw_maxpolys", "2500", 0, "Max polys drawn per frame");
static ConVar ez2_nav_draw_twosided("ez2_nav_draw_twosided", "1", 0, "Emit fill triangles with both windings (fixes invisible fill from some angles)");

namespace
{
	inline Vector DetourToSourceVec(const float* p, float zOffset)
	{
		return Vector(p[0], -p[2], p[1] + zOffset);
	}

	inline void SourceToDetourVec(const Vector& in, float out[3])
	{
		out[0] = in.x;
		out[1] = in.z;
		out[2] = -in.y;
	}

	static CUtlVector<Vector> s_TestPath;

	void GetAreaColor(unsigned char area, int& r, int& g, int& b)
	{
		switch (area)
		{
		case EZ2_NAV_AREA_DYNLINK:	r = 255; g = 200; b = 0;   break;
		case EZ2_NAV_AREA_JUMP:		r = 255; g = 0;   b = 255; break;
		case EZ2_NAV_AREA_CLIMB:	r = 0;   g = 200; b = 255; break;
		default:					r = 0;   g = 255; b = 60;  break;
		}
	}

	// XY half-plane clip (Sutherland-Hodgman). Keeps the side where G >= 0.
	float HalfG(const Vector& a, const Vector& b, const Vector& p)
	{
		return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
	}

	void ClipHalfXY(const std::vector<Vector>& in, const Vector& a, const Vector& b, std::vector<Vector>& out)
	{
		out.clear();
		const size_t n = in.size();
		for (size_t i = 0; i < n; ++i)
		{
			const Vector& S = in[(i + n - 1) % n];
			const Vector& E = in[i];
			const float gs = HalfG(a, b, S);
			const float ge = HalfG(a, b, E);
			const bool sIn = gs >= 0.0f;
			const bool eIn = ge >= 0.0f;
			if (eIn)
			{
				if (!sIn) out.push_back(S + (E - S) * (gs / (gs - ge)));
				out.push_back(E);
			}
			else if (sIn)
			{
				out.push_back(S + (E - S) * (gs / (gs - ge)));
			}
		}
	}

	// Appends the parts of subject that lie outside convex CCW polygon cut (XY)
	void SubtractConvexXY(const std::vector<Vector>& subject, const std::vector<Vector>& cut,
		std::vector<std::vector<Vector> >& out)
	{
		std::vector<Vector> rem = subject, outside, keep;
		const size_t m = cut.size();
		for (size_t k = 0; k < m && rem.size() >= 3; ++k)
		{
			const Vector& a = cut[k];
			const Vector& b = cut[(k + 1) % m];
			ClipHalfXY(rem, b, a, outside);   // outside edge k
			if (outside.size() >= 3)
				out.push_back(outside);
			ClipHalfXY(rem, a, b, keep);      // keep the inside part for the next edge
			rem.swap(keep);
		}
	}

	void DrawFilledEdged(const std::vector<Vector>& poly, int r, int g, int b, int fillAlpha, bool bTwoSided)
	{
		const size_t n = poly.size();
		for (size_t i = 0; i < n; ++i)
			NDebugOverlay::Line(poly[i], poly[(i + 1) % n], r, g, b, false, 0.0f);

		if (fillAlpha <= 0)
			return;

		for (size_t i = 1; i + 1 < n; ++i)
		{
			NDebugOverlay::Triangle(poly[0], poly[i], poly[i + 1], r, g, b, fillAlpha, false, 0.0f);
			if (bTwoSided)
				NDebugOverlay::Triangle(poly[0], poly[i + 1], poly[i], r, g, b, fillAlpha, false, 0.0f);
		}
	}

	// Fan-triangulates a polygon, subtracts every cut from each triangle, and draws what remains
	void DrawCutPoly(const Vector* verts, int nv, const std::vector<const std::vector<Vector>*>& cuts,
		int r, int g, int b, int fillAlpha, bool bTwoSided)
	{
		for (int v = 1; v < nv - 1; ++v)
		{
			std::vector<std::vector<Vector> > pieces(1), next;
			pieces[0].push_back(verts[0]);
			pieces[0].push_back(verts[v]);
			pieces[0].push_back(verts[v + 1]);

			for (size_t c = 0; c < cuts.size() && !pieces.empty(); ++c)
			{
				next.clear();
				for (size_t p = 0; p < pieces.size(); ++p)
					SubtractConvexXY(pieces[p], *cuts[c], next);
				pieces.swap(next);
			}

			for (size_t p = 0; p < pieces.size(); ++p)
				DrawFilledEdged(pieces[p], r, g, b, fillAlpha, bTwoSided);
		}
	}

	struct CutDraw_t
	{
		std::vector<Vector> fp;
		float minX, maxX, minY, maxY, zMin;
	};

	void DrawPolys(const Vector& vecCenter)
	{
		dtNavMesh* pNavMesh = g_EZ2NavMeshManager.GetNavMesh();
		if (!pNavMesh)
			return;

		const dtMeshTile* pTile = pNavMesh->getTileAt(0, 0, 0);
		if (!pTile || !pTile->header)
			return;

		const float zOff = ez2_nav_draw_zoffset.GetFloat();
		const float radiusSqr = Square(ez2_nav_draw_radius.GetFloat());
		const int fillAlpha = clamp(ez2_nav_draw_fill.GetInt(), 0, 255);
		const int maxPolys = ez2_nav_draw_maxpolys.GetInt();
		const bool bTwoSided = ez2_nav_draw_twosided.GetBool();

		// Prop floor cutouts near the player
		std::vector<std::vector<Vector> > footprints;
		EZ2_NavGetPropCutoutFootprints(footprints);
		const float cullSqr = Square(ez2_nav_draw_radius.GetFloat() + 512.0f);
		std::vector<CutDraw_t> cuts;
		for (size_t c = 0; c < footprints.size(); ++c)
		{
			CutDraw_t cd;
			cd.fp = footprints[c];
			if (cd.fp.size() < 3)
				continue;

			Vector cen(0, 0, 0);
			cd.minX = cd.maxX = cd.fp[0].x;
			cd.minY = cd.maxY = cd.fp[0].y;
			for (size_t k = 0; k < cd.fp.size(); ++k)
			{
				cen += cd.fp[k];
				cd.minX = MIN(cd.minX, cd.fp[k].x); cd.maxX = MAX(cd.maxX, cd.fp[k].x);
				cd.minY = MIN(cd.minY, cd.fp[k].y); cd.maxY = MAX(cd.maxY, cd.fp[k].y);
			}
			cen /= (float)cd.fp.size();
			cd.zMin = cd.fp[0].z;
			if ((cen - vecCenter).LengthSqr() > cullSqr)
				continue;
			cuts.push_back(cd);
		}

		int drawn = 0;

		for (int i = 0; i < pTile->header->polyCount && drawn < maxPolys; ++i)
		{
			const dtPoly* pPoly = &pTile->polys[i];

			int r, g, b;
			GetAreaColor(pPoly->getArea(), r, g, b);

			const int dyn = EZ2_NavGetDynState(i);
			if (dyn == EZ2_NAVDYN_BLOCKED) { r = 160; g = 0;   b = 0; }
			else if (dyn == EZ2_NAVDYN_DOOR) { r = 255; g = 128; b = 0; }

			// Off-mesh connection: just a line between its two endpoints
			if (pPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				const Vector a = DetourToSourceVec(&pTile->verts[pPoly->verts[0] * 3], zOff);
				const Vector bb = DetourToSourceVec(&pTile->verts[pPoly->verts[1] * 3], zOff);
				if ((a - vecCenter).LengthSqr() > radiusSqr && (bb - vecCenter).LengthSqr() > radiusSqr)
					continue;

				NDebugOverlay::Line(a, bb, r, g, b, false, 0.0f);
				NDebugOverlay::Box(a, Vector(-3, -3, -3), Vector(3, 3, 3), r, g, b, 0, 0.0f);
				NDebugOverlay::Box(bb, Vector(-3, -3, -3), Vector(3, 3, 3), r, g, b, 0, 0.0f);
				++drawn;
				continue;
			}

			const int nv = pPoly->vertCount;
			if (nv < 3)
				continue;

			Vector verts[DT_VERTS_PER_POLYGON];
			Vector centroid(0, 0, 0);
			for (int v = 0; v < nv; ++v)
			{
				verts[v] = DetourToSourceVec(&pTile->verts[pPoly->verts[v] * 3], zOff);
				centroid += verts[v];
			}
			centroid /= (float)nv;

			if ((centroid - vecCenter).LengthSqr() > radiusSqr)
				continue;

			// Cuts that overlap this polygon in XY and sit on its floor height
			float pMinX = FLT_MAX, pMaxX = -FLT_MAX, pMinY = FLT_MAX, pMaxY = -FLT_MAX;
			for (int v = 0; v < nv; ++v)
			{
				pMinX = MIN(pMinX, verts[v].x); pMaxX = MAX(pMaxX, verts[v].x);
				pMinY = MIN(pMinY, verts[v].y); pMaxY = MAX(pMaxY, verts[v].y);
			}

			std::vector<const std::vector<Vector>*> hits;
			for (size_t c = 0; c < cuts.size(); ++c)
			{
				const CutDraw_t& cd = cuts[c];
				if (cd.maxX < pMinX || cd.minX > pMaxX || cd.maxY < pMinY || cd.minY > pMaxY)
					continue;
				if (centroid.z < cd.zMin - 24.0f || centroid.z > cd.zMin + 24.0f)
					continue;
				hits.push_back(&cd.fp);
			}

			if (hits.empty())
			{
				std::vector<Vector> poly(verts, verts + nv);
				DrawFilledEdged(poly, r, g, b, fillAlpha, bTwoSided);
			}
			else
			{
				DrawCutPoly(verts, nv, hits, r, g, b, fillAlpha, bTwoSided);
			}

			++drawn;
		}
	}

	// Outlines and fills each resting prop's cutout footprint on the floor.
	// The footprint is the prop's bottom face at its 1.1x scaled OBB, so it
	// matches the area the filter rejects, regardless of polygon size.
	/*
	void DrawPropCutouts(const Vector& vecCenter)
	{
		std::vector<Vector> corners;
		EZ2_NavGetPropCutoutCorners(corners);

		const float zOff = ez2_nav_draw_zoffset.GetFloat();
		const float radiusSqr = Square(ez2_nav_draw_radius.GetFloat());
		const int fillAlpha = clamp(ez2_nav_draw_fill.GetInt(), 0, 255);
		const bool bTwoSided = ez2_nav_draw_twosided.GetBool();
		const int r = 160, g = 0, b = 160;   // magenta

		// 8 corners per box. Bit 0 = x, bit 1 = y, bit 2 = z, so indices
		// 0,1,2,3 have the z bit clear and form the bottom face.
		for (size_t base = 0; base + 8 <= corners.size(); base += 8)
		{
			const Vector mid = (corners[base + 0] + corners[base + 7]) * 0.5f;
			if ((mid - vecCenter).LengthSqr() > radiusSqr)
				continue;

			// Bottom face in perimeter order: (-,-), (+,-), (+,+), (-,+)
			Vector f[4] = { corners[base + 0], corners[base + 1], corners[base + 3], corners[base + 2] };
			for (int k = 0; k < 4; ++k)
				f[k].z += zOff;

			for (int k = 0; k < 4; ++k)
				NDebugOverlay::Line(f[k], f[(k + 1) % 4], r, g, b, false, 0.0f);

			if (fillAlpha > 0)
			{
				NDebugOverlay::Triangle(f[0], f[1], f[2], r, g, b, fillAlpha, false, 0.0f);
				NDebugOverlay::Triangle(f[0], f[2], f[3], r, g, b, fillAlpha, false, 0.0f);
				if (bTwoSided)
				{
					NDebugOverlay::Triangle(f[0], f[2], f[1], r, g, b, fillAlpha, false, 0.0f);
					NDebugOverlay::Triangle(f[0], f[3], f[2], r, g, b, fillAlpha, false, 0.0f);
				}
			}
		}
	}
	*/

	static ConVar ez2_nav_draw_hints("ez2_nav_draw_hints", "0", 0, "Draw baked cover hints");
	static ConVar ez2_nav_draw_hints_dynamic("ez2_nav_draw_hints_dynamic", "1", 0, "With ez2_nav_draw_hints: also draw live dynamic cover (physics props, doors) vs. where you're looking");
	static ConVar ez2_nav_draw_hints_radius("ez2_nav_draw_hints_radius", "768", 0, "Radius for the dynamic cover overlay");
	static std::vector<EZ2_NavDynCoverSpot_t> s_DynCover;

	void DrawNavHints()
	{
		const std::vector<EZ2_NavHint_t>& hints = g_EZ2NavMeshManager.GetNavHints();
		for (size_t i = 0; i < hints.size(); ++i)
		{
			const EZ2_NavHint_t& h = hints[i];
			if (h.polyRef == 0)
				continue;   // unbound hints can't be queried at runtime

			int r = 200, g = 200, b = 200;
			if (h.navHintType == EZ2_NAVHINT_COVER_CROUCH) { r = 255; g = 220; b = 0; }
			else if (h.navHintType == EZ2_NAVHINT_COVER_STAND) { r = 0; g = 180; b = 255; }
			else if (h.navHintType == EZ2_NAVHINT_PINCH_OR_FLANK) { r = 80; g = 255; b = 120; }
			if (h.bDisabled) { r = 80; g = 80; b = 80; }

			const float height = (h.navHintType == EZ2_NAVHINT_COVER_CROUCH) ? 32.0f : 62.0f;
			NDebugOverlay::Box(h.vecPos, Vector(-6, -6, 0), Vector(6, 6, height), r, g, b, 120, 0.0f);

			// Facing arrow
			Vector dir = UTIL_YawToVector(h.flYaw);
			NDebugOverlay::Line(h.vecPos + Vector(0, 0, 24), h.vecPos + Vector(0, 0, 24) + dir * 32.0f, 255, 255, 255, true, 0.0f);
		}
	}

	// Dynamic cover: orange = crouch-only, red = tall/corner. Arrow points toward the threat.
	void DrawDynamicCover()
	{
		CBasePlayer* pPlayer = UTIL_GetLocalPlayer();
		if (!pPlayer)
			return;

		static float s_flNext = 0.0f;
		if (gpGlobals->curtime >= s_flNext)
		{
			s_flNext = gpGlobals->curtime + 0.25f;

			Vector fwd;
			pPlayer->EyeVectors(&fwd);
			trace_t tr;
			UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + fwd * 2048.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr);
			const Vector vecThreat = tr.endpos;
			EZ2_NavCollectDynamicCoverAroundOrigin(pPlayer->GetAbsOrigin(), ez2_nav_draw_hints_radius.GetFloat(), s_DynCover, &vecThreat);
		}

		for (size_t i = 0; i < s_DynCover.size(); ++i)
		{
			const EZ2_NavDynCoverSpot_t& s = s_DynCover[i];
			const int r = 255, g = s.bCrouchOnly ? 140 : 60, b = s.bCrouchOnly ? 0 : 60;
			NDebugOverlay::Box(s.vecPos, Vector(-8, -8, 0), Vector(8, 8, MAX(s.flHeight, 24.0f)), r, g, b, 120, 0.0f);
			NDebugOverlay::Line(s.vecPos + Vector(0, 0, 24), s.vecPos + Vector(0, 0, 24) + UTIL_YawToVector(s.flYaw) * 32.0f, 255, 255, 255, true, 0.0f);
		}
	}

	void DrawTestPath()
	{
		const int n = s_TestPath.Count();
		for (int i = 0; i < n; ++i)
		{
			NDebugOverlay::Box(s_TestPath[i], Vector(-5, -5, -5), Vector(5, 5, 5), 255, 0, 0, 0, 0.0f);
			if (i + 1 < n)
			{
				NDebugOverlay::Line(s_TestPath[i] + Vector(0, 0, 8), s_TestPath[i + 1] + Vector(0, 0, 8), 255, 0, 0, true, 0.0f);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Per-frame drawing
//-----------------------------------------------------------------------------
class CEZ2_NavMeshDebugDraw : public CAutoGameSystemPerFrame
{
public:
	CEZ2_NavMeshDebugDraw() : CAutoGameSystemPerFrame("EZ2_NavMeshDebugDraw") {}

	virtual void LevelShutdownPostEntity() OVERRIDE
	{
		s_TestPath.Purge();
		s_DynCover.clear();
	}

	virtual void PreClientUpdate() OVERRIDE
	{
		if (!g_EZ2NavMeshManager.IsReady())
			return;

		if (ez2_nav_draw_hints.GetBool())
		{
			DrawNavHints();
			if (ez2_nav_draw_hints_dynamic.GetBool())
				DrawDynamicCover();
		}

		if (ez2_nav_draw.GetBool())
		{
			CBasePlayer* pPlayer = UTIL_GetLocalPlayer();
			if (pPlayer)
				DrawPolys(pPlayer->GetAbsOrigin());
		}

		// The test path is drawn even when ez2_nav_draw is off
		DrawTestPath();
	}
};

static CEZ2_NavMeshDebugDraw s_EZ2NavMeshDebugDraw;

//-----------------------------------------------------------------------------
// ez2_nav_path_aim: full straight path from the player to the crosshair
//-----------------------------------------------------------------------------
CON_COMMAND(ez2_nav_path_aim, "Finds a navmesh path from you to where you are looking and draws it in red")
{
	CBasePlayer* pPlayer = UTIL_GetCommandClient();
	if (!pPlayer)
		pPlayer = UTIL_GetLocalPlayer();
	if (!pPlayer)
		return;

	if (!g_EZ2NavMeshManager.IsReady())
	{
		Warning("[EZ2 NavMesh] No navmesh loaded. Check ez2_nav_status.\n");
		return;
	}

	Vector vecForward;
	pPlayer->EyeVectors(&vecForward);

	trace_t tr;
	UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + vecForward * 8192.0f,
		MASK_NPCSOLID_BRUSHONLY, pPlayer, COLLISION_GROUP_NONE, &tr);

	const Vector vecStart = pPlayer->GetAbsOrigin();
	const Vector vecEnd = tr.endpos;

	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();

	float startPos[3], endPos[3];
	SourceToDetourVec(vecStart, startPos);
	SourceToDetourVec(vecEnd, endPos);

	const float halfExtents[3] = { 64.0f, 128.0f, 64.0f };
	dtQueryFilter filter;

	CEZ2_NavFilter pathFilter;

	dtPolyRef startRef = 0, endRef = 0;
	float nearestStart[3], nearestEnd[3];

	s_TestPath.Purge();

	if (dtStatusFailed(pQuery->findNearestPoly(startPos, halfExtents, &filter, &startRef, nearestStart)) || !startRef)
	{
		Warning("[EZ2 NavMesh] You are not near any navmesh polygon.\n");
		return;
	}

	if (dtStatusFailed(pQuery->findNearestPoly(endPos, halfExtents, &filter, &endRef, nearestEnd)) || !endRef)
	{
		Warning("[EZ2 NavMesh] Target point is not near any navmesh polygon.\n");
		return;
	}

	static const int MAX_POLYS = 512;
	dtPolyRef polyPath[MAX_POLYS];
	int polyCount = 0;

	if (dtStatusFailed(pQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &pathFilter, polyPath, &polyCount, MAX_POLYS)) || polyCount <= 0)
	{
		Warning("[EZ2 NavMesh] No path found.\n");
		return;
	}

	static const int MAX_STRAIGHT = 128;
	float straight[MAX_STRAIGHT * 3];
	unsigned char straightFlags[MAX_STRAIGHT];
	dtPolyRef straightPolys[MAX_STRAIGHT];
	int straightCount = 0;

	if (dtStatusFailed(pQuery->findStraightPath(nearestStart, nearestEnd, polyPath, polyCount,
		straight, straightFlags, straightPolys, &straightCount, MAX_STRAIGHT)) || straightCount <= 0)
	{
		Warning("[EZ2 NavMesh] Straight-path query failed.\n");
		return;
	}

	for (int i = 0; i < straightCount; ++i)
		s_TestPath.AddToTail(DetourToSourceVec(&straight[i * 3], 0.0f));

	const bool bPartial = (polyPath[polyCount - 1] != endRef);
	Msg("[EZ2 NavMesh] Path: %d polys, %d waypoints%s\n", polyCount, straightCount, bPartial ? " (PARTIAL - target not reachable)" : "");
}

CON_COMMAND(ez2_nav_path_clear, "Clears the drawn navmesh test path")
{
	s_TestPath.Purge();
}

CON_COMMAND(ez2_nav_debug_dyn_cover, "Draws debug boxes and facing lines for dynamic cover spots")
{
	CBasePlayer* pPlayer = UTIL_GetLocalPlayer();
	if (!pPlayer)
		return;

	const Vector vecOrigin = pPlayer->GetAbsOrigin();
	std::vector<EZ2_NavDynCoverSpot_t> dynCover;
	EZ2_NavCollectDynamicCoverAroundOrigin(vecOrigin, 1024.0f, dynCover);

	Msg("[EZ2 NavMesh] Dynamic cover spots: %d\n", static_cast<int>(dynCover.size()));

	for (size_t i = 0; i < dynCover.size(); ++i)
	{
		const EZ2_NavDynCoverSpot_t& spot = dynCover[i];
		const float topZ = MAX(spot.flHeight, 24.0f);
		NDebugOverlay::Box(
			spot.vecPos,
			Vector(-12, -12, 0),
			Vector(12, 12, topZ),
			255, 140, 0, 128, 10.0f
		);

		Vector vecDir = UTIL_YawToVector(spot.flYaw);
		NDebugOverlay::Line(
			spot.vecPos + Vector(0, 0, 16),
			spot.vecPos + Vector(0, 0, 16) + vecDir * 36.0f,
			255, 64, 255, true, 10.0f
		);
	}
}