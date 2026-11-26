//=============================================================================
// NavMeshBuilder_Node.cpp
//
// Purpose: Builds a Detour navmesh that reproduces the existing CAI_Network
//          nodegraph instead of reading map geometry, while baking walkable
//          pads and translated hint records for all CAI_Hint entities.
//
// Console: ez2_nav_build_node [cellsize]
//          ez2_nav_debug_hints
//=============================================================================

#include "cbase.h"

#include "NavMeshBuilder_Node.h"
#include "NavMeshManager.h"

#include "ai_network.h"
#include "ai_node.h"
#include "ai_link.h"
#include "ai_hull.h"
#include "ai_basenpc.h"
#include "ai_hint.h"
#include "ndebugoverlay.h"

#include <vector>
#include <string.h>
#include <float.h>
#include <math.h>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

#include "filesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IFileSystem* filesystem;

namespace
{
	// Area ids used during rasterization. Higher id wins when spans merge,
	// so normal ground (63) beats dynamic links (5) where they overlap at node pads.
	static const unsigned char RC_AREA_DYNLINK = 5;

	class CRecastNodeBuildContext : public rcContext
	{
	protected:
		virtual void doLog(const rcLogCategory category, const char* msg, const int len) override
		{
			NOTE_UNUSED(category);
			NOTE_UNUSED(len);
			if (msg && msg[0])
			{
				DevMsg("[Recast/Node] %s\n", msg);
			}
		}
	};

	// Frees Recast intermediates on any exit path.
	struct RcScope_t
	{
		rcHeightfield* solid;
		rcCompactHeightfield* chf;
		rcContourSet* cset;
		rcPolyMesh* pmesh;
		rcPolyMeshDetail* dmesh;

		RcScope_t() : solid(NULL), chf(NULL), cset(NULL), pmesh(NULL), dmesh(NULL) {}
		~RcScope_t()
		{
			rcFreeHeightField(solid);
			rcFreeCompactHeightfield(chf);
			rcFreeContourSet(cset);
			rcFreePolyMesh(pmesh);
			rcFreePolyMeshDetail(dmesh);
		}
	};

	inline void SourceToRecast(const Vector& in, float out[3])
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

	// Flat floor geometry accumulator (3 verts per triangle, sequential indices)
	struct FloorGeo_t
	{
		std::vector<float>         verts;
		std::vector<unsigned char> dyn;   // per triangle: 1 = dynamic/OFF link

		int TriCount() const { return (int)dyn.size(); }

		// Always emits the triangle with an upward-facing normal (+Z in Source space).
		void AddTri(const Vector& a, const Vector& b, const Vector& c, bool bDyn)
		{
			Vector n = (b - a).Cross(c - a);
			if (fabsf(n.z) < 0.01f)
				return; // degenerate

			Vector v1 = b, v2 = c;
			if (n.z < 0.0f)
			{
				v1 = c;
				v2 = b;
			}

			const Vector* pts[3] = { &a, &v1, &v2 };
			for (int i = 0; i < 3; ++i)
			{
				float r[3];
				SourceToRecast(*pts[i], r);
				verts.push_back(r[0]);
				verts.push_back(r[1]);
				verts.push_back(r[2]);
			}
			dyn.push_back(bDyn ? 1 : 0);
		}

		void AddQuad(const Vector& a, const Vector& b, const Vector& c, const Vector& d, bool bDyn)
		{
			AddTri(a, b, c, bDyn);
			AddTri(a, c, d, bDyn);
		}

		void AddPad(const Vector& center, float half)
		{
			AddQuad(
				center + Vector(-half, -half, 0),
				center + Vector(half, -half, 0),
				center + Vector(half, half, 0),
				center + Vector(-half, half, 0),
				false);
		}

		void AddCorridor(const Vector& a, const Vector& b, float half, bool bDyn)
		{
			Vector dir = b - a;
			dir.z = 0.0f;
			const float len = VectorNormalize(dir);
			if (len < 1.0f)
				return;

			const Vector perp(-dir.y, dir.x, 0.0f);
			AddQuad(
				a + perp * half,
				a - perp * half,
				b - perp * half,
				b + perp * half,
				bDyn);
		}
	};

