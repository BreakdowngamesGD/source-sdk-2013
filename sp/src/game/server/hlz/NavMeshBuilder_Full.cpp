#include "cbase.h"
#include "NavMeshBuilder_Full.h"
#include "NavMeshManager.h"

#include <string.h>
#include <float.h>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "NavMeshBuilder_Node.h"   // EZ2_NAV_AREA_JUMP, EZ2_NAV_FLAG_*

// Source filesystem & console output helpers
#include "filesystem.h"
#include "tier0/dbg.h"
#include "tier1/strtools.h"
#include "tier1/convar.h"

extern IFileSystem* filesystem;

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Defined in navmesh_coverbake.cpp
int BakeFullMeshCoverHints(const dtNavMesh* pNavMesh, std::vector<EZ2_NavHint_t>& outHints);
// Defined in navmesh_jumpbake.cpp
int BakeFullMeshJumpLinks(const dtNavMesh* pNavMesh, std::vector<FullNavJumpLink_t>& out);

namespace
{
	// Generated links are cached so runtime carve rebuilds don't re-run the bake
	static std::vector<FullNavJumpLink_t> s_JumpCache;
	static char s_JumpCacheMap[128] = "";

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

	static void FreeMeshBuffers(float*& pVerts, int*& pTris, unsigned char*& pTriAreas)
	{
		delete[] pVerts;
		delete[] pTris;
		delete[] pTriAreas;
		pVerts = NULL;
		pTris = NULL;
		pTriAreas = NULL;
	}

	static bool TriOverlapsXZBox(
		const float* r0, const float* r1, const float* r2,
		float minX, float maxX, float minZ, float maxZ
	)
	{
		const float triMinX = MIN(r0[0], MIN(r1[0], r2[0]));
		const float triMaxX = MAX(r0[0], MAX(r1[0], r2[0]));
		if (triMaxX < minX || triMinX > maxX)
			return false;

		const float triMinZ = MIN(r0[2], MIN(r1[2], r2[2]));
		const float triMaxZ = MAX(r0[2], MAX(r1[2], r2[2]));
		if (triMaxZ < minZ || triMinZ > maxZ)
			return false;

		return true;
	}
}

