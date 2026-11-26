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
//         magenta = off-mesh jump connection, red = test path.
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

#include <float.h>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_draw( "ez2_nav_draw", "0", 0, "Draw the loaded navmesh in game" );
static ConVar ez2_nav_draw_radius( "ez2_nav_draw_radius", "1500", 0, "Only draw navmesh polys within this distance of the player" );
static ConVar ez2_nav_draw_fill( "ez2_nav_draw_fill", "40", 0, "Fill alpha (0-255). 0 = outlines only" );
static ConVar ez2_nav_draw_zoffset( "ez2_nav_draw_zoffset", "4", 0, "Vertical offset so the overlay doesn't z-fight the floor" );
static ConVar ez2_nav_draw_maxpolys( "ez2_nav_draw_maxpolys", "2500", 0, "Max polys drawn per frame" );
static ConVar ez2_nav_draw_twosided( "ez2_nav_draw_twosided", "1", 0, "Emit fill triangles with both windings (fixes invisible fill from some angles)" );

namespace
{
	inline Vector DetourToSourceVec( const float* p, float zOffset )
	{
		return Vector( p[0], -p[2], p[1] + zOffset );
	}

	inline void SourceToDetourVec( const Vector& in, float out[3] )
	{
		out[0] = in.x;
		out[1] = in.z;
		out[2] = -in.y;
	}

	static CUtlVector<Vector> s_TestPath;

	void GetAreaColor( unsigned char area, int& r, int& g, int& b )
	{
		switch ( area )
		{
		case EZ2_NAV_AREA_DYNLINK:	r = 255; g = 200; b = 0;   break;
		case EZ2_NAV_AREA_JUMP:		r = 255; g = 0;   b = 255; break;
		default:					r = 0;   g = 255; b = 60;  break;
		}
	}

	void DrawPolys( const Vector& vecCenter )
	{
		dtNavMesh* pNavMesh = g_EZ2NavMeshManager.GetNavMesh();
		if ( !pNavMesh )
			return;

		const dtMeshTile* pTile = pNavMesh->getTileAt(0, 0, 0);
		if ( !pTile || !pTile->header )
			return;

		const float zOff = ez2_nav_draw_zoffset.GetFloat();
		const float radiusSqr = Square( ez2_nav_draw_radius.GetFloat() );
		const int fillAlpha = clamp( ez2_nav_draw_fill.GetInt(), 0, 255 );
		const int maxPolys = ez2_nav_draw_maxpolys.GetInt();
		const bool bTwoSided = ez2_nav_draw_twosided.GetBool();

		int drawn = 0;

		for ( int i = 0; i < pTile->header->polyCount && drawn < maxPolys; ++i )
		{
			const dtPoly* pPoly = &pTile->polys[i];

			int r, g, b;
			GetAreaColor( pPoly->getArea(), r, g, b );

			const int dyn = EZ2_NavGetDynState(i);
			if (dyn == EZ2_NAVDYN_BLOCKED) { r = 160; g = 0;   b = 0; }
			else if (dyn == EZ2_NAVDYN_DOOR) { r = 255; g = 128; b = 0; }

			// Off-mesh connection: just a line between its two endpoints
			if ( pPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION )
			{
				const Vector a = DetourToSourceVec( &pTile->verts[pPoly->verts[0] * 3], zOff );
				const Vector bb = DetourToSourceVec( &pTile->verts[pPoly->verts[1] * 3], zOff );
				if ( ( a - vecCenter ).LengthSqr() > radiusSqr && ( bb - vecCenter ).LengthSqr() > radiusSqr )
					continue;

				NDebugOverlay::Line( a, bb, r, g, b, false, 0.0f );
				NDebugOverlay::Box( a, Vector( -3, -3, -3 ), Vector( 3, 3, 3 ), r, g, b, 0, 0.0f );
				NDebugOverlay::Box( bb, Vector( -3, -3, -3 ), Vector( 3, 3, 3 ), r, g, b, 0, 0.0f );
				++drawn;
				continue;
			}

			const int nv = pPoly->vertCount;
			if ( nv < 3 )
				continue;

			Vector verts[DT_VERTS_PER_POLYGON];
			Vector centroid( 0, 0, 0 );
			for ( int v = 0; v < nv; ++v )
			{
				verts[v] = DetourToSourceVec( &pTile->verts[pPoly->verts[v] * 3], zOff );
				centroid += verts[v];
			}
			centroid /= (float)nv;

			if ( ( centroid - vecCenter ).LengthSqr() > radiusSqr )
				continue;

			// Outline
			for ( int v = 0; v < nv; ++v )
			{
				NDebugOverlay::Line( verts[v], verts[( v + 1 ) % nv], r, g, b, false, 0.0f );
			}

			// Fill (triangle fan)
			if ( fillAlpha > 0 )
			{
				for ( int v = 1; v < nv - 1; ++v )
				{
					NDebugOverlay::Triangle( verts[0], verts[v], verts[v + 1], r, g, b, fillAlpha, false, 0.0f );
					if ( bTwoSided )
						NDebugOverlay::Triangle( verts[0], verts[v + 1], verts[v], r, g, b, fillAlpha, false, 0.0f );
				}
			}

			++drawn;
		}
	}

