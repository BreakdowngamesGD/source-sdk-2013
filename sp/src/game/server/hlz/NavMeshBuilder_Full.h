#ifndef EZ2_NAVMESH_BUILDER_FULL_H
#define EZ2_NAVMESH_BUILDER_FULL_H

#if defined( _WIN32 )
#pragma once
#endif

#include <vector>
#include "mathlib/vector.h"
#include "NavMeshManager.h"   // EZ2_NavHint_t
class dtNavMesh;

struct FullNavTriangle_t
{
	Vector v0;
	Vector v1;
	Vector v2;
};

struct FullNavBuildConfig_t
{
	float cellSize;          // Recast cs (e.g. 12.0f Source units)
	float cellHeight;        // Recast ch (e.g. 6.0f Source units)
	float agentHeight;       // e.g. 72.0f (standard HL2 humanoid height)
	float agentRadius;       // e.g. 18.0f (standard HL2 humanoid hull radius ~16-18 units)
	float agentMaxClimb;     // e.g. 18.0f (step height)
	float agentMaxSlopeDeg;  // e.g. 45.0f

	FullNavBuildConfig_t()
		: cellSize(12.0f)
		, cellHeight(6.0f)
		, agentHeight(72.0f)
		, agentRadius(18.0f)
		, agentMaxClimb(18.0f)
		, agentMaxSlopeDeg(45.0f)
	{
	}
};

// Builds a solo dtNavMesh from Source-space triangles and writes maps/<mapName>_full.nav
bool BuildAndSaveFullSoloNavMesh(
	const char* pszMapName,
	const std::vector<FullNavTriangle_t>& triangles,
	const FullNavBuildConfig_t& config,
	bool bAttachImmediately = true
);

// Bake-time cover sampler (navmesh_coverbake.cpp). Returns number of hints generated.
int BakeFullMeshCoverHints(const dtNavMesh* pNavMesh, std::vector<EZ2_NavHint_t>& outHints);

#endif // EZ2_NAVMESH_BUILDER_FULL_H