	struct OffMeshConn_t
	{
		float a[3];
		float b[3];
	};

	EZ2_NavHintType_t TranslateSourceHintType(Hint_e sourceType)
	{
		switch (sourceType)
		{
		case HINT_TACTICAL_COVER_LOW:
			return EZ2_NAVHINT_COVER_CROUCH;

		case HINT_TACTICAL_COVER_MED:
			return EZ2_NAVHINT_COVER_STAND;

		case HINT_TACTICAL_PINCH:
		case HINT_TACTICAL_ENEMY_DISADVANTAGED:
			return EZ2_NAVHINT_PINCH_OR_FLANK;

		case HINT_WORLD_WINDOW:
			return EZ2_NAVHINT_VANTAGE_SNIPER;

		case HINT_NPC_EXIT_POINT:
			return EZ2_NAVHINT_ENTRANCE_EXIT;

		case HINT_WORLD_WORK_POSITION:
		case HINT_WORLD_VISUALLY_INTERESTING:
		case HINT_WORLD_VISUALLY_INTERESTING_DONT_AIM:
		case HINT_WORLD_VISUALLY_INTERESTING_STEALTH:
			return EZ2_NAVHINT_PATROL_OR_IDLE;

		case HINT_ANTLION_BURROW_POINT:
		case HINT_STRIDER_NODE:
		case HINT_HEADCRAB_BURROW_POINT:
		case HINT_CROW_FLYTO_POINT:
			return EZ2_NAVHINT_JUMP_OR_CLIMB;

		case HINT_NONE:
			return EZ2_NAVHINT_NONE;

		default:
			return EZ2_NAVHINT_CUSTOM;
		}
	}

	Vector ResolveHintFloorPos(CAI_Hint* pHint, Hull_t hull)
	{
		Vector vecPos;
		pHint->GetPosition(hull, &vecPos);

		trace_t tr;
		UTIL_TraceLine(
			vecPos + Vector(0, 0, 16.0f),
			vecPos - Vector(0, 0, 64.0f),
			MASK_NPCSOLID_BRUSHONLY,
			NULL,
			COLLISION_GROUP_NONE,
			&tr
		);

		return tr.DidHit() ? tr.endpos : vecPos;
	}

	int FindNearestGroundNode(CAI_Network* pNet, const Vector& vecPos)
	{
		if (!pNet)
			return NO_NODE;

		int bestID = NO_NODE;
		float bestDistSqr = 512.0f * 512.0f;

		const int nNodes = pNet->NumNodes();
		for (int i = 0; i < nNodes; ++i)
		{
			CAI_Node* pNode = pNet->GetNode(i);
			if (!pNode || pNode->GetType() != NODE_GROUND)
				continue;

			float d2 = vecPos.DistToSqr(pNode->GetOrigin());
			if (d2 < bestDistSqr)
			{
				bestDistSqr = d2;
				bestID = i;
			}
		}

		return bestID;
	}
}

