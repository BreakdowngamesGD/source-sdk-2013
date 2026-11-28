#ifndef EZ2_NAVMESH_BUILDER_NODE_H
#define EZ2_NAVMESH_BUILDER_NODE_H
#if defined( _WIN32 )
#pragma once
#endif

#include <vector>
#include "mathlib/vector.h"
#include "NavMeshManager.h"      // EZ2_NavHint_t
#include "NavMeshBuilder_Full.h"  // FullNavTriangle_t, FullNavJumpLink_t, FullNavObstacle_t, FullNavExtras_t

struct NodeNavBuildConfig_t;

// Shared with the full-mesh builder: every CAI_Hint in the map as a translated nav hint
// bSkipTactical drops cover/pinch hints so the full builder generates those from geometry
int EZ2_NavAppendEntityHints(std::vector<EZ2_NavHint_t>&out, int hull, bool bSkipTactical = false);

// Walks the loaded nodegraph and collects ladders (ground -> climb nodes -> ground) for the given hull
int EZ2_NavCollectNodegraphClimbs(int hull, std::vector<EZ2_NavClimb_t>& out);

struct NodeNavBuildConfig_t
{
	float agentRadius;
	float agentHeight;
	float agentMaxClimb;
	float agentMaxSlopeDeg;
	float cellSize;
	float cellHeight;
	float corridorHalfWidth;
	int hull;
	bool includeOffLinks;
	bool includeClimbLinks = true;
	bool includeJumpLinks;
};

bool BuildAndSaveNodeNavMeshForCurrentMap();

// Poly area / flag ids written by the node builder (for use in dtQueryFilter later)
enum
{
	EZ2_NAV_AREA_GROUND = 0,
	EZ2_NAV_AREA_DYNLINK = 1,
	EZ2_NAV_AREA_JUMP = 2,
	EZ2_NAV_AREA_CLIMB = 3,   // off-mesh ladder connection

	EZ2_NAV_FLAG_WALK = 0x01,
	EZ2_NAV_FLAG_DYNLINK = 0x02,
	EZ2_NAV_FLAG_JUMP = 0x04,
	EZ2_NAV_FLAG_CLIMB = 0x08
};

// Builds a solo navmesh from the currently loaded nodegraph (g_pBigAINet),
// saves maps/<map>_node.nav, and optionally attaches it to g_EZ2NavMeshManager.
bool BuildAndSaveNodeSoloNavMesh(
	const char* pszMapName,
	const NodeNavBuildConfig_t& config,
	bool bAttachImmediately
);

#endif // EZ2_NAVMESH_BUILDER_NODE_H