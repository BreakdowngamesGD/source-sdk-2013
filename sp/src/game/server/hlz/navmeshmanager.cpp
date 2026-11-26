#include "cbase.h"          // must be first

#include "NavMeshManager.h"

#include <string.h>
#include <math.h>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"
#include "filesystem.h"
#include "NavMeshDynamic.h"

// memdbgon must be the last include file in a .cpp file
#include "tier0/memdbgon.h"

CEZ2_NavMeshManager g_EZ2NavMeshManager;

// ConVars for quick runtime testing and hybrid switching
static ConVar ez2_nav_mode(
	"ez2_nav_mode",
	"0",
	FCVAR_ARCHIVE,
	"Navigation mode: 0 = CAI_Network nodegraph, 1 = Full-geometry navmesh, 2 = Nodegraph-derived navmesh"
);

static ConVar ez2_nav_fallback_to_nodegraph(
	"ez2_nav_fallback_to_nodegraph",
	"1",
	FCVAR_ARCHIVE,
	"If 1, fall back to CAI_Network when no .nav file exists or navmesh load fails"
);

namespace
{
	static const unsigned int EZ2_NAV_MAGIC = 0x325A454E; // "NEZ2"
	static const int EZ2_NAV_VERSION = 2;

#pragma pack(push, 1)
	struct SoloNavFileHeader_t
	{
		unsigned int magic;
		int version;
		int profile;      // EZ2_NavProfile_t
		int tileDataSize; // Byte size of the solo tile buffer
		int hintCount;    // Number of EZ2_SerializedNavHint_t records following tile data (v2+)
	};
#pragma pack(pop)

	inline void SourceToDetour(const Vector& in, float out[3])
	{
		out[0] = in.x;
		out[1] = in.z;
		out[2] = -in.y;
	}

	inline void DetourToSource(const float in[3], Vector& out)
	{
		out.x = in[0];
		out.y = -in[2];
		out.z = in[1];
	}
}

CEZ2_NavMeshManager::CEZ2_NavMeshManager()
	: m_pNavMesh(NULL)
	, m_pNavQuery(NULL)
	, m_LoadedProfile(EZ2_NAV_PROFILE_FULL)
{
	m_szLoadedMap[0] = '\0';
}

CEZ2_NavMeshManager::~CEZ2_NavMeshManager()
{
	Shutdown();
}

void CEZ2_NavMeshManager::Init()
{
	// Ready for map loads; query is initialized when a navmesh is loaded
}

void CEZ2_NavMeshManager::Shutdown()
{
	UnloadMap();
}

void CEZ2_NavMeshManager::FreeQueryOnly()
{
	if (m_pNavQuery)
	{
		dtFreeNavMeshQuery(m_pNavQuery);
		m_pNavQuery = NULL;
	}
}

void CEZ2_NavMeshManager::FreeNavMeshOnly()
{
	if (m_pNavMesh)
	{
		dtFreeNavMesh(m_pNavMesh);
		m_pNavMesh = NULL;
	}
}

void CEZ2_NavMeshManager::UnloadMap()
{
	FreeQueryOnly();
	FreeNavMeshOnly();
	m_NavHints.clear();
	m_szLoadedMap[0] = '\0';
}

void CEZ2_NavMeshManager::SetNavHints(const std::vector<EZ2_NavHint_t>& hints)
{
	m_NavHints = hints;
	ResnapLoadedHintsToMesh();
}

void CEZ2_NavMeshManager::ResnapLoadedHintsToMesh()
{
	if (!m_pNavQuery)
		return;

	dtQueryFilter filter;
	float halfExtents[3] = { 32.0f, 64.0f, 32.0f };

	for (size_t i = 0; i < m_NavHints.size(); ++i)
	{
		float dPos[3], nearest[3];
		SourceToDetour(m_NavHints[i].vecPos, dPos);

		dtPolyRef polyRef = 0;
		if (dtStatusSucceed(m_pNavQuery->findNearestPoly(dPos, halfExtents, &filter, &polyRef, nearest)) && polyRef)
		{
			m_NavHints[i].polyRef = static_cast<unsigned int>(polyRef);
		}
		else
		{
			m_NavHints[i].polyRef = 0;
		}
	}
}

