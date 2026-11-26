//=============================================================================
// navmesh_bspextract.cpp
//
// Purpose: Reads the current map's .bsp, extracts world (model 0) triangles
//          including displacements, and feeds them to BuildAndSaveFullSoloNavMesh.
//
// Console:  ez2_nav_build_full [cellsize]
//
// Notes:
//  - Only model 0 (worldspawn) is used, so func_door / func_brush etc. are NOT baked in.
//  - Static props are not included.
//  - Compressed (LZMA) lumps are not supported; compile with vbsp default (uncompressed).
//=============================================================================

#include "cbase.h"
#include "bspfile.h"
#include "filesystem.h"
#include "utlbuffer.h"

#include <vector>
#include <float.h>

#include "NavMeshBuilder_Full.h"   // FullNavTriangle_t, FullNavBuildConfig_t, BuildAndSaveFullSoloNavMesh

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IFileSystem* filesystem;

static ConVar ez2_nav_agent_radius( "ez2_nav_agent_radius", "16", 0, "Navmesh build: agent radius (units)" );
static ConVar ez2_nav_agent_height( "ez2_nav_agent_height", "72", 0, "Navmesh build: agent height (units)" );
static ConVar ez2_nav_agent_climb( "ez2_nav_agent_climb", "18", 0, "Navmesh build: max step height (units)" );
static ConVar ez2_nav_agent_slope( "ez2_nav_agent_slope", "45", 0, "Navmesh build: max walkable slope (degrees)" );
static ConVar ez2_nav_cell_size( "ez2_nav_cell_size", "8", 0, "Navmesh build: voxel cell size (units). Smaller = better and much slower" );
static ConVar ez2_nav_cell_height( "ez2_nav_cell_height", "4", 0, "Navmesh build: voxel cell height (units)" );

namespace
{
	// Surfaces that should never become nav geometry
	static const int NAV_SKIP_SURF_FLAGS =
		SURF_SKY | SURF_SKY2D | SURF_NODRAW | SURF_SKIP | SURF_HINT | SURF_TRIGGER;

	// Returns pointer + element count for a lump, with bounds/size validation.
	template <class T>
	bool GetLump( const unsigned char* pBase, int nFileSize, const dheader_t* pHdr, int lumpIdx, const T*& pOut, int& nCount )
	{
		pOut = NULL;
		nCount = 0;

		const lump_t& l = pHdr->lumps[lumpIdx];

		if ( l.fourCC[0] || l.fourCC[1] || l.fourCC[2] || l.fourCC[3] )
		{
			Warning( "[EZ2 NavMesh] Lump %d is compressed; unsupported.\n", lumpIdx );
			return false;
		}

		if ( l.filelen == 0 )
			return true;

		if ( l.fileofs < 0 || l.filelen < 0 || (int64)l.fileofs + l.filelen > nFileSize || ( l.filelen % (int)sizeof( T ) ) != 0 )
		{
			Warning( "[EZ2 NavMesh] Lump %d has invalid bounds/size.\n", lumpIdx );
			return false;
		}

		pOut = reinterpret_cast<const T*>( pBase + l.fileofs );
		nCount = l.filelen / (int)sizeof( T );
		return true;
	}

	// Emit a triangle wound so its geometric normal agrees with desiredNormal.
	// (Source faces are clockwise; Recast wants CCW-up, so we orient by normal rather than trusting winding.)
	void EmitTri( std::vector<FullNavTriangle_t>& out, const Vector& a, const Vector& b, const Vector& c, const Vector& desiredNormal )
	{
		Vector cr = ( b - a ).Cross( c - a );
		if ( cr.LengthSqr() < 0.0001f )
			return; // degenerate

		FullNavTriangle_t t;
		t.v0 = a;
		if ( DotProduct( cr, desiredNormal ) < 0.0f )
		{
			t.v1 = c;
			t.v2 = b;
		}
		else
		{
			t.v1 = b;
			t.v2 = c;
		}
		out.push_back( t );
	}
}

