#ifndef EZ2_NAVMESH_MANAGER_H
#define EZ2_NAVMESH_MANAGER_H

#if defined( _WIN32 )
#pragma once
#endif
#include "cbase.h"          // must be first
#include "mathlib/vector.h"
#include "ehandle.h"
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtQueryFilter;
class CBaseEntity;

enum EZ2_NavMode_t
{
	EZ2_NAV_NODEGRAPH = 0,
	EZ2_NAV_MESH_FULL = 1,
	EZ2_NAV_MESH_NODEGRAPH = 2
};

enum EZ2_NavProfile_t
{
	EZ2_NAV_PROFILE_FULL = 0,
	EZ2_NAV_PROFILE_NODEGRAPH = 1
};

// Translated navmesh hint categories (mapped from Source Hint_e)
enum EZ2_NavHintType_t
{
	EZ2_NAVHINT_NONE = 0,
	EZ2_NAVHINT_COVER_CROUCH,     // HINT_TACTICAL_COVER_LOW / crouch cover
	EZ2_NAVHINT_COVER_STAND,      // HINT_TACTICAL_COVER_MED / HIGH / wall cover
	EZ2_NAVHINT_PINCH_OR_FLANK,   // HINT_TACTICAL_PINCH / flank / ambush
	EZ2_NAVHINT_VANTAGE_SNIPER,   // HINT_WORLD_WINDOW / sniper / high ground
	EZ2_NAVHINT_ENTRANCE_EXIT,    // HINT_WORLD_EXIT / door / chokepoint
	EZ2_NAVHINT_PATROL_OR_IDLE,   // Interesting look / patrol / guard point
	EZ2_NAVHINT_JUMP_OR_CLIMB,    // Jump / climb / strider / antlion burrow / special traversal
	EZ2_NAVHINT_CUSTOM            // Fallback preserving original Source Hint_e in sourceHintType
};

#pragma pack(push, 1)
struct EZ2_SerializedNavHint_t
{
	float x, y, z;
	float yaw;
	float fovCos;         // Cosine of half-FOV (or 1.0f if 360)
	float radius;
	float weight;
	int navHintType;      // EZ2_NavHintType_t
	int sourceHintType;   // Original Source Hint_e
	int sourceWCNodeID;   // Original Hammer/WC ID for debugging
	unsigned int polyRef; // Snapped dtPolyRef at bake time (re-snapped on load if needed)
};
#pragma pack(pop)

struct EZ2_NavHint_t
{
	Vector vecPos;
	float flYaw;
	float flFovCos;
	float flRadius;
	float flWeight;
	EZ2_NavHintType_t navHintType;
	int sourceHintType;
	int sourceWCNodeID;
	unsigned int polyRef;

	// Runtime claim/cooldown state (mirrors CAI_Hint locking without needing the entity)
	EHANDLE hClaimedBy;
	float flNextAvailableTime;
	bool bDisabled;          // runtime: mirrored from the live CAI_Hint (see navmesh_dynamic.cpp)
	Vector vecLiveOrigin;    // runtime: last seen CAI_Hint origin (vec3_invalid = not seen yet)

	EZ2_NavHint_t()
		: vecPos(vec3_origin)
		, flYaw(0.0f)
		, flFovCos(1.0f)
		, flRadius(0.0f)
		, flWeight(1.0f)
		, navHintType(EZ2_NAVHINT_NONE)
		, sourceHintType(0)
		, sourceWCNodeID(-1)
		, polyRef(0)
		, flNextAvailableTime(0.0f)
		, bDisabled(false)
		, vecLiveOrigin(vec3_invalid)
	{
	}
};

// Userid flag on off-mesh connections that are ladders (low bits = index into the climb table)
static const unsigned int EZ2_NAV_CLIMB_USERID_FLAG = 0x40000000u;

struct EZ2_NavClimbPoint_t
{
	Vector pos;
	float  yaw;
	EZ2_NavClimbPoint_t() : pos(0, 0, 0), yaw(0.0f) {}
};