bool CEZ2_NavMeshManager::SetNavMesh(dtNavMesh* pNavMesh, EZ2_NavProfile_t profile, int maxSearchNodes)
{
	FreeQueryOnly();
	FreeNavMeshOnly();

	if (!pNavMesh)
		return false;

	dtNavMeshQuery* pQuery = dtAllocNavMeshQuery();
	if (!pQuery)
	{
		dtFreeNavMesh(pNavMesh);
		return false;
	}

	if (dtStatusFailed(pQuery->init(pNavMesh, maxSearchNodes)))
	{
		dtFreeNavMeshQuery(pQuery);
		dtFreeNavMesh(pNavMesh);
		return false;
	}

	m_pNavMesh = pNavMesh;
	m_pNavQuery = pQuery;
	m_LoadedProfile = profile;
	return true;
}

EZ2_NavMode_t CEZ2_NavMeshManager::GetActiveModeForMap(const char* pszMapName) const
{
	// Hook per-map overrides here if needed (e.g. force nodegraph on specific campaign maps)
	NOTE_UNUSED(pszMapName);

	int mode = ez2_nav_mode.GetInt();
	if (mode < EZ2_NAV_NODEGRAPH || mode > EZ2_NAV_MESH_NODEGRAPH)
		return EZ2_NAV_NODEGRAPH;

	return static_cast<EZ2_NavMode_t>(mode);
}

bool CEZ2_NavMeshManager::ShouldUseNavMesh(const char* pszMapName) const
{
	const char* pMap = pszMapName ? pszMapName : m_szLoadedMap;
	EZ2_NavMode_t mode = GetActiveModeForMap(pMap);

	if (mode == EZ2_NAV_NODEGRAPH)
		return false;

	if (!IsReady())
		return false;

	EZ2_NavProfile_t desiredProfile = (mode == EZ2_NAV_MESH_FULL)
		? EZ2_NAV_PROFILE_FULL
		: EZ2_NAV_PROFILE_NODEGRAPH;

	return (m_LoadedProfile == desiredProfile);
}

bool CEZ2_NavMeshManager::LoadForMap(const char* pszMapName, EZ2_NavProfile_t profile)
{
	UnloadMap();

	if (!pszMapName || !pszMapName[0])
		return false;

	Q_strncpy(m_szLoadedMap, pszMapName, sizeof(m_szLoadedMap));

	const char* pszSuffix = (profile == EZ2_NAV_PROFILE_FULL) ? "_full.nav" : "_node.nav";
	char szModPath[MAX_PATH];
	Q_snprintf(szModPath, sizeof(szModPath), "maps/%s%s", pszMapName, pszSuffix);

	if (!LoadSoloNavMesh(szModPath, profile))
	{
		DevMsg("[EZ2 NavMesh] No/invalid navmesh at '%s' (using nodegraph fallback: %s)\n",
			szModPath, ez2_nav_fallback_to_nodegraph.GetBool() ? "yes" : "no");
		return false;
	}

	DevMsg("[EZ2 NavMesh] Loaded '%s' for map '%s' (%d hints)\n", szModPath, pszMapName, GetNavHintCount());
	return true;
}