//-----------------------------------------------------------------------------
// Core builder
//-----------------------------------------------------------------------------
bool BuildAndSaveNodeSoloNavMesh(const char* pszMapName, const NodeNavBuildConfig_t& config, bool bAttachImmediately)
{
	if (!pszMapName || !pszMapName[0])
		return false;

	if (!g_pBigAINet || g_pBigAINet->NumNodes() <= 0)
	{
		Warning("[EZ2 NavMesh] No nodegraph loaded (g_pBigAINet empty). Build/load the .ain first.\n");
		return false;
	}

	const int hull = clamp(config.hull, 0, NUM_HULLS - 1);
	const float geoHalf = config.corridorHalfWidth + config.agentRadius;

	//-------------------------------------------------------------------------
	// 1. Nodegraph + hint nodes -> floor geometry + off-mesh connections
	//-------------------------------------------------------------------------
	FloorGeo_t geo;
	std::vector<OffMeshConn_t> offMesh;
	std::vector<EZ2_NavHint_t> translatedHints;

	CAI_Network* pNet = g_pBigAINet;
	const int nNodes = pNet->NumNodes();

	int nGroundNodes = 0, nGroundLinks = 0, nDynLinks = 0, nJumpLinks = 0;

	for (int i = 0; i < nNodes; ++i)
	{
		CAI_Node* pNode = pNet->GetNode(i);
		if (!pNode || pNode->GetType() != NODE_GROUND)
			continue;

		const Vector& vPos = pNode->GetOrigin();
		geo.AddPad(vPos, geoHalf);
		++nGroundNodes;

		for (int l = 0; l < pNode->NumLinks(); ++l)
		{
			CAI_Link* pLink = pNode->GetLinkByIndex(l);
			if (!pLink)
				continue;

			// Each link is stored on both nodes; handle it once, from its source node
			if (pLink->m_iSrcID != i)
				continue;

			CAI_Node* pDest = pNet->GetNode(pLink->DestNodeID(i));
			if (!pDest || pDest->GetType() != NODE_GROUND)
				continue;

			const int moveTypes = pLink->m_iAcceptedMoveTypes[hull];
			const bool bDynamic = (pLink->m_pDynamicLink != NULL) || ((pLink->m_LinkInfo & bits_LINK_OFF) != 0);

			if (bDynamic && !config.includeOffLinks)
				continue;

			if (moveTypes & bits_CAP_MOVE_GROUND)
			{
				geo.AddCorridor(vPos, pDest->GetOrigin(), geoHalf, bDynamic);
				++nGroundLinks;
				if (bDynamic)
					++nDynLinks;
			}
			else if ((moveTypes & bits_CAP_MOVE_JUMP) && config.includeJumpLinks)
			{
				OffMeshConn_t con;
				SourceToRecast(vPos, con.a);
				SourceToRecast(pDest->GetOrigin(), con.b);
				offMesh.push_back(con);
				++nJumpLinks;
			}
		}
	}

	// Bake a pad + link corridor for every CAI_Hint in the map and record translated hints
	AIHintIter_t hintIter;
	CAI_Hint* pCurHint = CAI_HintManager::GetFirstHint(&hintIter);
	while (pCurHint)
	{
		Vector vecHintPos = ResolveHintFloorPos(pCurHint, static_cast<Hull_t>(hull));
		geo.AddPad(vecHintPos, geoHalf);

		CAI_Node* pLinkedNode = pCurHint->GetNode();
		if (pLinkedNode && pLinkedNode->GetType() == NODE_GROUND)
		{
			geo.AddCorridor(vecHintPos, pLinkedNode->GetOrigin(), geoHalf, false);
		}
		else
		{
			int nearestID = FindNearestGroundNode(pNet, vecHintPos);
			if (nearestID != NO_NODE)
			{
				CAI_Node* pNear = pNet->GetNode(nearestID);
				if (pNear)
				{
					geo.AddCorridor(vecHintPos, pNear->GetOrigin(), geoHalf, false);
				}
			}
		}

		EZ2_NavHint_t navHint;
		navHint.vecPos = vecHintPos;
		navHint.flYaw = pCurHint->Yaw();
		navHint.flFovCos = 1.0f;
#ifdef MAPBASE
		navHint.flRadius = static_cast<float>(pCurHint->GetRadius());
#else
		navHint.flRadius = 0.0f;
#endif
		navHint.flWeight = pCurHint->GetHintWeight();
		navHint.sourceHintType = static_cast<int>(pCurHint->HintType());
		navHint.navHintType = TranslateSourceHintType(pCurHint->HintType());
		navHint.sourceWCNodeID = pCurHint->GetWCId();
		navHint.polyRef = 0;

		translatedHints.push_back(navHint);

		pCurHint = CAI_HintManager::GetNextHint(&hintIter);
	}

	const int triCount = geo.TriCount();
	const int vertCount = triCount * 3;

	Msg("[EZ2 NavMesh] Nodegraph: %d ground nodes, %d ground links (%d dynamic), %d jump links, %d hints -> %d tris\n",
		nGroundNodes, nGroundLinks, nDynLinks, nJumpLinks, static_cast<int>(translatedHints.size()), triCount);

	if (triCount <= 0)
	{
		Warning("[EZ2 NavMesh] Nodegraph produced no ground geometry for hull %d.\n", hull);
		return false;
	}

	std::vector<int> tris(triCount * 3);
	for (int i = 0; i < triCount * 3; ++i)
		tris[i] = i;

	//-------------------------------------------------------------------------
	// 2. Bounds + Recast config
	//-------------------------------------------------------------------------
	float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

	for (int i = 0; i < vertCount; ++i)
	{
		for (int c = 0; c < 3; ++c)
		{
			const float v = geo.verts[i * 3 + c];
			if (v < bmin[c]) bmin[c] = v;
			if (v > bmax[c]) bmax[c] = v;
		}
	}
	for (size_t i = 0; i < offMesh.size(); ++i)
	{
		const float* e[2] = { offMesh[i].a, offMesh[i].b };
		for (int k = 0; k < 2; ++k)
			for (int c = 0; c < 3; ++c)
			{
				if (e[k][c] < bmin[c]) bmin[c] = e[k][c];
				if (e[k][c] > bmax[c]) bmax[c] = e[k][c];
			}
	}

	float cellSize = MAX(config.cellSize, 1.0f);

	// Keep the heightfield from exploding on big maps
	while (((bmax[0] - bmin[0]) / cellSize) * ((bmax[2] - bmin[2]) / cellSize) > 4.0e6f)
	{
		cellSize *= 2.0f;
		Warning("[EZ2 NavMesh] Large nodegraph extents; increasing cell size to %.1f\n", cellSize);
	}

	bmin[0] -= config.agentRadius + cellSize * 2.0f;
	bmax[0] += config.agentRadius + cellSize * 2.0f;
	bmin[2] -= config.agentRadius + cellSize * 2.0f;
	bmax[2] += config.agentRadius + cellSize * 2.0f;
	bmin[1] -= config.agentHeight;
	bmax[1] += config.agentHeight;

	rcConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.cs = cellSize;
	cfg.ch = config.cellHeight;
	cfg.walkableSlopeAngle = config.agentMaxSlopeDeg;
	cfg.walkableHeight = static_cast<int>(ceilf(config.agentHeight / cfg.ch));
	cfg.walkableClimb = static_cast<int>(floorf(config.agentMaxClimb / cfg.ch));
	cfg.walkableRadius = static_cast<int>(ceilf(config.agentRadius / cfg.cs));
	cfg.maxEdgeLen = static_cast<int>((config.agentRadius * 8.0f) / cfg.cs);
	cfg.maxSimplificationError = 1.3f;
	cfg.minRegionArea = 4;                 // small: corridors are narrow and must not be culled as islands
	cfg.mergeRegionArea = static_cast<int>(rcSqr(20));
	cfg.maxVertsPerPoly = 6;
	cfg.detailSampleDist = cfg.cs * 6.0f;
	cfg.detailSampleMaxError = cfg.ch * 1.0f;
	rcVcopy(cfg.bmin, bmin);
	rcVcopy(cfg.bmax, bmax);
	rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

	if (cfg.width <= 0 || cfg.height <= 0)
		return false;

	//-------------------------------------------------------------------------
	// 3. Recast pipeline
	//-------------------------------------------------------------------------
	CRecastNodeBuildContext ctx;
	RcScope_t rc;

	rc.solid = rcAllocHeightfield();
	if (!rc.solid || !rcCreateHeightfield(&ctx, *rc.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
		return false;

	std::vector<unsigned char> triAreas(triCount, 0);
	rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, &geo.verts[0], vertCount, &tris[0], triCount, &triAreas[0]);

	for (int i = 0; i < triCount; ++i)
	{
		if (triAreas[i] == RC_WALKABLE_AREA && geo.dyn[i])
			triAreas[i] = RC_AREA_DYNLINK;
	}

	if (!rcRasterizeTriangles(&ctx, &geo.verts[0], vertCount, &tris[0], &triAreas[0], triCount, *rc.solid, cfg.walkableClimb))
		return false;

	// NOTE: deliberately NO rcFilterLowHangingWalkableObstacles / rcFilterLedgeSpans.
	// There is no real floor beyond the corridor quads, so the ledge filter would
	// strip every edge cell and delete narrow corridors entirely.
	rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *rc.solid);

	rc.chf = rcAllocCompactHeightfield();
	if (!rc.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *rc.solid, *rc.chf))
		return false;

	rcFreeHeightField(rc.solid);
	rc.solid = NULL;

	if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *rc.chf) ||
		!rcBuildDistanceField(&ctx, *rc.chf) ||
		!rcBuildRegions(&ctx, *rc.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
	{
		return false;
	}

	rc.cset = rcAllocContourSet();
	if (!rc.cset || !rcBuildContours(&ctx, *rc.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *rc.cset))
		return false;

	rc.pmesh = rcAllocPolyMesh();
	if (!rc.pmesh || !rcBuildPolyMesh(&ctx, *rc.cset, cfg.maxVertsPerPoly, *rc.pmesh))
		return false;

	if (rc.pmesh->npolys <= 0)
	{
		Warning("[EZ2 NavMesh] Recast produced 0 polys. Try a smaller cell size or larger ez2_nav_node_corridor.\n");
		return false;
	}

	rc.dmesh = rcAllocPolyMeshDetail();
	if (!rc.dmesh || !rcBuildPolyMeshDetail(&ctx, *rc.pmesh, *rc.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *rc.dmesh))
		return false;

	rcFreeCompactHeightfield(rc.chf);
	rc.chf = NULL;
	rcFreeContourSet(rc.cset);
	rc.cset = NULL;

	// Map rasterization areas to final poly areas / flags
	for (int i = 0; i < rc.pmesh->npolys; ++i)
	{
		if (rc.pmesh->areas[i] == RC_NULL_AREA)
		{
			rc.pmesh->flags[i] = 0;
			continue;
		}

		if (rc.pmesh->areas[i] == RC_AREA_DYNLINK)
		{
			rc.pmesh->areas[i] = EZ2_NAV_AREA_DYNLINK;
			rc.pmesh->flags[i] = EZ2_NAV_FLAG_WALK | EZ2_NAV_FLAG_DYNLINK;
		}
		else
		{
			rc.pmesh->areas[i] = EZ2_NAV_AREA_GROUND;
			rc.pmesh->flags[i] = EZ2_NAV_FLAG_WALK;
		}
	}

	//-------------------------------------------------------------------------
	// 4. Detour data (with off-mesh jump connections)
	//-------------------------------------------------------------------------
	const int nOff = (int)offMesh.size();
	std::vector<float>          offVerts(nOff * 6);
	std::vector<float>          offRad(nOff);
	std::vector<unsigned char>  offDir(nOff);
	std::vector<unsigned char>  offAreas(nOff);
	std::vector<unsigned short> offFlags(nOff);
	std::vector<unsigned int>   offIds(nOff);

	for (int i = 0; i < nOff; ++i)
	{
		memcpy(&offVerts[i * 6 + 0], offMesh[i].a, sizeof(float) * 3);
		memcpy(&offVerts[i * 6 + 3], offMesh[i].b, sizeof(float) * 3);
		offRad[i] = config.corridorHalfWidth;
		offDir[i] = DT_OFFMESH_CON_BIDIR;
		offAreas[i] = EZ2_NAV_AREA_JUMP;
		offFlags[i] = EZ2_NAV_FLAG_WALK | EZ2_NAV_FLAG_JUMP;
		offIds[i] = (unsigned int)i;
	}

	dtNavMeshCreateParams params;
	memset(&params, 0, sizeof(params));
	params.verts = rc.pmesh->verts;
	params.vertCount = rc.pmesh->nverts;
	params.polys = rc.pmesh->polys;
	params.polyAreas = rc.pmesh->areas;
	params.polyFlags = rc.pmesh->flags;
	params.polyCount = rc.pmesh->npolys;
	params.nvp = rc.pmesh->nvp;
	params.detailMeshes = rc.dmesh->meshes;
	params.detailVerts = rc.dmesh->verts;
	params.detailVertsCount = rc.dmesh->nverts;
	params.detailTris = rc.dmesh->tris;
	params.detailTriCount = rc.dmesh->ntris;

	if (nOff > 0)
	{
		params.offMeshConVerts = &offVerts[0];
		params.offMeshConRad = &offRad[0];
		params.offMeshConDir = &offDir[0];
		params.offMeshConAreas = &offAreas[0];
		params.offMeshConFlags = &offFlags[0];
		params.offMeshConUserID = &offIds[0];
		params.offMeshConCount = nOff;
	}

	params.walkableHeight = config.agentHeight;
	params.walkableRadius = config.agentRadius;
	params.walkableClimb = config.agentMaxClimb;
	rcVcopy(params.bmin, rc.pmesh->bmin);
	rcVcopy(params.bmax, rc.pmesh->bmax);
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;

	unsigned char* pNavData = NULL;
	int navDataSize = 0;
	if (!dtCreateNavMeshData(&params, &pNavData, &navDataSize) || !pNavData || navDataSize <= 0)
	{
		Warning("[EZ2 NavMesh] dtCreateNavMeshData failed.\n");
		return false;
	}

	dtNavMesh* pNavMesh = dtAllocNavMesh();
	if (!pNavMesh)
	{
		dtFree(pNavData);
		return false;
	}

	dtNavMeshParams meshParams;
	memset(&meshParams, 0, sizeof(meshParams));
	rcVcopy(meshParams.orig, params.bmin);
	meshParams.tileWidth = params.bmax[0] - params.bmin[0];
	meshParams.tileHeight = params.bmax[2] - params.bmin[2];
	meshParams.maxTiles = 1;
	meshParams.maxPolys = 1 << 20;

	if (dtStatusFailed(pNavMesh->init(&meshParams)) ||
		dtStatusFailed(pNavMesh->addTile(pNavData, navDataSize, DT_TILE_FREE_DATA, 0, NULL)))
	{
		dtFree(pNavData);
		dtFreeNavMesh(pNavMesh);
		return false;
	}

	//-------------------------------------------------------------------------
	// 5. Snap hints, save, and attach
	//-------------------------------------------------------------------------
	// Temporarily attach so SetNavHints can snap every hint onto its dtPolyRef before SaveSoloNavMesh

	if (!g_EZ2NavMeshManager.SetNavMesh(pNavMesh, EZ2_NAV_PROFILE_NODEGRAPH))
		return false;

	g_EZ2NavMeshManager.SetNavHints(translatedHints);

	int boundHints = 0;
	const std::vector<EZ2_NavHint_t>& snapped = g_EZ2NavMeshManager.GetNavHints();
	for (size_t i = 0; i < snapped.size(); ++i)
	{
		if (snapped[i].polyRef != 0)
			++boundHints;
	}

	char szModPath[MAX_PATH];
	Q_snprintf(szModPath, sizeof(szModPath), "maps/%s_node.nav", pszMapName);

	const bool bSaved = g_EZ2NavMeshManager.SaveSoloNavMesh(szModPath, g_EZ2NavMeshManager.GetNavMesh(), EZ2_NAV_PROFILE_NODEGRAPH);
	if (bSaved)
	{
		Msg("[EZ2 NavMesh] Saved nodegraph navmesh to '%s' (%d bytes, %d polys, %d off-mesh, %d/%d hints baked)\n",
			szModPath, navDataSize, rc.pmesh->npolys, nOff, boundHints, static_cast<int>(snapped.size()));
	}
	else
	{
		Warning("[EZ2 NavMesh] Failed saving '%s'\n", szModPath);
	}

	if (!bAttachImmediately)
	{
		g_EZ2NavMeshManager.UnloadMap();
	}

	return bSaved;
}