struct EZ2_NavClimb_t
{
	Vector base;   // ground position at the bottom (off-mesh endpoint)
	Vector top;    // ground position at the top (off-mesh endpoint)
	std::vector<EZ2_NavClimbPoint_t> points;   // climb nodes, bottom -> top
	EZ2_NavClimb_t() : base(0, 0, 0), top(0, 0, 0) {}
};

class CEZ2_NavMeshManager
{
public:
	CEZ2_NavMeshManager();
	~CEZ2_NavMeshManager();

	void Init();
	void Shutdown();
	void UnloadMap();

	bool SetNavMesh(dtNavMesh* pNavMesh, EZ2_NavProfile_t profile, int maxSearchNodes = 2048);
	bool LoadForMap(const char* pszMapName, EZ2_NavProfile_t profile);
	bool SaveSoloNavMesh(const char* pszFilePath, const dtNavMesh* pNavMesh, EZ2_NavProfile_t profile);

	bool IsReady() const { return (m_pNavMesh != NULL && m_pNavQuery != NULL); }
	dtNavMesh* GetNavMesh() const { return m_pNavMesh; }
	dtNavMeshQuery* GetNavMeshQuery() const { return m_pNavQuery; }
	EZ2_NavProfile_t GetLoadedProfile() const { return m_LoadedProfile; }
	const char* GetLoadedMapName() const { return m_szLoadedMap; }

	EZ2_NavMode_t GetActiveModeForMap(const char* pszMapName = NULL) const;
	bool ShouldUseNavMesh(const char* pszMapName = NULL) const;

	bool FindStraightPathSimple(const Vector& vecStart, const Vector& vecEnd, Vector& vecOutNextWaypoint, const dtQueryFilter* pFilter = NULL);

	// Navmesh Hint API
	void SetNavHints(const std::vector<EZ2_NavHint_t>& hints);
	const std::vector<EZ2_NavHint_t>& GetNavHints() const { return m_NavHints; }
	std::vector<EZ2_NavHint_t>& AccessNavHints() { return m_NavHints; }
	int GetNavHintCount() const { return static_cast<int>(m_NavHints.size()); }

	// Ladder / climb table (indexed by off-mesh userId & ~EZ2_NAV_CLIMB_USERID_FLAG)
	void SetNavClimbs(const std::vector<EZ2_NavClimb_t>& climbs) { m_NavClimbs = climbs; }
	const std::vector<EZ2_NavClimb_t>& GetNavClimbs() const { return m_NavClimbs; }
	const EZ2_NavClimb_t* FindNavClimbNear(const Vector& pos, float radius) const;

	EZ2_NavHint_t* FindBestNavHint(
		CBaseEntity* pClaimant,
		const Vector& vecOrigin,
		EZ2_NavHintType_t desiredType,
		float flMaxDist,
		const Vector* pThreatPos = NULL,
		bool bCanJump = false
	);

	bool ClaimNavHint(EZ2_NavHint_t* pHint, CBaseEntity* pClaimant);
	void ReleaseNavHint(EZ2_NavHint_t* pHint, float flCooldown = 0.0f);

	// Re-snaps one hint's polyRef to the mesh (0 if no poly is near it)
	void SnapHintToMesh(EZ2_NavHint_t & h);

private:
	bool LoadSoloNavMesh(const char* pszFilePath, EZ2_NavProfile_t expectedProfile);
	void FreeQueryOnly();
	void FreeNavMeshOnly();
	void ResnapLoadedHintsToMesh();

	std::vector<EZ2_NavClimb_t> m_NavClimbs;

	dtNavMesh* m_pNavMesh;
	dtNavMeshQuery* m_pNavQuery;
	EZ2_NavProfile_t m_LoadedProfile;
	char m_szLoadedMap[128];
	std::vector<EZ2_NavHint_t> m_NavHints;
};

extern CEZ2_NavMeshManager g_EZ2NavMeshManager;

#endif // EZ2_NAVMESH_MANAGER_H