bool CEZ2_NavMeshManager::SaveSoloNavMesh(const char* pszFilePath, const dtNavMesh* pNavMesh, EZ2_NavProfile_t profile)
{
	if (!pszFilePath || !pNavMesh || !filesystem)
		return false;

	const dtMeshTile* pTile = pNavMesh->getTileAt(0, 0, 0);
	if (!pTile || !pTile->data || pTile->dataSize <= 0)
		return false;

	const dtNavMeshParams* pParams = pNavMesh->getParams();
	if (!pParams)
		return false;

	FileHandle_t fh = filesystem->Open(pszFilePath, "wb", "MOD");
	if (!fh)
		return false;

	int hintCount = static_cast<int>(m_NavHints.size());

	SoloNavFileHeader_t header;
	header.magic = EZ2_NAV_MAGIC;
	header.version = EZ2_NAV_VERSION;
	header.profile = static_cast<int>(profile);
	header.tileDataSize = pTile->dataSize;
	header.hintCount = hintCount;

	bool ok =
		(filesystem->Write(&header, sizeof(header), fh) == sizeof(header)) &&
		(filesystem->Write(pParams, sizeof(dtNavMeshParams), fh) == sizeof(dtNavMeshParams)) &&
		(filesystem->Write(pTile->data, pTile->dataSize, fh) == pTile->dataSize);

	if (ok && hintCount > 0)
	{
		for (int i = 0; i < hintCount; ++i)
		{
			const EZ2_NavHint_t& src = m_NavHints[i];
			EZ2_SerializedNavHint_t diskHint;
			diskHint.x = src.vecPos.x;
			diskHint.y = src.vecPos.y;
			diskHint.z = src.vecPos.z;
			diskHint.yaw = src.flYaw;
			diskHint.fovCos = src.flFovCos;
			diskHint.radius = src.flRadius;
			diskHint.weight = src.flWeight;
			diskHint.navHintType = static_cast<int>(src.navHintType);
			diskHint.sourceHintType = src.sourceHintType;
			diskHint.sourceWCNodeID = src.sourceWCNodeID;
			diskHint.polyRef = src.polyRef;

			if (filesystem->Write(&diskHint, sizeof(diskHint), fh) != sizeof(diskHint))
			{
				ok = false;
				break;
			}
		}
	}

	filesystem->Close(fh);
	return ok;
}

bool CEZ2_NavMeshManager::LoadSoloNavMesh(const char* pszFilePath, EZ2_NavProfile_t expectedProfile)
{
	if (!pszFilePath || !filesystem)
		return false;

	FileHandle_t fh = filesystem->Open(pszFilePath, "rb", "MOD");
	if (!fh)
		return false;

	unsigned int magic = 0;
	int version = 0;
	int profile = 0;
	int tileDataSize = 0;
	int hintCount = 0;

	if (filesystem->Read(&magic, sizeof(magic), fh) != sizeof(magic) ||
		filesystem->Read(&version, sizeof(version), fh) != sizeof(version) ||
		filesystem->Read(&profile, sizeof(profile), fh) != sizeof(profile) ||
		filesystem->Read(&tileDataSize, sizeof(tileDataSize), fh) != sizeof(tileDataSize))
	{
		filesystem->Close(fh);
		return false;
	}

	if (version >= 2)
	{
		if (filesystem->Read(&hintCount, sizeof(hintCount), fh) != sizeof(hintCount) || hintCount < 0)
		{
			filesystem->Close(fh);
			return false;
		}
	}

	dtNavMeshParams params;
	if (filesystem->Read(&params, sizeof(params), fh) != sizeof(params))
	{
		filesystem->Close(fh);
		return false;
	}

	if (magic != EZ2_NAV_MAGIC ||
		(version != 1 && version != 2) ||
		profile != static_cast<int>(expectedProfile) ||
		tileDataSize <= 0)
	{
		filesystem->Close(fh);
		return false;
	}

	// Detour frees buffers added with DT_TILE_FREE_DATA via dtFree(), so allocate with dtAlloc()
	unsigned char* pTileData = static_cast<unsigned char*>(dtAlloc(static_cast<size_t>(tileDataSize), DT_ALLOC_PERM));
	if (!pTileData)
	{
		filesystem->Close(fh);
		return false;
	}

	if (filesystem->Read(pTileData, tileDataSize, fh) != tileDataSize)
	{
		dtFree(pTileData);
		filesystem->Close(fh);
		return false;
	}

	std::vector<EZ2_NavHint_t> loadedHints;
	if (version >= 2 && hintCount > 0)
	{
		loadedHints.reserve(static_cast<size_t>(hintCount));
		for (int i = 0; i < hintCount; ++i)
		{
			EZ2_SerializedNavHint_t diskHint;
			if (filesystem->Read(&diskHint, sizeof(diskHint), fh) != sizeof(diskHint))
				break;

			EZ2_NavHint_t hint;
			hint.vecPos = Vector(diskHint.x, diskHint.y, diskHint.z);
			hint.flYaw = diskHint.yaw;
			hint.flFovCos = diskHint.fovCos;
			hint.flRadius = diskHint.radius;
			hint.flWeight = diskHint.weight;
			hint.navHintType = static_cast<EZ2_NavHintType_t>(diskHint.navHintType);
			hint.sourceHintType = diskHint.sourceHintType;
			hint.sourceWCNodeID = diskHint.sourceWCNodeID;
			hint.polyRef = diskHint.polyRef;
			loadedHints.push_back(hint);
		}
	}

	filesystem->Close(fh);

	dtNavMesh* pNavMesh = dtAllocNavMesh();
	if (!pNavMesh)
	{
		dtFree(pTileData);
		return false;
	}

	if (dtStatusFailed(pNavMesh->init(&params)))
	{
		dtFree(pTileData);
		dtFreeNavMesh(pNavMesh);
		return false;
	}

	if (dtStatusFailed(pNavMesh->addTile(pTileData, tileDataSize, DT_TILE_FREE_DATA, 0, NULL)))
	{
		dtFree(pTileData);
		dtFreeNavMesh(pNavMesh);
		return false;
	}

	if (!SetNavMesh(pNavMesh, expectedProfile))
		return false;

	SetNavHints(loadedHints);
	return true;
}