//-----------------------------------------------------------------------------
// Extracts world triangles from maps/<mapName>.bsp. Returns false on failure.
//-----------------------------------------------------------------------------
static bool ExtractWorldTriangles( const char* pszMapName, std::vector<FullNavTriangle_t>& outTris )
{
	outTris.clear();

	char szBspPath[MAX_PATH];
	Q_snprintf( szBspPath, sizeof( szBspPath ), "maps/%s.bsp", pszMapName );

	CUtlBuffer buf;
	if ( !filesystem->ReadFile( szBspPath, "GAME", buf ) )
	{
		Warning( "[EZ2 NavMesh] Could not read '%s'\n", szBspPath );
		return false;
	}

	const unsigned char* pBase = (const unsigned char*)buf.Base();
	const int nSize = buf.TellPut();

	if ( nSize < (int)sizeof( dheader_t ) )
		return false;

	const dheader_t* pHdr = reinterpret_cast<const dheader_t*>( pBase );
	if ( pHdr->ident != IDBSPHEADER )
	{
		Warning( "[EZ2 NavMesh] '%s' is not a valid BSP.\n", szBspPath );
		return false;
	}

	const Vector*       pVerts = NULL;     int nVerts = 0;
	const dedge_t*      pEdges = NULL;     int nEdges = 0;
	const int*          pSurfEdges = NULL; int nSurfEdges = 0;
	const dface_t*      pFaces = NULL;     int nFaces = 0;
	const texinfo_t*    pTexInfo = NULL;   int nTexInfo = 0;
	const dplane_t*     pPlanes = NULL;    int nPlanes = 0;
	const dmodel_t*     pModels = NULL;    int nModels = 0;
	const ddispinfo_t*  pDispInfo = NULL;  int nDispInfo = 0;
	const CDispVert*    pDispVerts = NULL; int nDispVerts = 0;

	if ( !GetLump( pBase, nSize, pHdr, LUMP_VERTEXES, pVerts, nVerts ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_EDGES, pEdges, nEdges ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_SURFEDGES, pSurfEdges, nSurfEdges ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_FACES, pFaces, nFaces ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_TEXINFO, pTexInfo, nTexInfo ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_PLANES, pPlanes, nPlanes ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_MODELS, pModels, nModels ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_DISPINFO, pDispInfo, nDispInfo ) ||
		 !GetLump( pBase, nSize, pHdr, LUMP_DISP_VERTS, pDispVerts, nDispVerts ) )
	{
		return false;
	}

	if ( nModels < 1 )
		return false;

	// Model 0 is the world
	const dmodel_t& world = pModels[0];
	const int firstFace = world.firstface;
	const int lastFace = world.firstface + world.numfaces;
	if ( firstFace < 0 || lastFace > nFaces )
	{
		Warning( "[EZ2 NavMesh] World model face range invalid.\n" );
		return false;
	}

	std::vector<Vector> poly;
	int nDispFaces = 0;

	for ( int fi = firstFace; fi < lastFace; ++fi )
	{
		const dface_t& f = pFaces[fi];

		if ( f.numedges < 3 )
			continue;
		if ( f.planenum >= nPlanes )
			continue;

		if ( f.texinfo >= 0 && f.texinfo < nTexInfo )
		{
			if ( pTexInfo[f.texinfo].flags & NAV_SKIP_SURF_FLAGS )
				continue;
		}

		// Collect polygon vertices in winding order
		poly.clear();
		bool bOk = true;
		for ( int i = 0; i < f.numedges; ++i )
		{
			const int seIdx = f.firstedge + i;
			if ( seIdx < 0 || seIdx >= nSurfEdges ) { bOk = false; break; }

			const int se = pSurfEdges[seIdx];
			const int eIdx = ( se >= 0 ) ? se : -se;
			if ( eIdx >= nEdges ) { bOk = false; break; }

			const int vIdx = ( se >= 0 ) ? pEdges[eIdx].v[0] : pEdges[eIdx].v[1];
			if ( vIdx >= nVerts ) { bOk = false; break; }

			poly.push_back( pVerts[vIdx] );
		}
		if ( !bOk )
			continue;

		Vector faceNormal = pPlanes[f.planenum].normal;
		if ( f.side )
			faceNormal = -faceNormal;

		// ---- Displacement face ----
		if ( f.dispinfo >= 0 )
		{
			if ( f.dispinfo >= nDispInfo || poly.size() != 4 )
				continue;

			const ddispinfo_t& di = pDispInfo[f.dispinfo];
			const int size = ( 1 << di.power ) + 1;
			if ( di.power < 2 || di.power > 4 )
				continue;
			if ( di.m_iDispVertStart < 0 || di.m_iDispVertStart + size * size > nDispVerts )
				continue;

			// Rotate corners so corner 0 is the one nearest the displacement start position
			int startCorner = 0;
			float bestDist = FLT_MAX;
			for ( int c = 0; c < 4; ++c )
			{
				float d = ( poly[c] - di.startPosition ).LengthSqr();
				if ( d < bestDist )
				{
					bestDist = d;
					startCorner = c;
				}
			}

			Vector pts[4];
			for ( int c = 0; c < 4; ++c )
				pts[c] = poly[( startCorner + c ) & 3];

			std::vector<Vector> grid( size * size );
			const float invStep = 1.0f / (float)( size - 1 );

			for ( int y = 0; y < size; ++y )
			{
				const Vector edge0 = pts[0] + ( pts[3] - pts[0] ) * ( y * invStep );
				const Vector edge1 = pts[1] + ( pts[2] - pts[1] ) * ( y * invStep );

				for ( int x = 0; x < size; ++x )
				{
					const Vector base = edge0 + ( edge1 - edge0 ) * ( x * invStep );
					const CDispVert& dv = pDispVerts[di.m_iDispVertStart + y * size + x];
					grid[y * size + x] = base + dv.m_vVector * dv.m_flDist;
				}
			}

			for ( int y = 0; y < size - 1; ++y )
			{
				for ( int x = 0; x < size - 1; ++x )
				{
					const Vector& p00 = grid[y * size + x];
					const Vector& p10 = grid[y * size + x + 1];
					const Vector& p11 = grid[( y + 1 ) * size + x + 1];
					const Vector& p01 = grid[( y + 1 ) * size + x];

					EmitTri( outTris, p00, p10, p11, faceNormal );
					EmitTri( outTris, p00, p11, p01, faceNormal );
				}
			}

			++nDispFaces;
			continue;
		}

		// ---- Regular convex face: fan triangulation ----
		for ( size_t i = 1; i + 1 < poly.size(); ++i )
		{
			EmitTri( outTris, poly[0], poly[i], poly[i + 1], faceNormal );
		}
	}

	Msg( "[EZ2 NavMesh] Extracted %d triangles from '%s' (%d displacement faces)\n",
		(int)outTris.size(), szBspPath, nDispFaces );

	return !outTris.empty();
}