	static ConVar ez2_nav_draw_hints("ez2_nav_draw_hints", "0", 0, "Draw baked cover hints");

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

	void DrawTestPath()
	{
		const int n = s_TestPath.Count();
		for ( int i = 0; i < n; ++i )
		{
			NDebugOverlay::Box( s_TestPath[i], Vector( -5, -5, -5 ), Vector( 5, 5, 5 ), 255, 0, 0, 0, 0.0f );
			if ( i + 1 < n )
			{
				NDebugOverlay::Line( s_TestPath[i] + Vector( 0, 0, 8 ), s_TestPath[i + 1] + Vector( 0, 0, 8 ), 255, 0, 0, true, 0.0f );
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
	CEZ2_NavMeshDebugDraw() : CAutoGameSystemPerFrame( "EZ2_NavMeshDebugDraw" ) {}

	virtual void LevelShutdownPostEntity() OVERRIDE
	{
		s_TestPath.Purge();
	}

	virtual void PreClientUpdate() OVERRIDE
	{
		if ( !g_EZ2NavMeshManager.IsReady() )
			return;

		if (ez2_nav_draw_hints.GetBool())
			DrawNavHints();

		if ( ez2_nav_draw.GetBool() )
		{
			CBasePlayer* pPlayer = UTIL_GetLocalPlayer();
			if ( pPlayer )
				DrawPolys( pPlayer->GetAbsOrigin() );
		}

		// The test path is drawn even when ez2_nav_draw is off
		DrawTestPath();
	}
};

static CEZ2_NavMeshDebugDraw s_EZ2NavMeshDebugDraw;

//-----------------------------------------------------------------------------
// ez2_nav_path_aim: full straight path from the player to the crosshair
//-----------------------------------------------------------------------------
CON_COMMAND( ez2_nav_path_aim, "Finds a navmesh path from you to where you are looking and draws it in red" )
{
	CBasePlayer* pPlayer = UTIL_GetCommandClient();
	if ( !pPlayer )
		pPlayer = UTIL_GetLocalPlayer();
	if ( !pPlayer )
		return;

	if ( !g_EZ2NavMeshManager.IsReady() )
	{
		Warning( "[EZ2 NavMesh] No navmesh loaded. Check ez2_nav_status.\n" );
		return;
	}

	Vector vecForward;
	pPlayer->EyeVectors( &vecForward );

	trace_t tr;
	UTIL_TraceLine( pPlayer->EyePosition(), pPlayer->EyePosition() + vecForward * 8192.0f,
		MASK_NPCSOLID_BRUSHONLY, pPlayer, COLLISION_GROUP_NONE, &tr );

	const Vector vecStart = pPlayer->GetAbsOrigin();
	const Vector vecEnd = tr.endpos;

	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();

	float startPos[3], endPos[3];
	SourceToDetourVec( vecStart, startPos );
	SourceToDetourVec( vecEnd, endPos );

	const float halfExtents[3] = { 64.0f, 128.0f, 64.0f };
	dtQueryFilter filter;

	CEZ2_NavFilter pathFilter;

	dtPolyRef startRef = 0, endRef = 0;
	float nearestStart[3], nearestEnd[3];

	s_TestPath.Purge();

	if ( dtStatusFailed( pQuery->findNearestPoly( startPos, halfExtents, &filter, &startRef, nearestStart ) ) || !startRef )
	{
		Warning( "[EZ2 NavMesh] You are not near any navmesh polygon.\n" );
		return;
	}

	if ( dtStatusFailed( pQuery->findNearestPoly( endPos, halfExtents, &filter, &endRef, nearestEnd ) ) || !endRef )
	{
		Warning( "[EZ2 NavMesh] Target point is not near any navmesh polygon.\n" );
		return;
	}

	static const int MAX_POLYS = 512;
	dtPolyRef polyPath[MAX_POLYS];
	int polyCount = 0;

	if (dtStatusFailed(pQuery->findPath(startRef, endRef, nearestStart, nearestEnd, &pathFilter, polyPath, &polyCount, MAX_POLYS)) || polyCount <= 0) 
	{
		Warning( "[EZ2 NavMesh] No path found.\n" );
		return;
	}

	static const int MAX_STRAIGHT = 128;
	float straight[MAX_STRAIGHT * 3];
	unsigned char straightFlags[MAX_STRAIGHT];
	dtPolyRef straightPolys[MAX_STRAIGHT];
	int straightCount = 0;

	if ( dtStatusFailed( pQuery->findStraightPath( nearestStart, nearestEnd, polyPath, polyCount,
			straight, straightFlags, straightPolys, &straightCount, MAX_STRAIGHT ) ) || straightCount <= 0 )
	{
		Warning( "[EZ2 NavMesh] Straight-path query failed.\n" );
		return;
	}

	for ( int i = 0; i < straightCount; ++i )
		s_TestPath.AddToTail( DetourToSourceVec( &straight[i * 3], 0.0f ) );

	const bool bPartial = ( polyPath[polyCount - 1] != endRef );
	Msg( "[EZ2 NavMesh] Path: %d polys, %d waypoints%s\n", polyCount, straightCount, bPartial ? " (PARTIAL - target not reachable)" : "" );
}

CON_COMMAND( ez2_nav_path_clear, "Clears the drawn navmesh test path" )
{
	s_TestPath.Purge();
}