EZ2_NavHint_t* CEZ2_NavMeshManager::FindBestNavHint(
	CBaseEntity* pClaimant,
	const Vector& vecOrigin,
	EZ2_NavHintType_t desiredType,
	float flMaxDist,
	const Vector* pThreatPos,
	bool bCanJump
)
{
	if (!IsReady() || m_NavHints.empty())
		return NULL;

	EZ2_NavHint_t* pBest = NULL;
	float flBestScore = -1e9f;
	const float flMaxDistSqr = flMaxDist * flMaxDist;

	for (size_t i = 0; i < m_NavHints.size(); ++i)
	{
		EZ2_NavHint_t& hint = m_NavHints[i];

		if (!bCanJump && hint.navHintType == EZ2_NAVHINT_JUMP_OR_CLIMB)
			continue;

		if (desiredType != EZ2_NAVHINT_NONE && hint.navHintType != desiredType)
			continue;

		CBaseEntity* pOwner = hint.hClaimedBy.Get();
		if (pOwner && pOwner != pClaimant)
			continue;

		if (pOwner == NULL && gpGlobals->curtime < hint.flNextAvailableTime)
			continue;

		if (hint.polyRef == 0 || hint.bDisabled)
			continue;

		if (EZ2_NavGetDynStateByRef(hint.polyRef) == EZ2_NAVDYN_BLOCKED)
			continue;

		float flDistSqr = vecOrigin.DistToSqr(hint.vecPos);
		if (flMaxDist > 0.0f && flDistSqr > flMaxDistSqr)
			continue;

		if (pThreatPos && hint.flFovCos < 0.999f)
		{
			Vector vecDir = *pThreatPos - hint.vecPos;
			vecDir.z = 0.0f;
			if (vecDir.LengthSqr() > 1.0f)
			{
				vecDir.NormalizeInPlace();
				Vector vecForward = UTIL_YawToVector(hint.flYaw);
				if (DotProduct(vecForward, vecDir) < hint.flFovCos)
					continue;
			}
		}

		float flDist = sqrtf(flDistSqr);
		float flWeight = (hint.flWeight > 0.01f) ? hint.flWeight : 1.0f;
		float flScore = (1000.0f * flWeight) - flDist;

		if (flScore > flBestScore)
		{
			flBestScore = flScore;
			pBest = &hint;
		}
	}

	return pBest;
}