bool BuildAndSaveFullSoloNavMesh(
	const char* pszMapName,
	const std::vector<FullNavTriangle_t>& triangles,
	const FullNavBuildConfig_t& config,
	bool bAttachImmediately,
	const FullNavExtras_t* pExtras,
	bool bSaveToDisk
)
{
	if (!pszMapName || !pszMapName[0] || triangles.empty())
		return false;

	const int totalTriCount = static_cast<int>(triangles.size());
	const int totalVertCount = totalTriCount * 3;

	float* pAllVerts = new float[totalVertCount * 3];
	int* pAllTris = new int[totalTriCount * 3];
	if (!pAllVerts || !pAllTris)
	{
		delete[] pAllVerts;
		delete[] pAllTris;
		return false;
	}

	float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

	for (int i = 0; i < totalTriCount; ++i)
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
			pAllVerts[vIdx + 0] = pts[k][0];
			pAllVerts[vIdx + 1] = pts[k][1];
			pAllVerts[vIdx + 2] = pts[k][2];

			for (int c = 0; c < 3; ++c)
			{
				if (pts[k][c] < bmin[c]) bmin[c] = pts[k][c];
				if (pts[k][c] > bmax[c]) bmax[c] = pts[k][c];
			}
		}

		pAllTris[i * 3 + 0] = i * 3 + 0;
		pAllTris[i * 3 + 1] = i * 3 + 1;
		pAllTris[i * 3 + 2] = i * 3 + 2;
	}

	// Pad bounds slightly around agent radius/height
	bmin[0] -= config.agentRadius;
	bmax[0] += config.agentRadius;
	bmin[2] -= config.agentRadius;
	bmax[2] += config.agentRadius;
	bmin[1] -= config.agentHeight;
	bmax[1] += config.agentHeight;

	const float spanX = bmax[0] - bmin[0];
	const float spanZ = bmax[2] - bmin[2];
	const float FULL_NAV_TILE_SIZE = MAX(spanX, spanZ) + 1.0f;

	const int gw = 1;
	const int gh = 1;
	const int maxAllowedTiles = 1;

	dtNavMesh* pNavMesh = dtAllocNavMesh();
	if (!pNavMesh)
	{
		delete[] pAllVerts;
		delete[] pAllTris;
		return false;
	}

	dtNavMeshParams meshParams;
	memset(&meshParams, 0, sizeof(meshParams));
	rcVcopy(meshParams.orig, bmin);
	meshParams.tileWidth = FULL_NAV_TILE_SIZE;
	meshParams.tileHeight = FULL_NAV_TILE_SIZE;
	meshParams.maxTiles = maxAllowedTiles;
	meshParams.maxPolys = 1 << 20;

	const dtStatus st = pNavMesh->init(&meshParams);
	if (dtStatusFailed(st))
	{
		Warning("[EZ2 NavMesh] dtNavMesh::init failed (status 0x%x, tiles=%d)\n", (unsigned)st, maxAllowedTiles);
		dtFreeNavMesh(pNavMesh);
		delete[] pAllVerts;
		delete[] pAllTris;
		return false;
	}

	CRecastBuildContext ctx;
	const int borderSize = MAX(1, static_cast<int>(ceilf(config.agentRadius / config.cellSize))) + 3;
	const float padXZ = borderSize * config.cellSize;
	int totalBytes = 0;
	int addedTiles = 0;

	std::vector<int> tileTris;
	tileTris.reserve(totalTriCount / 4 + 64);

	for (int ty = 0; ty < gh && addedTiles < maxAllowedTiles; ++ty)
	{
		for (int tx = 0; tx < gw && addedTiles < maxAllowedTiles; ++tx)
		{
			float tbmin[3], tbmax[3];
			tbmin[0] = bmin[0] + tx * FULL_NAV_TILE_SIZE;
			tbmin[1] = bmin[1];
			tbmin[2] = bmin[2] + ty * FULL_NAV_TILE_SIZE;

			tbmax[0] = tbmin[0] + FULL_NAV_TILE_SIZE;
			tbmax[1] = bmax[1];
			tbmax[2] = tbmin[2] + FULL_NAV_TILE_SIZE;

			const float qMinX = tbmin[0] - padXZ;
			const float qMaxX = tbmax[0] + padXZ;
			const float qMinZ = tbmin[2] - padXZ;
			const float qMaxZ = tbmax[2] + padXZ;

			tileTris.clear();
			for (int i = 0; i < totalTriCount; ++i)
			{
				const float* r0 = &pAllVerts[pAllTris[i * 3 + 0] * 3];
				const float* r1 = &pAllVerts[pAllTris[i * 3 + 1] * 3];
				const float* r2 = &pAllVerts[pAllTris[i * 3 + 2] * 3];
				if (TriOverlapsXZBox(r0, r1, r2, qMinX, qMaxX, qMinZ, qMaxZ))
					tileTris.push_back(i);
			}

			if (tileTris.empty())
				continue;

			const int tTriCount = static_cast<int>(tileTris.size());
			const int tVertCount = tTriCount * 3;
			float* pTVerts = new float[tVertCount * 3];
			int* pTTris = new int[tTriCount * 3];
			unsigned char* pTTriAreas = new unsigned char[tTriCount];

			if (!pTVerts || !pTTris || !pTTriAreas)
			{
				FreeMeshBuffers(pTVerts, pTTris, pTTriAreas);
				continue;
			}

			for (int i = 0; i < tTriCount; ++i)
			{
				const int srcTri = tileTris[i];
				for (int k = 0; k < 3; ++k)
				{
					const int srcV = pAllTris[srcTri * 3 + k] * 3;
					const int dstV = (i * 3 + k) * 3;
					pTVerts[dstV + 0] = pAllVerts[srcV + 0];
					pTVerts[dstV + 1] = pAllVerts[srcV + 1];
					pTVerts[dstV + 2] = pAllVerts[srcV + 2];
					pTTris[i * 3 + k] = i * 3 + k;
				}
				pTTriAreas[i] = 0;
			}

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
			cfg.borderSize = borderSize;

			cfg.bmin[0] = tbmin[0] - padXZ;
			cfg.bmin[1] = tbmin[1];
			cfg.bmin[2] = tbmin[2] - padXZ;
			cfg.bmax[0] = tbmax[0] + padXZ;
			cfg.bmax[1] = tbmax[1];
			cfg.bmax[2] = tbmax[2] + padXZ;

			rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
			if (cfg.width <= 0 || cfg.height <= 0)
			{
				FreeMeshBuffers(pTVerts, pTTris, pTTriAreas);
				continue;
			}

			rcHeightfield* pSolid = rcAllocHeightfield();
			if (!pSolid || !rcCreateHeightfield(&ctx, *pSolid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
			{
				rcFreeHeightField(pSolid);
				FreeMeshBuffers(pTVerts, pTTris, pTTriAreas);
				continue;
			}

			rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, pTVerts, tVertCount, pTTris, tTriCount, pTTriAreas);
			if (!rcRasterizeTriangles(&ctx, pTVerts, tVertCount, pTTris, pTTriAreas, tTriCount, *pSolid, cfg.walkableClimb))
			{
				rcFreeHeightField(pSolid);
				FreeMeshBuffers(pTVerts, pTTris, pTTriAreas);
				continue;
			}

			delete[] pTVerts;
			delete[] pTTris;
			pTVerts = NULL;
			pTTris = NULL;

			rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *pSolid);
			rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *pSolid);
			rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *pSolid);

			rcCompactHeightfield* pCHF = rcAllocCompactHeightfield();
			if (!pCHF || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *pSolid, *pCHF))
			{
				rcFreeHeightField(pSolid);
				rcFreeCompactHeightfield(pCHF);
				delete[] pTTriAreas;
				continue;
			}

			rcFreeHeightField(pSolid);
			pSolid = NULL;

			if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *pCHF) ||
				!rcBuildDistanceField(&ctx, *pCHF) ||
				!rcBuildRegions(&ctx, *pCHF, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
			{
				rcFreeCompactHeightfield(pCHF);
				delete[] pTTriAreas;
				continue;
			}

			delete[] pTTriAreas;
			pTTriAreas = NULL;

			rcContourSet* pCSet = rcAllocContourSet();
			if (!pCSet || !rcBuildContours(&ctx, *pCHF, cfg.maxSimplificationError, cfg.maxEdgeLen, *pCSet))
			{
				rcFreeCompactHeightfield(pCHF);
				rcFreeContourSet(pCSet);
				continue;
			}

			rcPolyMesh* pPMesh = rcAllocPolyMesh();
			if (!pPMesh || !rcBuildPolyMesh(&ctx, *pCSet, cfg.maxVertsPerPoly, *pPMesh))
			{
				rcFreeCompactHeightfield(pCHF);
				rcFreeContourSet(pCSet);
				rcFreePolyMesh(pPMesh);
				continue;
			}

			rcPolyMeshDetail* pDMesh = rcAllocPolyMeshDetail();
			if (!pDMesh || !rcBuildPolyMeshDetail(&ctx, *pPMesh, *pCHF, cfg.detailSampleDist, cfg.detailSampleMaxError, *pDMesh))
			{
				rcFreeCompactHeightfield(pCHF);
				rcFreeContourSet(pCSet);
				rcFreePolyMesh(pPMesh);
				rcFreePolyMeshDetail(pDMesh);
				continue;
			}

			DevMsg("[EZ2 NavMesh] polymesh: %d polys, %d verts\n", pPMesh->npolys, pPMesh->nverts);

			rcFreeCompactHeightfield(pCHF);
			rcFreeContourSet(pCSet);

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
			rcVcopy(params.bmin, tbmin);
			rcVcopy(params.bmax, tbmax);
			params.cs = cfg.cs;
			params.ch = cfg.ch;
			params.buildBvTree = true;

			unsigned char* pNavData = NULL;
			int navDataSize = 0;
			if (dtCreateNavMeshData(&params, &pNavData, &navDataSize) && pNavData && navDataSize > 0)
			{
				dtMeshHeader* pHeader = reinterpret_cast<dtMeshHeader*>(pNavData);
				pHeader->x = tx;
				pHeader->y = ty;
				pHeader->layer = 0;

				dtTileRef tileRef = 0;
				if (dtStatusSucceed(pNavMesh->addTile(pNavData, navDataSize, DT_TILE_FREE_DATA, 0, &tileRef)))
				{
					totalBytes += navDataSize;
					++addedTiles;

					// Pass 2: generate jump links from the mesh just built, then rebuild the tile with them
					std::vector<FullNavJumpLink_t> jumps;
					if (!bSaveToDisk && !s_JumpCache.empty() && Q_stricmp(s_JumpCacheMap, pszMapName) == 0)
					{
						jumps = s_JumpCache;
					}
					else
					{
						BakeFullMeshJumpLinks(pNavMesh, jumps);
						if (pExtras)   // optional nodegraph jumps (ez2_nav_jump_use_nodegraph)
							jumps.insert(jumps.end(), pExtras->jumps.begin(), pExtras->jumps.end());
						s_JumpCache = jumps;
						Q_strncpy(s_JumpCacheMap, pszMapName, sizeof(s_JumpCacheMap));
					}

					const std::vector<EZ2_NavClimb_t> noClimbs;
					const std::vector<EZ2_NavClimb_t>& climbs = pExtras ? pExtras->climbs : noClimbs;

					if (!jumps.empty() || !climbs.empty())
					{
						const int nJump = (int)jumps.size();
						const int nOff = nJump + (int)climbs.size();
						std::vector<float>          offVerts(nOff * 6);
						std::vector<float>          offRad(nOff);
						std::vector<unsigned char>  offDir(nOff), offAreas(nOff);
						std::vector<unsigned short> offFlags(nOff);
						std::vector<unsigned int>   offIds(nOff);

						const float jumpRad = pExtras ? pExtras->jumpRadius : 48.0f;
						for (int j = 0; j < nJump; ++j)
						{
							SourceVecToRecast(jumps[j].a, &offVerts[j * 6 + 0]);
							SourceVecToRecast(jumps[j].b, &offVerts[j * 6 + 3]);
							offRad[j] = jumpRad;
							offDir[j] = jumps[j].bBidir ? DT_OFFMESH_CON_BIDIR : 0;
							offAreas[j] = EZ2_NAV_AREA_JUMP;
							offFlags[j] = EZ2_NAV_FLAG_WALK | EZ2_NAV_FLAG_JUMP;
							offIds[j] = (unsigned int)j;
						}
						for (int c = 0; c < (int)climbs.size(); ++c)
						{
							const int idx = nJump + c;
							SourceVecToRecast(climbs[c].base, &offVerts[idx * 6 + 0]);
							SourceVecToRecast(climbs[c].top, &offVerts[idx * 6 + 3]);
							offRad[idx] = jumpRad;
							offDir[idx] = DT_OFFMESH_CON_BIDIR;
							offAreas[idx] = EZ2_NAV_AREA_CLIMB;
							offFlags[idx] = EZ2_NAV_FLAG_WALK | EZ2_NAV_FLAG_CLIMB;
							offIds[idx] = EZ2_NAV_CLIMB_USERID_FLAG | (unsigned int)c;
						}

						params.offMeshConVerts = &offVerts[0];
						params.offMeshConRad = &offRad[0];
						params.offMeshConDir = &offDir[0];
						params.offMeshConAreas = &offAreas[0];
						params.offMeshConFlags = &offFlags[0];
						params.offMeshConUserID = &offIds[0];
						params.offMeshConCount = nOff;

						unsigned char* pData2 = NULL;
						int size2 = 0;
						if (dtCreateNavMeshData(&params, &pData2, &size2) && pData2 && size2 > 0)
						{
							dtMeshHeader* pH2 = reinterpret_cast<dtMeshHeader*>(pData2);
							pH2->x = tx;
							pH2->y = ty;
							pH2->layer = 0;

							if (dtStatusSucceed(pNavMesh->removeTile(tileRef, NULL, NULL)))
							{
								totalBytes -= navDataSize;
								if (dtStatusSucceed(pNavMesh->addTile(pData2, size2, DT_TILE_FREE_DATA, 0, NULL)))
									totalBytes += size2;
								else
								{
									dtFree(pData2);
									--addedTiles;
								}
							}
							else
								dtFree(pData2);
						}
					}
				}
				else
				{
					dtFree(pNavData);
				}
			}

			rcFreePolyMesh(pPMesh);
			rcFreePolyMeshDetail(pDMesh);
		}
	}

	delete[] pAllVerts;
	delete[] pAllTris;

	if (addedTiles <= 0)
	{
		Warning("[EZ2 NavMesh] No tile produced (tris=%d, bounds %.0f x %.0f). Run 'developer 1' for [Recast] errors.\n",
			totalTriCount, spanX, spanZ);
		dtFreeNavMesh(pNavMesh);
		return false;
	}

	if (!g_EZ2NavMeshManager.SetNavMesh(pNavMesh, EZ2_NAV_PROFILE_FULL))
		return false;

	std::vector<EZ2_NavHint_t> coverHints;
	const int nCover = BakeFullMeshCoverHints(pNavMesh, coverHints);
	if (pExtras && !pExtras->entityHints.empty())
	{
		coverHints.insert(coverHints.end(), pExtras->entityHints.begin(), pExtras->entityHints.end());
	}
	g_EZ2NavMeshManager.SetNavHints(coverHints);

	static const std::vector<EZ2_NavClimb_t> s_NoClimbs;
	g_EZ2NavMeshManager.SetNavClimbs(pExtras ? pExtras->climbs : s_NoClimbs);

	int boundHints = 0;
	const std::vector<EZ2_NavHint_t>& snapped = g_EZ2NavMeshManager.GetNavHints();
	for (size_t i = 0; i < snapped.size(); ++i)
	{
		if (snapped[i].polyRef != 0)
			++boundHints;
	}

	// Runtime carves rebuild in memory only. The saved file stays the clean mesh.
	if (!bSaveToDisk)
	{
		DevMsg("[EZ2 NavMesh] Carved navmesh rebuilt (%d tiles, %d cover hints)\n", addedTiles, nCover);
		if (!bAttachImmediately)
			g_EZ2NavMeshManager.UnloadMap();
		return true;
	}

	char szModPath[MAX_PATH];
	Q_snprintf(szModPath, sizeof(szModPath), "maps/%s_full.nav", pszMapName);

	const bool bSaved = g_EZ2NavMeshManager.SaveSoloNavMesh(szModPath, pNavMesh, EZ2_NAV_PROFILE_FULL);
	if (bSaved)
	{
		Msg("[EZ2 NavMesh] Saved full solo navmesh to '%s' (%d tiles, %d bytes, %d cover hints baked, %d bound to polys)\n",
			szModPath, addedTiles, totalBytes, nCover, boundHints);
	}
	else
	{
		Warning("[EZ2 NavMesh] Failed saving '%s'\n", szModPath);
	}

	if (!bAttachImmediately)
		g_EZ2NavMeshManager.UnloadMap();

	return bSaved;
}