//-----------------------------------------------------------------------------
// ez2_nav_build_full [cellsize]
//-----------------------------------------------------------------------------
CON_COMMAND( ez2_nav_build_full, "Builds and saves maps/<map>_full.nav from the current map's BSP: ez2_nav_build_full [cellsize]" )
{
	if ( !gpGlobals || gpGlobals->mapname == NULL_STRING )
	{
		Warning( "[EZ2 NavMesh] No map loaded.\n" );
		return;
	}

	const char* pszMap = STRING( gpGlobals->mapname );

	std::vector<FullNavTriangle_t> tris;
	if ( !ExtractWorldTriangles( pszMap, tris ) )
	{
		Warning( "[EZ2 NavMesh] Triangle extraction failed for '%s'.\n", pszMap );
		return;
	}

	// Horizontal bounds (Source X/Y) to keep the heightfield from exploding on big maps
	float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
	for ( size_t i = 0; i < tris.size(); ++i )
	{
		const Vector* v[3] = { &tris[i].v0, &tris[i].v1, &tris[i].v2 };
		for ( int k = 0; k < 3; ++k )
		{
			minX = MIN( minX, v[k]->x );
			maxX = MAX( maxX, v[k]->x );
			minY = MIN( minY, v[k]->y );
			maxY = MAX( maxY, v[k]->y );
		}
	}

	float cellSize = ez2_nav_cell_size.GetFloat();
	if ( args.ArgC() >= 2 )
		cellSize = atof( args[1] );
	if ( cellSize < 1.0f )
		cellSize = 1.0f;

	const float extX = maxX - minX;
	const float extY = maxY - minY;
	while ( ( extX / cellSize ) * ( extY / cellSize ) > 4.0e6f )
	{
		cellSize *= 2.0f;
		Warning( "[EZ2 NavMesh] Map is large; increasing cell size to %.1f\n", cellSize );
	}

	FullNavBuildConfig_t cfg;
	cfg.agentRadius = ez2_nav_agent_radius.GetFloat();
	cfg.agentHeight = ez2_nav_agent_height.GetFloat();
	cfg.agentMaxClimb = ez2_nav_agent_climb.GetFloat();
	cfg.agentMaxSlopeDeg = ez2_nav_agent_slope.GetFloat();
	cfg.cellSize = cellSize;
	cfg.cellHeight = ez2_nav_cell_height.GetFloat();

	Msg( "[EZ2 NavMesh] Building '%s' (%d tris, cs=%.1f ch=%.1f r=%.1f h=%.1f climb=%.1f slope=%.1f)...\n",
		pszMap, (int)tris.size(), cfg.cellSize, cfg.cellHeight, cfg.agentRadius, cfg.agentHeight,
		cfg.agentMaxClimb, cfg.agentMaxSlopeDeg );

	const double t0 = Plat_FloatTime();
	const bool bOk = BuildAndSaveFullSoloNavMesh( pszMap, tris, cfg, true );
	const double dt = Plat_FloatTime() - t0;

	if ( bOk )
		Msg( "[EZ2 NavMesh] Build succeeded in %.2fs. Set ez2_nav_mode 1 and run ez2_nav_status.\n", dt );
	else
		Warning( "[EZ2 NavMesh] Build FAILED after %.2fs (see [Recast] output with developer 1).\n", dt );
}