bool CEZ2_NavMeshManager::ClaimNavHint(EZ2_NavHint_t* pHint, CBaseEntity* pClaimant)
{
	if (!pHint)
		return false;

	CBaseEntity* pOwner = pHint->hClaimedBy.Get();
	if (pOwner && pOwner != pClaimant)
		return false;

	pHint->hClaimedBy = pClaimant;
	return true;
}

void CEZ2_NavMeshManager::ReleaseNavHint(EZ2_NavHint_t* pHint, float flCooldown)
{
	if (!pHint)
		return;

	pHint->hClaimedBy = NULL;
	pHint->flNextAvailableTime = gpGlobals->curtime + flCooldown;
}

bool CEZ2_NavMeshManager::FindStraightPathSimple(const Vector& vecStart, const Vector& vecEnd, Vector& vecOutNextPoint, const dtQueryFilter* pFilter) 
{
	if (!IsReady())
		return false;

	float startPos[3], endPos[3];
	SourceToDetour(vecStart, startPos);
	SourceToDetour(vecEnd, endPos);

	float halfExtents[3] = { 32.0f, 64.0f, 32.0f };
	dtQueryFilter filter;                       // plain: endpoint snapping only
	CEZ2_NavFilter defaultFilter;
	const dtQueryFilter* pPathFilter = pFilter ? pFilter : &defaultFilter;

	dtPolyRef startRef = 0, endRef = 0;
	float nearestStart[3], nearestEnd[3];

	if (dtStatusFailed(m_pNavQuery->findNearestPoly(startPos, halfExtents, &filter, &startRef, nearestStart)) || !startRef)
		return false;

	if (dtStatusFailed(m_pNavQuery->findNearestPoly(endPos, halfExtents, &filter, &endRef, nearestEnd)) || !endRef)
		return false;

	static const int MAX_POLYS = 256;
	dtPolyRef polyPath[MAX_POLYS];
	int polyCount = 0;

	if (dtStatusFailed(m_pNavQuery->findPath(startRef, endRef, nearestStart, nearestEnd, pPathFilter, polyPath, &polyCount, MAX_POLYS)) || polyCount <= 0)
		return false;

	static const int MAX_STRAIGHT = 64;
	float straightPath[MAX_STRAIGHT * 3];
	unsigned char straightFlags[MAX_STRAIGHT];
	dtPolyRef straightPolys[MAX_STRAIGHT];
	int straightCount = 0;

	if (dtStatusFailed(m_pNavQuery->findStraightPath(
		nearestStart, nearestEnd, polyPath, polyCount,
		straightPath, straightFlags, straightPolys, &straightCount, MAX_STRAIGHT)) || straightCount <= 0)
	{
		return false;
	}

	// straightPath[0..2] is the start point; index 1 (if present) is the next steering waypoint
	const float* pTarget = (straightCount > 1) ? &straightPath[3] : &straightPath[0];
	DetourToSource(pTarget, vecOutNextPoint);
	return true;
}

// Automatic level-shutdown cleanup so navmeshes never leak across map changes
static void EZ2_NavLoadCurrentMap()
{
	const int mode = ez2_nav_mode.GetInt();
	if (mode <= EZ2_NAV_NODEGRAPH || !gpGlobals || gpGlobals->mapname == NULL_STRING)
		return;

	g_EZ2NavMeshManager.LoadForMap(STRING(gpGlobals->mapname),
		(mode == EZ2_NAV_MESH_FULL) ? EZ2_NAV_PROFILE_FULL : EZ2_NAV_PROFILE_NODEGRAPH);
}

class CEZ2_NavMeshAutoGameSystem : public CAutoGameSystem
{
public:
	CEZ2_NavMeshAutoGameSystem() : CAutoGameSystem("EZ2_NavMeshAutoGameSystem") {}

	virtual void LevelInitPostEntity() OVERRIDE { EZ2_NavLoadCurrentMap(); }
	virtual void LevelShutdownPostEntity() OVERRIDE { g_EZ2NavMeshManager.UnloadMap(); }
};

