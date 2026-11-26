#include "NavMeshBuilder_Full.h"
#include "NavMeshManager.h"

#include <string.h>
#include <float.h>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"

// Source filesystem & console output helpers
#include "filesystem.h"
#include "tier0/dbg.h"
#include "tier1/strtools.h"
#include "tier1/convar.h"

extern IFileSystem* filesystem;

namespace
{
	inline void SourceVecToRecast(const Vector& src, float dst[3])
	{
		dst[0] = src.x;
		dst[1] = src.z;
		dst[2] = -src.y;
	}

	class CRecastBuildContext : public rcContext
	{
	protected:
		virtual void doLog(const rcLogCategory category, const char* msg, const int len) override
		{
			NOTE_UNUSED(category);
			NOTE_UNUSED(len);
			if (msg && msg[0])
			{
				DevMsg("[Recast] %s\n", msg);
			}
		}
	};
}

bool BuildAndSaveFullSoloNavMesh(
	const char* pszMapName,
	const std::vector<FullNavTriangle_t>& triangles,
	const FullNavBuildConfig_t& config,
	bool bAttachImmediately
)
{
	if (!pszMapName || !pszMapName[0] || triangles.empty())
		return false;

	const int triCount = static_cast<int>(triangles.size());
	const int vertCount = triCount * 3;

	float* pVerts = new float[vertCount * 3];
	int* pTris = new int[triCount * 3];
	unsigned char* pTriAreas = new unsigned char[triCount];

	if (!pVerts || !pTris || !pTriAreas)
	{
		delete[] pVerts;
		delete[] pTris;
		delete[] pTriAreas;
		return false;
	}

	float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

	for (int i = 0; i < triCount; ++i)
	{
		const FullNavTriangle_t& tri = triangles[i];
		float r0[3], r1[3], r2[3];
		SourceVecToRecast(tri.v0, r0);
		SourceVecToRecast(tri.v1, r1);
		SourceVecToRecast(tri.v2, r2);

		const float* pts[3] = { r0, r1, r2 };
		for (int k = 0; k < 3; ++k)
		{
			const int vIdx = (i * 3 + k) * 3;
			pVerts[vIdx + 0] = pts[k][0];
			pVerts[vIdx + 1] = pts[k][1];
			pVerts[vIdx + 2] = pts[k][2];

			for (int c = 0; c < 3; ++c)
			{
				if (pts[k][c] < bmin[c]) bmin[c] = pts[k][c];
				if (pts[k][c] > bmax[c]) bmax[c] = pts[k][c];
			}
		}

		pTris[i * 3 + 0] = i * 3 + 0;
		pTris[i * 3 + 1] = i * 3 + 1;
		pTris[i * 3 + 2] = i * 3 + 2;
		pTriAreas[i] = 0;
	}

	// Pad bounds slightly around agent radius/height
	bmin[0] -= config.agentRadius;
	bmax[0] += config.agentRadius;
	bmin[2] -= config.agentRadius;
	bmax[2] += config.agentRadius;
	bmin[1] -= config.agentHeight;
	bmax[1] += config.agentHeight;

	rcConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.cs = config.cellSize;
	cfg.ch = config.cellHeight;
	cfg.walkableSlopeAngle = config.agentMaxSlopeDeg;
	cfg.walkableHeight = static_cast<int>(ceilf(config.agentHeight / cfg.ch));
	cfg.walkableClimb = static_cast<int>(ceilf(config.agentMaxClimb / cfg.ch));
	cfg.walkableRadius = static_cast<int>(ceilf(config.agentRadius / cfg.cs));
	cfg.maxEdgeLen = static_cast<int>((config.agentRadius * 8.0f) / cfg.cs);
	cfg.maxSimplificationError = 1.3f;
	cfg.minRegionArea = static_cast<int>(rcSqr(8));
	cfg.mergeRegionArea = static_cast<int>(rcSqr(20));
	cfg.maxVertsPerPoly = 6;
	cfg.detailSampleDist = cfg.cs * 6.0f;
	cfg.detailSampleMaxError = cfg.ch * 1.0f;
	rcVcopy(cfg.bmin, bmin);
	rcVcopy(cfg.bmax, bmax);

	rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
	if (cfg.width <= 0 || cfg.height <= 0)
	{
		delete[] pVerts;
		delete[] pTris;
		delete[] pTriAreas;
		return false;
	}

	CRecastBuildContext ctx;

	rcHeightfield* pSolid = rcAllocHeightfield();
	if (!pSolid || !rcCreateHeightfield(&ctx, *pSolid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
	{
		rcFreeHeightField(pSolid);
		delete[] pVerts;
		delete[] pTris;
		delete[] pTriAreas;
		return false;
	}

	rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, pVerts, vertCount, pTris, triCount, pTriAreas);
	if (!rcRasterizeTriangles(&ctx, pVerts, vertCount, pTris, pTriAreas, triCount, *pSolid, cfg.walkableClimb))
	{
		rcFreeHeightField(pSolid);
		delete[] pVerts;
		delete[] pTris;
		delete[] pTriAreas;
		return false;
	}

	// Verts/tris no longer needed after rasterization
	delete[] pVerts;
	delete[] pTris;
	pVerts = NULL;
	pTris = NULL;

	rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *pSolid);
	rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *pSolid);
	rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *pSolid);

	rcCompactHeightfield* pCHF = rcAllocCompactHeightfield();
	if (!pCHF || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *pSolid, *pCHF))
	{
		rcFreeHeightField(pSolid);
		rcFreeCompactHeightfield(pCHF);
		delete[] pTriAreas;
		return false;
	}

	rcFreeHeightField(pSolid);
	pSolid = NULL;

	if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *pCHF) ||
		!rcBuildDistanceField(&ctx, *pCHF) ||
		!rcBuildRegions(&ctx, *pCHF, 0, cfg.minRegionArea, cfg.mergeRegionArea))
	{
		rcFreeCompactHeightfield(pCHF);
		delete[] pTriAreas;
		return false;
	}

	delete[] pTriAreas;
	pTriAreas = NULL;

	rcContourSet* pCSet = rcAllocContourSet();
	if (!pCSet || !rcBuildContours(&ctx, *pCHF, cfg.maxSimplificationError, cfg.maxEdgeLen, *pCSet))
	{
		rcFreeCompactHeightfield(pCHF);
		rcFreeContourSet(pCSet);
		return false;
	}

	rcPolyMesh* pPMesh = rcAllocPolyMesh();
	if (!pPMesh || !rcBuildPolyMesh(&ctx, *pCSet, cfg.maxVertsPerPoly, *pPMesh))
	{
		rcFreeCompactHeightfield(pCHF);
		rcFreeContourSet(pCSet);
		rcFreePolyMesh(pPMesh);
		return false;
	}

	rcPolyMeshDetail* pDMesh = rcAllocPolyMeshDetail();
	if (!pDMesh || !rcBuildPolyMeshDetail(&ctx, *pPMesh, *pCHF, cfg.detailSampleDist, cfg.detailSampleMaxError, *pDMesh))
	{
		rcFreeCompactHeightfield(pCHF);
		rcFreeContourSet(pCSet);
		rcFreePolyMesh(pPMesh);
		rcFreePolyMeshDetail(pDMesh);
		return false;
	}

	rcFreeCompactHeightfield(pCHF);
	rcFreeContourSet(pCSet);

	// Assign default walkable poly flags
	for (int i = 0; i < pPMesh->npolys; ++i)
	{
		if (pPMesh->areas[i] != RC_NULL_AREA)
		{
			pPMesh->areas[i] = 0;
			pPMesh->flags[i] = 0x01;
		}
	}

	dtNavMeshCreateParams params;
	memset(&params, 0, sizeof(params));
	params.verts = pPMesh->verts;
	params.vertCount = pPMesh->nverts;
	params.polys = pPMesh->polys;
	params.polyAreas = pPMesh->areas;
	params.polyFlags = pPMesh->flags;
	params.polyCount = pPMesh->npolys;
	params.nvp = pPMesh->nvp;
	params.detailMeshes = pDMesh->meshes;
	params.detailVerts = pDMesh->verts;
	params.detailVertsCount = pDMesh->nverts;
	params.detailTris = pDMesh->tris;
	params.detailTriCount = pDMesh->ntris;
	params.walkableHeight = config.agentHeight;
	params.walkableRadius = config.agentRadius;
	params.walkableClimb = config.agentMaxClimb;
	rcVcopy(params.bmin, pPMesh->bmin);
	rcVcopy(params.bmax, pPMesh->bmax);
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;

	unsigned char* pNavData = NULL;
	int navDataSize = 0;
	if (!dtCreateNavMeshData(&params, &pNavData, &navDataSize) || !pNavData || navDataSize <= 0)
	{
		rcFreePolyMesh(pPMesh);
		rcFreePolyMeshDetail(pDMesh);
		return false;
	}

	rcFreePolyMesh(pPMesh);
	rcFreePolyMeshDetail(pDMesh);

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

	// Attach first so SetNavHints can snap hints onto polys (SetNavMesh frees pNavMesh itself on failure)
	if (!g_EZ2NavMeshManager.SetNavMesh(pNavMesh, EZ2_NAV_PROFILE_FULL))
		return false;

	// Always overwrite hints so stale ones from a previously loaded node mesh never leak into this file
	std::vector<EZ2_NavHint_t> coverHints;
	const int nCover = BakeFullMeshCoverHints(pNavMesh, coverHints);
	g_EZ2NavMeshManager.SetNavHints(coverHints);

	int boundHints = 0;
	const std::vector<EZ2_NavHint_t>& snapped = g_EZ2NavMeshManager.GetNavHints();
	for (size_t i = 0; i < snapped.size(); ++i)
	{
		if (snapped[i].polyRef != 0)
			++boundHints;
	}

	char szModPath[MAX_PATH];
	Q_snprintf(szModPath, sizeof(szModPath), "maps/%s_full.nav", pszMapName);

	const bool bSaved = g_EZ2NavMeshManager.SaveSoloNavMesh(szModPath, pNavMesh, EZ2_NAV_PROFILE_FULL);
	if (bSaved)
	{
		Msg("[EZ2 NavMesh] Saved full solo navmesh to '%s' (%d bytes, %d cover hints baked, %d bound to polys)\n",
			szModPath, navDataSize, nCover, boundHints);
	}
	else
	{
		Warning("[EZ2 NavMesh] Failed saving '%s'\n", szModPath);
	}

	if (!bAttachImmediately)
		g_EZ2NavMeshManager.UnloadMap();

	return bSaved;
}