//-----------------------------------------------------------------------------
// Console commands
//-----------------------------------------------------------------------------
static ConVar ez2_nav_node_agent_radius("ez2_nav_node_agent_radius", "16", 0, "Node navmesh build: agent radius");
static ConVar ez2_nav_node_agent_height("ez2_nav_node_agent_height", "72", 0, "Node navmesh build: agent height");
static ConVar ez2_nav_node_agent_climb("ez2_nav_node_agent_climb", "18", 0, "Node navmesh build: max step height");
static ConVar ez2_nav_node_agent_slope("ez2_nav_node_agent_slope", "60", 0, "Node navmesh build: max slope (degrees) for link quads");
static ConVar ez2_nav_node_cell_size("ez2_nav_node_cell_size", "8", 0, "Node navmesh build: cell size");
static ConVar ez2_nav_node_cell_height("ez2_nav_node_cell_height", "4", 0, "Node navmesh build: cell height");
static ConVar ez2_nav_node_corridor("ez2_nav_node_corridor", "24", 0, "Node navmesh build: walkable half-width around links, after erosion");
static ConVar ez2_nav_node_hull("ez2_nav_node_hull", "0", 0, "Node navmesh build: Hull_t used to filter links (0 = HULL_HUMAN)");
static ConVar ez2_nav_node_include_off("ez2_nav_node_include_off", "1", 0, "Node navmesh build: 1 = include OFF/dynamic links (tagged with their own area)");
static ConVar ez2_nav_node_jump_links("ez2_nav_node_jump_links", "1", 0, "Node navmesh build: 1 = convert jump links to off-mesh connections");