static CEZ2_NavMeshAutoGameSystem s_EZ2NavMeshAutoGameSystem;

CON_COMMAND(ez2_nav_reload, "Reloads the navmesh for the current map using ez2_nav_mode")
{
	g_EZ2NavMeshManager.UnloadMap();
	EZ2_NavLoadCurrentMap();
}

CON_COMMAND(ez2_nav_status, "Prints current EZ2 hybrid navigation / navmesh status")
{
	const char* pszMap = (gpGlobals && gpGlobals->mapname != NULL_STRING) ? STRING(gpGlobals->mapname) : "<none>";
	EZ2_NavMode_t mode = g_EZ2NavMeshManager.GetActiveModeForMap(pszMap);

	const char* pszModeName = "nodegraph (CAI_Network)";
	if (mode == EZ2_NAV_MESH_FULL)
		pszModeName = "navmesh_full";
	else if (mode == EZ2_NAV_MESH_NODEGRAPH)
		pszModeName = "navmesh_nodegraph";

	const char* pszProfileName = (g_EZ2NavMeshManager.GetLoadedProfile() == EZ2_NAV_PROFILE_FULL)
		? "full"
		: "nodegraph_derived";

	Msg("[EZ2 NavMesh] Map: %s | Mode: %s | Ready: %s | Loaded Profile: %s | Hints: %d | Fallback: %s\n",
		pszMap,
		pszModeName,
		g_EZ2NavMeshManager.IsReady() ? "yes" : "no",
		g_EZ2NavMeshManager.IsReady() ? pszProfileName : "none",
		g_EZ2NavMeshManager.GetNavHintCount(),
		ez2_nav_fallback_to_nodegraph.GetBool() ? "yes" : "no"
	);
}

CON_COMMAND(ez2_nav_test_path, "Tests Detour path from player to the ground spot you are looking at")
{
	CBasePlayer* pPlayer = UTIL_GetCommandClient();
	if (!pPlayer)
	{
		Warning("[EZ2 NavMesh] No player found for ez2_nav_test_path.\n");
		return;
	}

	if (!g_EZ2NavMeshManager.IsReady())
	{
		Warning("[EZ2 NavMesh] No navmesh is currently loaded/ready.\n");
		return;
	}

	Vector vecEye = pPlayer->EyePosition();
	Vector vecForward;
	pPlayer->EyeVectors(&vecForward);

	Vector vecTraceEnd = vecEye + vecForward * 4096.0f;

	trace_t tr;
	UTIL_TraceLine(vecEye, vecTraceEnd, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr);

	if (tr.fraction >= 1.0f)
	{
		Warning("[EZ2 NavMesh] Aim ray did not hit any solid surface.\n");
		return;
	}

	// Snap slightly down to ensure we land right on the floor surface
	trace_t groundTr;
	UTIL_TraceLine(tr.endpos + Vector(0, 0, 8.0f), tr.endpos - Vector(0, 0, 64.0f), MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &groundTr);

	Vector vecGoal = groundTr.DidHit() ? groundTr.endpos : tr.endpos;
	Vector vecStart = pPlayer->GetAbsOrigin();
	Vector vecNext(vec3_origin);

	if (g_EZ2NavMeshManager.FindStraightPathSimple(vecStart, vecGoal, vecNext))
	{
		Msg("[EZ2 NavMesh] Start: (%.1f, %.1f, %.1f) -> Goal: (%.1f, %.1f, %.1f) | Next: (%.1f, %.1f, %.1f)\n",
			vecStart.x, vecStart.y, vecStart.z,
			vecGoal.x, vecGoal.y, vecGoal.z,
			vecNext.x, vecNext.y, vecNext.z);
	}
	else
	{
		Warning("[EZ2 NavMesh] Path query failed from (%.1f, %.1f, %.1f) to ground spot (%.1f, %.1f, %.1f).\n",
			vecStart.x, vecStart.y, vecStart.z,
			vecGoal.x, vecGoal.y, vecGoal.z);
	}
}