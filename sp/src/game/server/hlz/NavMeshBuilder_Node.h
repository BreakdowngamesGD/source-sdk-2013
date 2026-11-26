#ifndef EZ2_NAVMESH_BUILDER_NODE_H
#define EZ2_NAVMESH_BUILDER_NODE_H

#if defined( _WIN32 )
#pragma once
#endif


bool BuildAndSaveNodeNavMeshForCurrentMap();


struct NodeNavBuildConfig_t
{
	// Recast / agent settings (Source units)
	float agentRadius;        // hull half-width; the mesh is eroded by this
	float agentHeight;
	float agentMaxClimb;
	float agentMaxSlopeDeg;
	float cellSize;
	float cellHeight;

	// Nodegraph settings
	float corridorHalfWidth;  // walkable half-width left around node links AFTER erosion
	int   hull;               // Hull_t used to filter links (HULL_HUMAN = 0)
	bool  includeOffLinks;    // include links currently flagged OFF / dynamic (tagged with their own area)
	bool  includeJumpLinks;   // convert jump links to Detour off-mesh connections

	NodeNavBuildConfig_t()
		: agentRadius( 16.0f )
		, agentHeight( 72.0f )
		, agentMaxClimb( 18.0f )
		, agentMaxSlopeDeg( 60.0f )
		, cellSize( 8.0f )
		, cellHeight( 4.0f )
		, corridorHalfWidth( 24.0f )
		, hull( 0 )
		, includeOffLinks( true )
		, includeJumpLinks( true )
	{
	}
};

// Poly area / flag ids written by the node builder (for use in dtQueryFilter later)
enum
{
	EZ2_NAV_AREA_GROUND  = 0,
	EZ2_NAV_AREA_DYNLINK = 1,   // link that was OFF / dynamic when the mesh was built (doors, breakables, etc.)
	EZ2_NAV_AREA_JUMP    = 2,   // off-mesh jump connection

	EZ2_NAV_FLAG_WALK    = 0x01,
	EZ2_NAV_FLAG_DYNLINK = 0x02,
	EZ2_NAV_FLAG_JUMP    = 0x04
};

// Builds a solo navmesh from the currently loaded nodegraph (g_pBigAINet),
// saves maps/<map>_node.nav, and optionally attaches it to g_EZ2NavMeshManager.
bool BuildAndSaveNodeSoloNavMesh( const char* pszMapName, const NodeNavBuildConfig_t& config, bool bAttachImmediately );

#endif // EZ2_NAVMESH_BUILDER_NODE_H