CON_COMMAND(ez2_nav_build_node, "Builds and saves maps/<map>_node.nav from the loaded nodegraph: ez2_nav_build_node [cellsize]")
{
	if (!gpGlobals || gpGlobals->mapname == NULL_STRING)
	{
		Warning("[EZ2 NavMesh] No map loaded.\n");
		return;
	}

	NodeNavBuildConfig_t cfg;
	cfg.agentRadius = ez2_nav_node_agent_radius.GetFloat();
	cfg.agentHeight = ez2_nav_node_agent_height.GetFloat();
	cfg.agentMaxClimb = ez2_nav_node_agent_climb.GetFloat();
	cfg.agentMaxSlopeDeg = ez2_nav_node_agent_slope.GetFloat();
	cfg.cellSize = ez2_nav_node_cell_size.GetFloat();
	cfg.cellHeight = ez2_nav_node_cell_height.GetFloat();
	cfg.corridorHalfWidth = ez2_nav_node_corridor.GetFloat();
	cfg.hull = ez2_nav_node_hull.GetInt();
	cfg.includeOffLinks = ez2_nav_node_include_off.GetBool();
	cfg.includeJumpLinks = ez2_nav_node_jump_links.GetBool();

	if (args.ArgC() >= 2)
		cfg.cellSize = atof(args[1]);

	const char* pszMap = STRING(gpGlobals->mapname);

	const double t0 = Plat_FloatTime();
	const bool bOk = BuildAndSaveNodeSoloNavMesh(pszMap, cfg, true);
	const double dt = Plat_FloatTime() - t0;

	if (bOk)
		Msg("[EZ2 NavMesh] Node build succeeded in %.2fs. Set ez2_nav_mode 2 and run ez2_nav_status.\n", dt);
	else
		Warning("[EZ2 NavMesh] Node build FAILED after %.2fs (use developer 1 for [Recast/Node] output).\n", dt);
}

CON_COMMAND(ez2_nav_debug_hints, "Draws debug boxes and facing lines for baked navmesh hints")
{
	const std::vector<EZ2_NavHint_t>& hints = g_EZ2NavMeshManager.GetNavHints();
	Msg("[EZ2 NavMesh] Baked navmesh hints: %d\n", static_cast<int>(hints.size()));

	for (size_t i = 0; i < hints.size(); ++i)
	{
		const EZ2_NavHint_t& h = hints[i];
		bool bBound = (h.polyRef != 0);
		NDebugOverlay::Box(
			h.vecPos,
			Vector(-12, -12, 0),
			Vector(12, 12, 32),
			bBound ? 0 : 255,
			bBound ? 255 : 64,
			bBound ? 128 : 64,
			128,
			10.0f
		);

		Vector vecDir = UTIL_YawToVector(h.flYaw);
		NDebugOverlay::Line(
			h.vecPos + Vector(0, 0, 16),
			h.vecPos + Vector(0, 0, 16) + vecDir * 36.0f,
			255, 255, 0, true, 10.0f
		);
	}
}