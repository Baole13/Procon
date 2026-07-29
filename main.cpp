// HEXUDON V1 bot, C++17 sandbox transport.
// Reads one JSON message per stdin line and writes one JSON response per stdout line.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "minijson.hpp"

using namespace std;

static const int INF = 1 << 28;

// BTC directions, even-r offset grid.
static const int DIR_EVEN[6][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,0}};
static const int DIR_ODD[6][2]  = {{-1,-1},{0,-1},{1,0},{0,1},{-1,1},{-1,0}};

struct Agent {
    int id = 0;
    int kind = 0; // 0 patrol, 1 refuel
    int pos = 0;
    int fuel = INF;
};

struct Spot {
    int id = 0;
    int pos = 0;
    int brand = 0;
    int amount = 1;
};

struct MoveCost {
    int steps = INF;
    int fuel = INF;
    bool passable = false;
};

struct Score {
    int globalBrands = 0;
    int dailyBrands = 0;
    int portions = 0;
    int responseAdvantage = 0;
};

struct OfficialScore {
    int globalBrands = 0;
    int dailyBrands = 0;
    int cappedPortions = 0;
    int responseAdvantage = 0;
};

struct OccupancyInterval {
    int pos = -1;
    int startStep = 0;
    int endStep = 0;
};

struct RefuelWindow {
    int patrolId = -1;
    int pos = -1;
    int startStep = 0;
    int endStep = 0;
    int fuelBefore = 0;
    int lostFutureStock = 0;
    bool mustServe = false;
};

struct RefuelEvent {
    int tankerId = -1;
    int patrolId = -1;
    int pos = -1;
    int effectiveStep = -1;
};

struct HeuristicEval {
    int serverEst = 0;
    int fuelSafety = 0;
    int terminalValue = 0;
    int roadPenalty = 0;
    int refuelFeasibility = 0;
};

struct Route {
    vector<int> actions;
    vector<int> visitedSpots;
    vector<int> moveSources;
    vector<pair<int,int>> arrivals; // step, position after each completed move plus start/end.
    vector<pair<int,int>> fuelTimeline; // step, fuel after each completed move plus start/end.
    vector<pair<int,int>> visitTimeline; // step, spot id.
    vector<int> posAtStep;
    vector<int> fuelAtStep;
    int stepsUsed = 0;
    int fuelUsed = 0;
    Score score;
    bool valid = false;
    int endPos = 0;
    int fuelLeft = INF;
};

struct RouteCandidate {
    Route route;
    int mode = 0;
    int conservativeSteps = 0;
    int conservativeScore = 0;
    int roadUse = 0;
    int waitOnRoadRisk = 0;
    bool endOnRoad = false;
    int slack = 0;
    int terminalValue = 0;
    int clusterId = -1;
    int spotDeficitGain = 0;
    int clusterIdStart = -1;
    int clusterIdEnd = -1;
    int futureFuelDebt = 0;
    bool refuelReachable = false;
    bool needsRefuel = false;
    int meetPoint = -1;
    int minFuelAlongRoute = INF;
    int fuelEnd = INF;
    int conservativePortions = 0;
    int executedPortions = 0;
    bool truncated = false;
    int lostVisitsAfterTruncate = 0;
    string signature;
    int fuelEfficientSteps = INF;
    int fuelSavedVsFastest = 0;
    bool paretoPathUsed = false;
    int remoteDeficitGain = 0;
    int clusterQuotaGain = 0;
    int brandMask = 0;
    int topSpotMask = 0;
    int refuelWindowStart = INF;
    int refuelWindowEnd = -1;
    int refuelWindowPos = -1;
};

struct PathOption {
    vector<int> actions;
    int steps = INF;
    int fuel = INF;
    int roadSteps = 0;
    int waitOnRoadRisk = 0;
    bool endOnRoad = false;
    bool valid = false;
    int kind = 0; // 0 fastest, 1 fuel-efficient, 2 road-light.
};

struct Cluster {
    int id = 0;
    vector<int> spots;
    vector<int> entrySpots;
    int center = 0;
    int totalStock = 0;
    int brandMask = 0;
    int patrolDemand = 1;
    int avgDistanceFromStarts = 0;
    int remainingStock = 0;
};

struct RefuelRequest {
    int patrolId = -1;
    int routeId = -1;
    int earliestStep = 0;
    int latestStep = 0;
    vector<int> meetPositions;
    int priority = 0;
    int lostFutureStock = 0;
    int serviceZone = -1;
    bool mustServe = false;
};

struct TeamPlanEval {
    OfficialScore official;
    HeuristicEval heuristic;
    int dailyBrands = 0;
    int cappedPortions = 0;
    int highStockDeficit = 0;
    int remoteDeficitGain = 0;
    int clusterBalance = 0;
    int clusterQuotaGain = 0;
    int fuelRisk = 0;
    int terminalValue = 0;
    int roadPenalty = 0;
    int responseCost = 0;
};

struct PlanEval {
    int dailyBrands = 0;
    int cappedPortions = 0;
    int assignedPortions = 0;
    int ghostVisits = 0;
    int executionEfficiency = 100;
    int assignmentCoverage = 0;
    int effectiveCapture = 0;
    int serverEst = 0;
    int spotDebtGain = 0;
    int roadUse = 0;
    int waitOnRoadRisk = 0;
    int endOnRoadRoutes = 0;
    int negativeSlackRoutes = 0;
    int roadHeavyRoutes = 0;
    int oversweepRoutes = 0;
    int fuelRisk = 0;
    int minFuelEnd = INF;
    int activePatrols = 0;
    int lowFuelPatrols = 0;
    int teamFuelRisk = 0;
    int feasibleRefuels = 0;
    int failedRendezvous = 0;
    int strandedPatrols = 0;
    int terminalPortions = 0;
};

struct SetupForecast {
    int serverEst = 0;
    int cappedPortions = 0;
    int minFuelEnd = INF;
    int lowFuelPatrols = 0;
    int dailyBrandsSum = 0;
    int reachableCap = 0;
    int tankerIdleDays = 0;
    int teamFuelRisk = 0;
    int feasibleRefuels = 0;
    int successfulRefuels = 0;
    int savedPortions = 0;
    int idlePenalty = 0;
};

enum class PlanProfile {
    LegacySmallMap,
    Hybrid16,
    GeneralLarge
};

struct ExactScore {
    bool valid = false;
    int globalBrands = 0;
    int dailyBrands = 0;
    int actualPortions = 0;
    int hardSlack = 0;
    int robustSlack = 0;
    int futureSlack = 0;
    int fuelRisk = 0;
    int roadRisk = 0;
    int failedRefuels = 0;
    int terminalValue = 0;
    int effectiveCaptureRatio = 0;
    int ghostStock = 0;
    int serverEst = 0;
};

struct TeamSimulation {
    vector<Route> routes;
    vector<RefuelEvent> refuels;
    vector<int> finalFuel;
    OfficialScore official;
    bool valid = false;
};

struct TerminalEval {
    int nextDayReachableBrands = 0;
    int nextDayCappedPortions = 0;
    int serviceablePatrols = 0;
    int strandedPatrols = 0;
    int roadRisk = 0;
};

struct GameState {
    int width = 0;
    int height = 0;
    int day = 0;
    vector<int> cells;
    vector<Spot> spots;
    vector<int> daySteps;
    vector<Agent> agents;
    map<int,int> traffic;
    vector<int> assignment;
    vector<int> spotMissDebt;
    int tankerIdleStreak = 0;
    set<int> globalBrandsSeen;
    set<int> globalSpotsSeen;
    int fuelLimit = 120;
    int busyThreshold = 5;
    int jammedThreshold = 10;
    int deadlineMarginMs = 1000000;
    int deadlineMode = 0;
    int daySeconds = 0;
    bool ultraFastMode = false;
    int hardBudgetMs = 0;

    int nodeCount() const { return width * height; }
    int dayBudget() const {
        if (day >= 0 && day < (int)daySteps.size()) return daySteps[day];
        return 30;
    }
};

static GameState G;
static const int MAX_VISITS_PER_PATROL = 8;
static const int MAX_CANDIDATE_SPOTS = 80;
static const int LOW_FUEL_ROUTE_LIMIT = 18;
static const int MIN_ROUTE_SLACK = 4;
static const int ROUTE_MODES = 17;

static int stockCap();

struct PlannerConfig {
    int dayBudget = 30;
    int medianDaySteps = 30;
    int daysLeft = 1;
    double stepRatio = 1.0;
    double mapScale = 8.0;
    double fuelRatio = 1.0;
    int stockCap = 0;
    int patrols = 0;
    int deadlineMode = 0;
    int daySeconds = 0;
    bool ultraFastMode = false;
    bool patrolBeamEnabled = true;
    int beamWidth = 40;
    int poolLimit = 9;
    int routeModeCount = 10;
    int heavyTargetLimit = 3;
    int minSlack = 0;
    int trafficRiskReserveSteps = 0;
    int deadlineReserveMs = 8;
    int maxVisits = MAX_VISITS_PER_PATROL;
    int computeBudgetMs = 220;
};

static PlannerConfig CURRENT_CONFIG;
static chrono::steady_clock::time_point PLANNER_STARTED;
static bool SUPPRESS_PLAN_LOGS = false;
static bool BASELINE_0564_MODE = false;

struct PlanningDeadline {
    chrono::steady_clock::time_point startedAt;
    chrono::steady_clock::time_point stopAt;
    int hardBudgetMs = 0;
    int reserveMs = 0;

    bool expired(int extraReserve = 0) const {
        return chrono::steady_clock::now() + chrono::milliseconds(max(0, reserveMs + extraReserve)) >= stopAt;
    }

    int remainingMs() const {
        return max(0, (int)chrono::duration_cast<chrono::milliseconds>(
            stopAt - chrono::steady_clock::now()).count());
    }
};

struct PlannerStats {
    long long dijkstraCalls = 0;
    long long dijkstraCacheHits = 0;
    long long paretoLabels = 0;
    long long patrolLabels = 0;
    long long teamStates = 0;
    long long dominancePruned = 0;
    long long deadlineChecks = 0;
    string timeoutStage;
};

static PlanningDeadline ACTIVE_DEADLINE;
static bool DEADLINE_ACTIVE = false;
static PlannerStats PLANNER_STATS;

static int medianDaySteps() {
    if (G.daySteps.empty()) return G.dayBudget();
    vector<int> steps = G.daySteps;
    sort(steps.begin(), steps.end());
    return steps[steps.size() / 2];
}

static PlannerConfig makePlannerConfig(const vector<Agent>& agents) {
    PlannerConfig cfg;
    cfg.dayBudget = G.dayBudget();
    cfg.medianDaySteps = max(1, medianDaySteps());
    cfg.daysLeft = max(1, (int)G.daySteps.size() - G.day);
    cfg.stepRatio = (double)cfg.dayBudget / (double)cfg.medianDaySteps;
    cfg.mapScale = sqrt((double)max(1, G.nodeCount()));
    cfg.stockCap = stockCap();
    long long fuelSum = 0;
    int fuelCount = 0;
    for (const Agent& agent : agents) {
        if (agent.kind != 0) continue;
        cfg.patrols++;
        fuelSum += min(agent.fuel, G.fuelLimit);
        fuelCount++;
    }
    cfg.fuelRatio = fuelCount ? (double)fuelSum / (double)max(1, fuelCount * G.fuelLimit) : 1.0;
    cfg.deadlineMode = G.deadlineMode;
    cfg.daySeconds = G.daySeconds;
    cfg.ultraFastMode = G.ultraFastMode || cfg.deadlineMode >= 2 ||
        (cfg.daySeconds > 0 && cfg.daySeconds <= 1) || G.deadlineMarginMs < 250;

    double stockPerPatrol = (double)cfg.stockCap / (double)max(1, cfg.patrols);
    bool large = cfg.mapScale >= 24.0;
    bool maxLarge = cfg.mapScale >= 32.0;
    bool finalDay = cfg.daysLeft <= 1;
    int fuelTight = cfg.fuelRatio < 0.35 ? 1 : 0;

    cfg.beamWidth = 28 + (int)(stockPerPatrol * 3.0) + cfg.dayBudget / 3 + (large ? 10 : 0);
    cfg.poolLimit = 7 + min(5, (int)(stockPerPatrol / 2.0)) + cfg.dayBudget / 45 + (large ? 2 : 0);
    cfg.routeModeCount = 8 + min(8, cfg.dayBudget / 16 + (cfg.stockCap >= 40 ? 2 : 0));
    cfg.heavyTargetLimit = 2 + (cfg.stockCap >= 32 ? 1 : 0) + (stockPerPatrol >= 6.0 ? 1 : 0);
    cfg.minSlack = finalDay ? 0 : max(0, cfg.dayBudget / (large ? 12 : 18) + (large ? 2 : 0));
    cfg.maxVisits = max(3, min(14, cfg.dayBudget / 9 + (cfg.stockCap >= 40 ? 2 : 0) - fuelTight));
    if (cfg.dayBudget <= 40 && stockPerPatrol >= 5.0 && cfg.fuelRatio >= 0.45) cfg.maxVisits = max(cfg.maxVisits, 4);
    if (cfg.dayBudget <= 40 && stockPerPatrol >= 6.5 && cfg.fuelRatio >= 0.60) cfg.maxVisits = max(cfg.maxVisits, 5);
    cfg.computeBudgetMs = large ? 420 : 190;
    cfg.computeBudgetMs += min(160, cfg.stockCap * 3);
    cfg.computeBudgetMs = min(650, max(120, cfg.computeBudgetMs));

    if (cfg.dayBudget < cfg.medianDaySteps) {
        cfg.beamWidth = max(24, (int)(cfg.beamWidth * cfg.stepRatio));
        cfg.poolLimit = max(6, (int)(cfg.poolLimit * max(0.65, cfg.stepRatio)));
        cfg.routeModeCount = max(7, (int)(cfg.routeModeCount * max(0.60, cfg.stepRatio)));
        cfg.heavyTargetLimit = max(1, cfg.heavyTargetLimit - 1);
    }
    if (cfg.ultraFastMode) {
        bool lateState = G.deadlineMarginMs < 0;
        if (large && lateState) {
            cfg.beamWidth = 8;
            cfg.poolLimit = 4;
            cfg.routeModeCount = 2;
            cfg.heavyTargetLimit = 2;
            cfg.minSlack += max(12, cfg.dayBudget / 5);
            cfg.maxVisits = min(cfg.maxVisits, 4);
            cfg.computeBudgetMs = 35;
            cfg.patrolBeamEnabled = false;
        } else if (large) {
            cfg.beamWidth = maxLarge ? 18 : 16;
            cfg.poolLimit = 6;
            cfg.routeModeCount = 5;
            cfg.heavyTargetLimit = 3;
            cfg.trafficRiskReserveSteps = max(5, cfg.dayBudget / 16);
            cfg.maxVisits = min(cfg.maxVisits, 6);
            if (G.deadlineMarginMs >= 300) {
                cfg.beamWidth = maxLarge ? 36 : 32;
                cfg.poolLimit = 10;
                cfg.routeModeCount = 10;
                cfg.heavyTargetLimit = max(5, min(7, (int)G.spots.size()));
                cfg.maxVisits = min(max(cfg.maxVisits, 8), 10);
                cfg.computeBudgetMs = min(220, max(160, G.deadlineMarginMs - 200));
            } else {
                cfg.computeBudgetMs = maxLarge ? 110 : 90;
            }
        } else {
            bool lateState = G.deadlineMarginMs < 0;
            if (lateState) {
                cfg.beamWidth = 12;
                cfg.poolLimit = 4;
                cfg.routeModeCount = 4;
                cfg.heavyTargetLimit = 1;
                cfg.minSlack += max(8, cfg.dayBudget / 6);
                cfg.computeBudgetMs = 35;
            } else {
                cfg.beamWidth = 28;
                cfg.poolLimit = 8;
                cfg.routeModeCount = 8;
                cfg.heavyTargetLimit = max(2, min(4, (int)G.spots.size()));
                cfg.minSlack += max(4, cfg.dayBudget / 12);
                cfg.maxVisits = min(max(cfg.maxVisits, 6), 9);
                cfg.computeBudgetMs = min(220, max(120, G.deadlineMarginMs - 180));
            }
        }
        cfg.patrolBeamEnabled = false;
    } else if (cfg.deadlineMode == 1) {
        cfg.beamWidth = max(24, cfg.beamWidth * 2 / 3);
        cfg.poolLimit = max(6, cfg.poolLimit * 2 / 3);
        cfg.routeModeCount = max(7, cfg.routeModeCount * 2 / 3);
        cfg.heavyTargetLimit = max(1, cfg.heavyTargetLimit - 1);
        if (large) {
            cfg.poolLimit = max(cfg.poolLimit, 8);
            cfg.routeModeCount = max(cfg.routeModeCount, 6);
            cfg.heavyTargetLimit = max(cfg.heavyTargetLimit, 3);
        }
        cfg.minSlack += max(2, cfg.dayBudget / 25);
        cfg.computeBudgetMs = min(cfg.computeBudgetMs, 180);
    }

    cfg.beamWidth = min(cfg.beamWidth, large ? 100 : 85);
    cfg.poolLimit = min(cfg.poolLimit, large ? 14 : 12);
    cfg.routeModeCount = min(cfg.routeModeCount, ROUTE_MODES);
    if (large) {
        cfg.beamWidth = min(cfg.beamWidth, cfg.deadlineMode ? 48 : 64);
        cfg.poolLimit = min(cfg.poolLimit, cfg.deadlineMode ? 8 : 10);
        cfg.routeModeCount = min(cfg.routeModeCount, cfg.deadlineMode ? 7 : 10);
        if (!cfg.ultraFastMode) cfg.computeBudgetMs = min(cfg.computeBudgetMs, 240);
    }
    if (!cfg.ultraFastMode && cfg.dayBudget <= 40 && cfg.stockCap >= 18) cfg.heavyTargetLimit = max(cfg.heavyTargetLimit, min((int)G.spots.size(), cfg.patrols + 4));
    cfg.heavyTargetLimit = min(cfg.heavyTargetLimit, max(1, (int)G.spots.size()));
    if (cfg.trafficRiskReserveSteps <= 0) {
        cfg.trafficRiskReserveSteps = max(3, cfg.dayBudget / (large ? 16 : 20));
    }
    if (G.hardBudgetMs > 0) {
        cfg.computeBudgetMs = min(cfg.computeBudgetMs, max(10, G.hardBudgetMs));
    }
    cfg.deadlineReserveMs = cfg.computeBudgetMs <= 60 ? 3 : 8;
    return cfg;
}

static int requiredSlack(int dayBudget) {
    (void)dayBudget;
    return max(MIN_ROUTE_SLACK, min(12, CURRENT_CONFIG.trafficRiskReserveSteps));
}

static bool isLargeMap() {
    return max(G.width, G.height) >= 24 || G.nodeCount() >= 24 * 24;
}

static PlanProfile selectPlanProfile() {
    int maxDim = max(G.width, G.height);
    if (maxDim <= 12 || (int)G.spots.size() <= 8) return PlanProfile::LegacySmallMap;
    if (maxDim <= 16) return PlanProfile::Hybrid16;
    return PlanProfile::GeneralLarge;
}

static const char* profileName(PlanProfile profile) {
    if (profile == PlanProfile::LegacySmallMap) return "legacySmallMap";
    if (profile == PlanProfile::Hybrid16) return "hybrid16";
    return "generalLarge";
}

static int plannerBudgetMs() {
    return CURRENT_CONFIG.computeBudgetMs;
}

static int plannerElapsedMs() {
    return (int)chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now() - PLANNER_STARTED).count();
}

static bool plannerTimeExceeded(int reserveMs = 0) {
    PLANNER_STATS.deadlineChecks++;
    if (DEADLINE_ACTIVE) return ACTIVE_DEADLINE.expired(reserveMs);
    int budget = plannerBudgetMs();
    return budget > 0 && plannerElapsedMs() + reserveMs >= budget;
}

static int adaptiveBeamWidth(int dayBudget) {
    (void)dayBudget;
    return CURRENT_CONFIG.beamWidth;
}

static int adaptivePoolLimit(int dayBudget) {
    (void)dayBudget;
    return CURRENT_CONFIG.poolLimit;
}

static int adaptiveHeavyTargetLimit(int dayBudget) {
    (void)dayBudget;
    return CURRENT_CONFIG.heavyTargetLimit;
}

static int adaptiveRouteModeCount(int dayBudget) {
    (void)dayBudget;
    return CURRENT_CONFIG.routeModeCount;
}

static int minSlackForRoute(int dayBudget) {
    (void)dayBudget;
    return CURRENT_CONFIG.trafficRiskReserveSteps;
}

[[maybe_unused]] static bool betterScore(const Score& a, const Score& b) {
    if (a.globalBrands != b.globalBrands) return a.globalBrands > b.globalBrands;
    if (a.dailyBrands != b.dailyBrands) return a.dailyBrands > b.dailyBrands;
    if (a.portions != b.portions) return a.portions > b.portions;
    return a.responseAdvantage > b.responseAdvantage;
}

static int valueOrDefault(const mj::Value& obj, const string& key, int fallback) {
    auto it = obj.obj.find(key);
    if (it == obj.obj.end() || it->second->isNull()) return fallback;
    return it->second->asInt();
}

static int firstValueOrDefault(const mj::Value& obj, const vector<string>& keys, int fallback) {
    for (const string& key : keys) {
        auto it = obj.obj.find(key);
        if (it != obj.obj.end() && !it->second->isNull()) return it->second->asInt();
    }
    return fallback;
}

static bool inBounds(int pos) {
    return pos >= 0 && pos < G.nodeCount();
}

static int terrainAt(int pos) {
    if (!inBounds(pos)) return 3;
    return G.cells[pos];
}

static int neighbor(int pos, int d) {
    if (!inBounds(pos) || d < 0 || d >= 6) return -1;
    int r = pos / G.width;
    int c = pos % G.width;
    const int (*delta)[2] = (r % 2) ? DIR_ODD : DIR_EVEN;
    int nc = c + delta[d][0];
    int nr = r + delta[d][1];
    if (nc < 0 || nc >= G.width || nr < 0 || nr >= G.height) return -1;
    return nr * G.width + nc;
}

static MoveCost moveCost(int pos, int trafficStatus) {
    switch (terrainAt(pos)) {
        case 0: return {2, 1, true}; // land
        case 2: return {3, 2, true}; // mountain
        case 1:
            if (trafficStatus == 1) return {2, 2, true};
            if (trafficStatus == 2) return {4, 2, true};
            return {1, 2, true};
        default:
            return {INF, INF, false}; // pond or invalid
    }
}

static MoveCost moveCost(int pos) {
    auto it = G.traffic.find(pos);
    return moveCost(pos, it == G.traffic.end() ? 0 : it->second);
}

static int dirTo(int from, int to) {
    for (int d = 0; d < 6; ++d) {
        if (neighbor(from, d) == to) return d;
    }
    return -1;
}

struct DijkstraResult {
    vector<int> dist;
    vector<int> fuel;
    vector<int> prev;
    bool complete = true;
};

static map<int, DijkstraResult> DAY_PATH_CACHE;

static DijkstraResult computeDijkstraFrom(int src) {
    DijkstraResult result;
    PLANNER_STATS.dijkstraCalls++;
    result.dist.assign(G.nodeCount(), INF);
    result.fuel.assign(G.nodeCount(), INF);
    result.prev.assign(G.nodeCount(), -1);
    if (!inBounds(src) || terrainAt(src) == 3) return result;

    priority_queue<pair<int,int>, vector<pair<int,int>>, greater<pair<int,int>>> pq;
    result.dist[src] = 0;
    result.fuel[src] = 0;
    pq.push({0, src});
    int expansions = 0;
    while (!pq.empty()) {
        if (!BASELINE_0564_MODE && ((++expansions) & 127) == 0 && plannerTimeExceeded(1)) {
            result.complete = false;
            if (PLANNER_STATS.timeoutStage.empty()) PLANNER_STATS.timeoutStage = "dijkstra";
            return result;
        }
        auto [du, u] = pq.top();
        pq.pop();
        if (du != result.dist[u]) continue;
        MoveCost cost = moveCost(u);
        if (!cost.passable) continue;
        for (int d = 0; d < 6; ++d) {
            int v = neighbor(u, d);
            if (v < 0 || terrainAt(v) == 3) continue;
            int nd = du + cost.steps;
            int nf = result.fuel[u] + cost.fuel;
            if (nd < result.dist[v] || (nd == result.dist[v] && nf < result.fuel[v])) {
                result.dist[v] = nd;
                result.fuel[v] = nf;
                result.prev[v] = u;
                pq.push({nd, v});
            }
        }
    }
    return result;
}

static DijkstraResult dijkstraFrom(int src) {
    auto it = DAY_PATH_CACHE.find(src);
    if (it != DAY_PATH_CACHE.end()) {
        PLANNER_STATS.dijkstraCacheHits++;
        return it->second;
    }
    DijkstraResult result = computeDijkstraFrom(src);
    if (result.complete) DAY_PATH_CACHE[src] = result;
    return result;
}

static vector<int> restorePath(int src, int dst, const vector<int>& prev) {
    vector<int> empty;
    if (!inBounds(src) || !inBounds(dst) || terrainAt(src) == 3 || terrainAt(dst) == 3) return empty;
    if (src != dst && prev[dst] < 0) return empty;

    vector<int> path;
    for (int cur = dst; cur != src; cur = prev[cur]) {
        if (cur < 0) return empty;
        path.push_back(cur);
    }
    reverse(path.begin(), path.end());
    return path;
}

static vector<int> shortestPath(int src, int dst) {
    DijkstraResult result = dijkstraFrom(src);
    return restorePath(src, dst, result.prev);
}

struct FuelPathResult {
    vector<int> path;
    int steps = INF;
    int fuel = INF;
    bool valid = false;
    bool pareto = false;
};

static FuelPathResult fuelParetoPath(int src, int dst, int stepBudget, int fuelBudget, bool preferFuel) {
    FuelPathResult result;
    long long globalLabelCap = isLargeMap() ? 35000 : 24000;
    if (PLANNER_STATS.paretoLabels >= globalLabelCap || plannerTimeExceeded(8)) return result;
    if (!inBounds(src) || !inBounds(dst) || terrainAt(src) == 3 || terrainAt(dst) == 3) return result;
    int n = G.nodeCount();
    int fuelCap = max(0, min(fuelBudget, min(G.fuelLimit, 600)));
    vector<vector<int>> dist(n, vector<int>(fuelCap + 1, INF));
    vector<vector<int>> parentPos(n, vector<int>(fuelCap + 1, -1));
    vector<vector<int>> parentFuel(n, vector<int>(fuelCap + 1, -1));
    using Node = tuple<int,int,int>;
    priority_queue<Node, vector<Node>, greater<Node>> pq;
    dist[src][0] = 0;
    pq.push({0, 0, src});
    int expansions = 0;
    const int labelCap = isLargeMap() ? 28000 : 20000;
    while (!pq.empty()) {
        if (((++expansions) & 127) == 0) {
            PLANNER_STATS.paretoLabels += 127;
            if (expansions >= labelCap || PLANNER_STATS.paretoLabels >= globalLabelCap || plannerTimeExceeded(2)) {
                if (PLANNER_STATS.timeoutStage.empty()) PLANNER_STATS.timeoutStage = "pareto";
                return result;
            }
        }
        auto [steps, fuel, pos] = pq.top();
        pq.pop();
        if (steps != dist[pos][fuel]) continue;
        if (steps > stepBudget) continue;
        MoveCost cost = moveCost(pos);
        if (!cost.passable) continue;
        for (int d = 0; d < 6; ++d) {
            int nxt = neighbor(pos, d);
            if (nxt < 0 || terrainAt(nxt) == 3) continue;
            int ns = steps + cost.steps;
            int nf = fuel + cost.fuel;
            if (ns > stepBudget || nf > fuelCap) continue;
            if (ns < dist[nxt][nf]) {
                dist[nxt][nf] = ns;
                parentPos[nxt][nf] = pos;
                parentFuel[nxt][nf] = fuel;
                pq.push({ns, nf, nxt});
            }
        }
    }

    int bestFuel = -1;
    long long bestRank = (1LL << 60);
    for (int f = 0; f <= fuelCap; ++f) {
        int s = dist[dst][f];
        if (s >= INF) continue;
        long long rank = preferFuel ? (100000LL * f + s) : (100000LL * s + f);
        if (rank < bestRank) {
            bestRank = rank;
            bestFuel = f;
        }
    }
    if (bestFuel < 0) return result;

    vector<int> rev;
    int cur = dst;
    int fuel = bestFuel;
    while (cur != src) {
        rev.push_back(cur);
        int pp = parentPos[cur][fuel];
        int pf = parentFuel[cur][fuel];
        if (pp < 0 || pf < 0) return result;
        cur = pp;
        fuel = pf;
    }
    reverse(rev.begin(), rev.end());
    result.path = std::move(rev);
    result.steps = dist[dst][bestFuel];
    result.fuel = bestFuel;
    result.valid = true;
    result.pareto = true;
    return result;
}

static vector<int> roadLightPath(int src, int dst, int stepBudget) {
    if (!inBounds(src) || !inBounds(dst) || terrainAt(src) == 3 || terrainAt(dst) == 3) return {};
    int n = G.nodeCount();
    vector<int> best(n, INF), steps(n, INF), prev(n, -1);
    using Node = tuple<int,int,int>;
    priority_queue<Node, vector<Node>, greater<Node>> pq;
    best[src] = 0;
    steps[src] = 0;
    pq.push({0, 0, src});
    int expansions = 0;
    while (!pq.empty()) {
        if (((++expansions) & 127) == 0 && plannerTimeExceeded(2)) break;
        auto [rank, used, pos] = pq.top();
        pq.pop();
        if (rank != best[pos] || used != steps[pos]) continue;
        MoveCost cost = moveCost(pos);
        if (!cost.passable) continue;
        for (int d = 0; d < 6; ++d) {
            int nxt = neighbor(pos, d);
            if (nxt < 0 || terrainAt(nxt) == 3) continue;
            int ns = used + cost.steps;
            if (ns > stepBudget) continue;
            int roadRisk = terrainAt(pos) == 1 ? 1 : 0;
            int nr = rank + cost.steps * 10 + cost.fuel * 2 + roadRisk * ((G.daySeconds > 0 && G.daySeconds <= 2) ? 35 : 20);
            if (nr < best[nxt] || (nr == best[nxt] && ns < steps[nxt])) {
                best[nxt] = nr;
                steps[nxt] = ns;
                prev[nxt] = pos;
                pq.push({nr, ns, nxt});
            }
        }
    }
    return restorePath(src, dst, prev);
}

static PathOption pathOptionFromPath(const Agent& agent, const vector<int>& path, int dayBudget, int kind) {
    PathOption option;
    option.kind = kind;
    vector<int> actions;
    int cur = agent.pos;
    int used = 0;
    int fuel = agent.fuel;
    int roadSteps = 0;
    for (int nxt : path) {
        MoveCost cost = moveCost(cur);
        int dir = dirTo(cur, nxt);
        if (dir < 0 || !cost.passable || used + cost.steps > dayBudget || fuel < cost.fuel) return option;
        if (terrainAt(cur) == 1) roadSteps++;
        actions.push_back(dir);
        used += cost.steps;
        fuel -= cost.fuel;
        cur = nxt;
    }
    option.actions = std::move(actions);
    option.steps = used;
    option.fuel = agent.fuel - fuel;
    option.roadSteps = roadSteps;
    option.endOnRoad = inBounds(cur) && terrainAt(cur) == 1;
    option.valid = true;
    return option;
}

static vector<PathOption> pathOptionsToSpot(const Agent& agent, int spotId, int dayBudget) {
    vector<PathOption> options;
    if (spotId < 0 || spotId >= (int)G.spots.size()) return options;
    int dst = G.spots[spotId].pos;
    vector<vector<int>> paths;
    paths.push_back(shortestPath(agent.pos, dst));
    if (!CURRENT_CONFIG.ultraFastMode && (isLargeMap() || CURRENT_CONFIG.fuelRatio < 0.65)) {
        FuelPathResult fuelPath = fuelParetoPath(agent.pos, dst, dayBudget, agent.fuel, true);
        if (fuelPath.valid) paths.push_back(fuelPath.path);
    }
    paths.push_back(roadLightPath(agent.pos, dst, dayBudget));

    set<string> seen;
    int kind = 0;
    for (const vector<int>& path : paths) {
        ostringstream sig;
        for (int pos : path) sig << pos << ".";
        if (path.empty() && agent.pos != dst) {
            kind++;
            continue;
        }
        if (!seen.insert(sig.str()).second) {
            kind++;
            continue;
        }
        PathOption option = pathOptionFromPath(agent, path, dayBudget, kind);
        if (option.valid) options.push_back(std::move(option));
        kind++;
    }
    sort(options.begin(), options.end(), [](const PathOption& a, const PathOption& b) {
        int ar = a.roadSteps * 8 + a.fuel * 2 + a.steps + (a.endOnRoad ? 12 : 0);
        int br = b.roadSteps * 8 + b.fuel * 2 + b.steps + (b.endOnRoad ? 12 : 0);
        return ar < br;
    });
    if ((int)options.size() > 4) options.resize(4);
    return options;
}

static const Spot* spotAt(int pos) {
    for (const Spot& s : G.spots) {
        if (s.pos == pos) return &s;
    }
    return nullptr;
}

static void finalizeClusters(vector<Cluster>& clusters, const vector<Agent>* agents = nullptr) {
    for (Cluster& cluster : clusters) {
        cluster.totalStock = 0;
        cluster.remainingStock = 0;
        cluster.brandMask = 0;
        cluster.entrySpots.clear();
        if (cluster.spots.empty()) continue;
        int bestSpot = cluster.spots.front();
        for (int sid : cluster.spots) {
            int stock = max(1, G.spots[sid].amount);
            cluster.totalStock += stock;
            cluster.remainingStock += stock;
            if (G.spots[sid].brand >= 0 && G.spots[sid].brand < 30) {
                cluster.brandMask |= (1 << G.spots[sid].brand);
            }
            if (stock > G.spots[bestSpot].amount) bestSpot = sid;
        }
        cluster.center = G.spots[bestSpot].pos;
        cluster.patrolDemand = max(1, (cluster.totalStock + 5) / 6);

        vector<pair<int,int>> entries;
        for (int sid : cluster.spots) entries.push_back({-G.spots[sid].amount, sid});
        sort(entries.begin(), entries.end());
        for (int i = 0; i < (int)entries.size() && i < 3; ++i) {
            cluster.entrySpots.push_back(entries[i].second);
        }

        if (agents) {
            long long totalDist = 0;
            int count = 0;
            for (const Agent& agent : *agents) {
                if (agent.kind != 0) continue;
                DijkstraResult paths = dijkstraFrom(agent.pos);
                int d = paths.dist[cluster.center];
                if (d < INF) {
                    totalDist += d;
                    count++;
                }
            }
            cluster.avgDistanceFromStarts = count ? (int)(totalDist / count) : INF;
        }
    }
    sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
        if (a.totalStock != b.totalStock) return a.totalStock > b.totalStock;
        if (a.avgDistanceFromStarts != b.avgDistanceFromStarts) return a.avgDistanceFromStarts < b.avgDistanceFromStarts;
        return a.spots.size() > b.spots.size();
    });
    for (int i = 0; i < (int)clusters.size(); ++i) clusters[i].id = i;
}

static vector<Cluster> buildLinkedClusters(const vector<Agent>* agents) {
    vector<Cluster> clusters;
    int n = (int)G.spots.size();
    vector<int> used(n, 0);
    int linkBudget = max(8, min(24, G.dayBudget() / 5 + (isLargeMap() ? 4 : 0)));
    for (int i = 0; i < n; ++i) {
        if (used[i]) continue;
        Cluster cluster;
        cluster.id = (int)clusters.size();
        queue<int> q;
        q.push(i);
        used[i] = 1;
        while (!q.empty()) {
            int si = q.front();
            q.pop();
            cluster.spots.push_back(si);
            DijkstraResult paths = dijkstraFrom(G.spots[si].pos);
            for (int sj = 0; sj < n; ++sj) {
                if (!used[sj] && paths.dist[G.spots[sj].pos] <= linkBudget) {
                    used[sj] = 1;
                    q.push(sj);
                }
            }
        }
        clusters.push_back(cluster);
    }
    finalizeClusters(clusters, agents);
    return clusters;
}

static vector<Cluster> buildSpatialClusters(const vector<Agent>& agents, int k) {
    vector<Cluster> clusters;
    int n = (int)G.spots.size();
    if (n == 0) return clusters;
    k = max(1, min(k, n));

    vector<int> centers;
    vector<int> byStock(n);
    iota(byStock.begin(), byStock.end(), 0);
    sort(byStock.begin(), byStock.end(), [](int a, int b) {
        if (G.spots[a].amount != G.spots[b].amount) return G.spots[a].amount > G.spots[b].amount;
        return G.spots[a].id < G.spots[b].id;
    });
    centers.push_back(byStock.front());
    while ((int)centers.size() < k) {
        if (plannerTimeExceeded(2)) {
            if (PLANNER_STATS.timeoutStage.empty()) PLANNER_STATS.timeoutStage = "clustering";
            break;
        }
        int best = -1;
        int bestScore = -1;
        for (int sid = 0; sid < n; ++sid) {
            if (find(centers.begin(), centers.end(), sid) != centers.end()) continue;
            DijkstraResult paths = dijkstraFrom(G.spots[sid].pos);
            int nearest = INF;
            for (int c : centers) nearest = min(nearest, paths.dist[G.spots[c].pos]);
            int score = nearest + 6 * max(1, G.spots[sid].amount);
            if (score > bestScore) {
                bestScore = score;
                best = sid;
            }
        }
        if (best < 0) break;
        centers.push_back(best);
    }

    clusters.resize(centers.size());
    for (int i = 0; i < (int)clusters.size(); ++i) {
        clusters[i].id = i;
        clusters[i].center = G.spots[centers[i]].pos;
    }
    for (int sid = 0; sid < n; ++sid) {
        if ((sid & 7) == 0 && plannerTimeExceeded(2)) {
            if (PLANNER_STATS.timeoutStage.empty()) PLANNER_STATS.timeoutStage = "clustering";
            break;
        }
        int bestCluster = 0;
        int bestDist = INF;
        DijkstraResult paths = dijkstraFrom(G.spots[sid].pos);
        for (int ci = 0; ci < (int)centers.size(); ++ci) {
            int d = paths.dist[G.spots[centers[ci]].pos];
            if (d < bestDist) {
                bestDist = d;
                bestCluster = ci;
            }
        }
        clusters[bestCluster].spots.push_back(sid);
    }
    finalizeClusters(clusters, &agents);
    return clusters;
}

static vector<Cluster> buildClusters(const vector<Agent>& agents) {
    int patrols = 0;
    for (const Agent& agent : agents) if (agent.kind == 0) patrols++;
    bool largeMap = max(G.width, G.height) >= 24 || G.nodeCount() >= 24 * 24;
    bool mediumDense = false;
    if (largeMap && G.spots.size() >= 6) {
        int k = min((int)G.spots.size(), max(2, patrols / 2));
        return buildSpatialClusters(agents, k);
    }
    if (mediumDense) {
        int k = min((int)G.spots.size(), max(2, patrols / 2));
        return buildSpatialClusters(agents, k);
    }
    return buildLinkedClusters(&agents);
}

static int nearestClusterId(int pos, const vector<Cluster>& clusters) {
    int best = -1;
    int bestDist = INF;
    DijkstraResult paths = dijkstraFrom(pos);
    for (const Cluster& cluster : clusters) {
        int d = paths.dist[cluster.center];
        if (d < bestDist) {
            bestDist = d;
            best = cluster.id;
        }
    }
    return best;
}

static int estimateTerminalValue(const Route& route, const vector<Cluster>& clusters) {
    if (clusters.empty()) return route.fuelLeft;
    DijkstraResult paths = dijkstraFrom(route.endPos);
    int best = 0;
    for (const Cluster& cluster : clusters) {
        int d = paths.dist[cluster.center];
        if (d >= INF) continue;
        int value = 100 * cluster.totalStock + 30 * __builtin_popcount((unsigned)cluster.brandMask) - 5 * d;
        best = max(best, value);
    }
    return best + min(route.fuelLeft, G.fuelLimit);
}

static int stockCap() {
    int cap = 0;
    for (const Spot& spot : G.spots) cap += max(1, spot.amount);
    return cap;
}

static int desiredQuotaForSpot(int sid) {
    if (sid < 0 || sid >= (int)G.spots.size()) return 0;
    int stock = max(1, G.spots[sid].amount);
    if (max(G.width, G.height) >= 24) return min(stock, stock >= 6 ? 6 : stock);
    return min(stock, stock >= 6 ? 5 : stock);
}

static int spotDebt(int sid) {
    if (sid < 0 || sid >= (int)G.spotMissDebt.size()) return 0;
    return max(0, G.spotMissDebt[sid]);
}

static int spotDebtBonus(int sid) {
    if (sid < 0 || sid >= (int)G.spots.size()) return 0;
    int stock = max(1, G.spots[sid].amount);
    return min(3500, spotDebt(sid) * (55 + 12 * stock));
}

static vector<int> topDebtSpotIds(int limit) {
    vector<pair<int,int>> ranked;
    for (size_t sid = 0; sid < G.spots.size(); ++sid) {
        int debt = spotDebt((int)sid);
        if (debt <= 0) continue;
        int stock = max(1, G.spots[sid].amount);
        ranked.push_back({debt * (100 + 25 * stock) + stock * 80, (int)sid});
    }
    sort(ranked.rbegin(), ranked.rend());
    vector<int> result;
    for (int i = 0; i < (int)ranked.size() && i < limit; ++i) result.push_back(ranked[i].second);
    return result;
}

static vector<int> computeSpotDeficits(const vector<int>& remainingStock) {
    vector<int> deficits(G.spots.size(), 0);
    for (size_t i = 0; i < G.spots.size(); ++i) {
        int rem = i < remainingStock.size() ? remainingStock[i] : 0;
        if (rem <= 0) continue;
        int debtBoost = min(rem, max(0, spotDebt((int)i) / max(1, max(1, G.spots[i].amount) / 2)));
        deficits[i] = min(rem, desiredQuotaForSpot((int)i) + debtBoost);
    }
    return deficits;
}

static int spotDeficitGainOfRoute(const Route& route, const vector<int>& deficits) {
    int gain = 0;
    set<int> seen;
    for (int sid : route.visitedSpots) {
        if (sid < 0 || sid >= (int)G.spots.size() || seen.count(sid)) continue;
        seen.insert(sid);
        int deficit = sid < (int)deficits.size() ? deficits[sid] : max(1, G.spots[sid].amount);
        int stock = max(1, G.spots[sid].amount);
        gain += max(0, deficit) * (60 + 15 * stock);
    }
    return gain;
}

static int estimateFutureFuelDebt(const Route& route, const vector<Cluster>& clusters) {
    if (G.day + 1 >= (int)G.daySteps.size()) return 0;
    int safeFuel = max(G.fuelLimit / 3, 35);
    int debt = max(0, safeFuel - route.fuelLeft);
    if (!clusters.empty()) {
        DijkstraResult paths = dijkstraFrom(route.endPos);
        int nearest = INF;
        for (const Cluster& cluster : clusters) nearest = min(nearest, paths.dist[cluster.center]);
        if (nearest >= INF) debt += 100;
        else debt += max(0, nearest - G.dayBudget() / 3) / 2;
    }
    return debt;
}

static bool isRefuelReachable(const Route& route, const vector<Agent>& agents, int dayBudget) {
    int refuelPos = -1;
    int bestFuel = -1;
    for (const Agent& agent : agents) {
        if (agent.kind != 1) continue;
        if (agent.fuel > bestFuel) {
            bestFuel = agent.fuel;
            refuelPos = agent.pos;
        }
    }
    if (refuelPos < 0) return false;
    DijkstraResult paths = dijkstraFrom(refuelPos);
    if (!route.posAtStep.empty()) {
        for (int step = 0; step <= dayBudget && step < (int)route.posAtStep.size(); ++step) {
            int pos = route.posAtStep[step];
            if (!inBounds(pos)) continue;
            int d = paths.dist[pos];
            if (d < INF && d <= step) return true;
        }
    }
    for (const auto& arrival : route.arrivals) {
        int step = arrival.first;
        int pos = arrival.second;
        if (!inBounds(pos)) continue;
        int d = paths.dist[pos];
        if (d < INF && d <= step) return true;
    }
    int d = paths.dist[route.endPos];
    return d < INF && d <= dayBudget;
}

static pair<int,int> refuelWindowForRoute(const Route& route, const vector<Agent>& agents, int dayBudget) {
    int refuelPos = -1;
    int bestFuel = -1;
    for (const Agent& agent : agents) {
        if (agent.kind != 1) continue;
        if (agent.fuel > bestFuel) {
            bestFuel = agent.fuel;
            refuelPos = agent.pos;
        }
    }
    if (refuelPos < 0) return {INF, -1};
    DijkstraResult paths = dijkstraFrom(refuelPos);
    int bestStart = INF;
    int bestPos = -1;
    if (!route.posAtStep.empty()) {
        for (int step = 0; step <= dayBudget && step < (int)route.posAtStep.size(); ++step) {
            int pos = route.posAtStep[step];
            if (!inBounds(pos)) continue;
            int d = paths.dist[pos];
            if (d >= INF || d > step) continue;
            bool sameNext = (step + 1 < (int)route.posAtStep.size() && route.posAtStep[step + 1] == pos);
            if ((sameNext || step >= route.stepsUsed) && step < bestStart) {
                bestStart = step;
                bestPos = pos;
            }
        }
        if (bestPos >= 0) return {bestStart, bestPos};
    }
    for (const auto& arrival : route.arrivals) {
        int step = arrival.first;
        int pos = arrival.second;
        if (!inBounds(pos)) continue;
        int d = paths.dist[pos];
        if (d < INF && d <= step && step < bestStart) {
            bestStart = step;
            bestPos = pos;
        }
    }
    int d = paths.dist[route.endPos];
    if (d < INF && d <= dayBudget) {
        int start = max(route.stepsUsed, d);
        if (start < bestStart) {
            bestStart = start;
            bestPos = route.endPos;
        }
    }
    return {bestStart, bestPos};
}

static int minFuelAlongRoute(const Agent& agent, const Route& route) {
    int fuel = agent.fuel;
    int minFuel = fuel;
    for (int src : route.moveSources) {
        MoveCost cost = moveCost(src);
        if (cost.passable) fuel -= cost.fuel;
        minFuel = min(minFuel, fuel);
    }
    return minFuel;
}

static void appendWait(vector<int>& actions, int waitSteps) {
    if (waitSteps > 0) actions.push_back(-waitSteps);
}

static void collectSpotVisit(
    int pos,
    int step,
    vector<int>& visitedSpots,
    vector<pair<int,int>>& visitTimeline,
    Score& score,
    set<int>& routeBrands,
    set<int>& routeSpots
) {
    const Spot* spot = spotAt(pos);
    if (!spot || routeSpots.count(spot->id)) return;
    routeSpots.insert(spot->id);
    visitedSpots.push_back(spot->id);
    visitTimeline.push_back({step, spot->id});
    score.portions += 1;
    if (!G.globalBrandsSeen.count(spot->brand) && !routeBrands.count(spot->brand)) {
        score.globalBrands++;
    }
    routeBrands.insert(spot->brand);
}

static Route simulateRoute(const Agent& start, const vector<int>& rawActions, int dayBudget) {
    Route route;
    route.actions = rawActions;
    route.endPos = start.pos;
    route.fuelLeft = start.fuel;

    int used = 0;
    int cur = start.pos;
    int fuel = start.fuel;
    set<int> routeBrands;
    set<int> routeSpots;
    route.arrivals.push_back({0, cur});
    route.fuelTimeline.push_back({0, fuel});
    route.posAtStep.assign(dayBudget + 1, cur);
    route.fuelAtStep.assign(dayBudget + 1, fuel);

    if (start.kind == 0) {
        collectSpotVisit(cur, 0, route.visitedSpots, route.visitTimeline, route.score, routeBrands, routeSpots);
    }

    for (int action : rawActions) {
        if (action < 0) {
            int wait = -action;
            if (wait <= 0 || used + wait > dayBudget) return route;
            for (int t = used + 1; t <= used + wait && t <= dayBudget; ++t) {
                route.posAtStep[t] = cur;
                route.fuelAtStep[t] = fuel;
            }
            used += wait;
            continue;
        }
        if (action >= 6 || used >= dayBudget) return route;
        int nxt = neighbor(cur, action);
        MoveCost cost = moveCost(cur);
        if (nxt < 0 || terrainAt(nxt) == 3 || !cost.passable) return route;
        int fuelCost = start.kind == 1 ? 0 : cost.fuel;
        if (used + cost.steps > dayBudget || fuel < fuelCost) return route;
        route.moveSources.push_back(cur);
        for (int t = used + 1; t < used + cost.steps && t <= dayBudget; ++t) {
            route.posAtStep[t] = cur;
            route.fuelAtStep[t] = fuel;
        }
        used += cost.steps;
        fuel -= fuelCost;
        cur = nxt;
        route.posAtStep[used] = cur;
        route.fuelAtStep[used] = fuel;
        route.arrivals.push_back({used, cur});
        route.fuelTimeline.push_back({used, fuel});
        if (start.kind == 0) {
            collectSpotVisit(cur, used, route.visitedSpots, route.visitTimeline, route.score, routeBrands, routeSpots);
        }
    }

    route.stepsUsed = used;
    for (int t = used + 1; t <= dayBudget; ++t) {
        route.posAtStep[t] = cur;
        route.fuelAtStep[t] = fuel;
    }
    route.fuelUsed = start.fuel >= INF / 2 ? 0 : start.fuel - fuel;
    route.fuelLeft = fuel;
    route.endPos = cur;
    route.score.dailyBrands = (int)routeBrands.size();
    route.score.responseAdvantage = dayBudget - used;
    route.valid = used <= dayBudget;
    return route;
}

static vector<vector<int>> normalizePlan(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    vector<vector<int>> normalized = plan;
    normalized.resize(agents.size());
    for (size_t i = 0; i < agents.size(); ++i) {
        Route r = simulateRoute(agents[i], normalized[i], dayBudget);
        if (!r.valid || r.stepsUsed > dayBudget) {
            normalized[i].clear();
            appendWait(normalized[i], dayBudget);
            continue;
        }
        int remaining = dayBudget - r.stepsUsed;
        appendWait(normalized[i], remaining);
    }
    return normalized;
}

static vector<OccupancyInterval> buildOccupancyIntervals(const Route& route) {
    vector<OccupancyInterval> intervals;
    if (route.posAtStep.empty()) return intervals;
    int start = 0;
    int pos = route.posAtStep[0];
    for (int t = 1; t < (int)route.posAtStep.size(); ++t) {
        if (route.posAtStep[t] == pos) continue;
        intervals.push_back({pos, start, t - 1});
        start = t;
        pos = route.posAtStep[t];
    }
    intervals.push_back({pos, start, (int)route.posAtStep.size() - 1});
    return intervals;
}

static bool overlapForRefuel(
    const Route& patrol,
    const Route& tanker,
    int& eventStep,
    int& eventPos
) {
    int limit = min(patrol.posAtStep.size(), tanker.posAtStep.size());
    for (int t = 0; t + 1 < limit; ++t) {
        if (patrol.posAtStep[t] == tanker.posAtStep[t] &&
            patrol.posAtStep[t + 1] == tanker.posAtStep[t + 1] &&
            patrol.posAtStep[t] == patrol.posAtStep[t + 1]) {
            eventStep = t + 1;
            eventPos = patrol.posAtStep[t];
            return true;
        }
    }
    return false;
}

static TeamSimulation simulateTeamPlan(
    const vector<vector<int>>& plan,
    const vector<Agent>& agents,
    int dayBudget
) {
    TeamSimulation sim;
    sim.routes.resize(agents.size());
    sim.finalFuel.resize(agents.size(), 0);
    if (plan.size() != agents.size()) return sim;

    vector<int> remainingStock(G.spots.size(), 0);
    for (size_t sid = 0; sid < G.spots.size(); ++sid) {
        remainingStock[sid] = max(1, G.spots[sid].amount);
    }
    set<int> brands;
    for (size_t i = 0; i < agents.size(); ++i) {
        sim.routes[i] = simulateRoute(agents[i], plan[i], dayBudget);
        if (!sim.routes[i].valid || sim.routes[i].stepsUsed != dayBudget) return sim;
        sim.finalFuel[i] = sim.routes[i].fuelLeft;
        if (agents[i].kind != 0) continue;
        for (int sid : sim.routes[i].visitedSpots) {
            if (sid < 0 || sid >= (int)remainingStock.size() || remainingStock[sid] <= 0) continue;
            remainingStock[sid]--;
            sim.official.cappedPortions++;
            brands.insert(G.spots[sid].brand);
        }
    }

    set<int> servedPatrols;
    for (size_t ti = 0; ti < agents.size(); ++ti) {
        if (agents[ti].kind != 1) continue;
        for (size_t pi = 0; pi < agents.size(); ++pi) {
            if (agents[pi].kind != 0 || servedPatrols.count((int)pi)) continue;
            int eventStep = -1;
            int eventPos = -1;
            if (!overlapForRefuel(sim.routes[pi], sim.routes[ti], eventStep, eventPos)) continue;
            sim.refuels.push_back({(int)ti, (int)pi, eventPos, eventStep});
            sim.finalFuel[pi] = G.fuelLimit;
            servedPatrols.insert((int)pi);
        }
    }
    sim.official.dailyBrands = (int)brands.size();
    sim.valid = true;
    return sim;
}

static bool validatePlan(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    return simulateTeamPlan(plan, agents, dayBudget).valid;
}

static vector<vector<int>> buildSafePlan(const vector<Agent>& agents, int dayBudget) {
    vector<vector<int>> plan(agents.size());
    for (auto& actions : plan) appendWait(actions, dayBudget);
    return plan;
}

static Route routeToSpot(const Agent& agent, const Spot& target, int dayBudget);

static vector<vector<int>> buildUltraFastPlan(const vector<Agent>& agents, int dayBudget) {
    vector<vector<int>> plan(agents.size());
    set<int> plannedBrands;
    vector<int> assignedStock(G.spots.size(), 0);
    vector<int> patrolIds;
    for (const Agent& agent : agents) {
        if (agent.kind == 0) patrolIds.push_back(agent.id);
    }
    sort(patrolIds.begin(), patrolIds.end(), [&](int a, int b) {
        return agents[a].fuel > agents[b].fuel;
    });
    set<int> allBrands;
    for (const Spot& spot : G.spots) allBrands.insert(spot.brand);
    vector<int> strategicSpotBonus(G.spots.size(), 0);
    bool hasDebt = any_of(G.spotMissDebt.begin(), G.spotMissDebt.end(), [](int debt) { return debt > 0; });
    if ((isLargeMap() || hasDebt) && !patrolIds.empty()) {
        vector<DijkstraResult> startPaths(agents.size());
        for (int agentId : patrolIds) startPaths[agentId] = dijkstraFrom(agents[agentId].pos);
        vector<pair<int,int>> ranked;
        for (size_t sid = 0; sid < G.spots.size(); ++sid) {
            int bestDist = INF;
            int farDist = 0;
            int reachable = 0;
            for (int agentId : patrolIds) {
                int d = startPaths[agentId].dist[G.spots[sid].pos];
                if (d >= INF || d > dayBudget) continue;
                bestDist = min(bestDist, d);
                farDist = max(farDist, d);
                reachable++;
            }
            if (!reachable) continue;
            int stock = max(1, G.spots[sid].amount);
            int score =
                220000 * stock +
                35000 * desiredQuotaForSpot((int)sid) +
                4500 * spotDebt((int)sid) +
                4500 * min(farDist, dayBudget) -
                2500 * bestDist +
                (bestDist > dayBudget / 3 ? 180000 : 0);
            ranked.push_back({score, (int)sid});
        }
        sort(ranked.rbegin(), ranked.rend());
        int limit = min((int)ranked.size(), max(3, min((int)G.spots.size(), CURRENT_CONFIG.heavyTargetLimit + 3)));
        for (int i = 0; i < limit; ++i) {
            strategicSpotBonus[ranked[i].second] = max(0, ranked[i].first / 4);
        }
    }

    for (int agentId : patrolIds) {
        const Agent& agent = agents[agentId];
        DijkstraResult paths = dijkstraFrom(agent.pos);
        int bestSpot = -1;
        long long bestRank = -1;
        for (size_t sid = 0; sid < G.spots.size(); ++sid) {
            const Spot& spot = G.spots[sid];
            if (assignedStock[sid] >= max(1, spot.amount)) continue;
            int d = paths.dist[spot.pos];
            int f = paths.fuel[spot.pos];
            if (d >= INF || f >= INF || d > dayBudget || f > agent.fuel) continue;
            bool newBrand = !plannedBrands.count(spot.brand);
            int slack = dayBudget - d;
            long long rank =
                (newBrand ? 100000000LL : 0LL) +
                1000000LL * max(1, spot.amount) +
                10000LL * desiredQuotaForSpot((int)sid) +
                2500LL * spotDebt((int)sid) +
                ((isLargeMap() || spotDebt((int)sid) > 0) && !newBrand && plannedBrands.size() >= min(allBrands.size(), patrolIds.size()) ? strategicSpotBonus[sid] : 0) +
                100LL * slack -
                1000LL * d -
                200LL * f;
            if (isLargeMap() && d > dayBudget / 2) rank += 500000LL + strategicSpotBonus[sid] / 3;
            if (rank > bestRank) {
                bestRank = rank;
                bestSpot = (int)sid;
            }
        }
        if (bestSpot >= 0) {
            vector<int> path = restorePath(agent.pos, G.spots[bestSpot].pos, paths.prev);
            int cur = agent.pos;
            int used = 0;
            int fuelLeft = agent.fuel;
            for (int nxt : path) {
                int dir = dirTo(cur, nxt);
                if (dir < 0) break;
                MoveCost cost = moveCost(cur);
                if (!cost.passable || used + cost.steps > dayBudget || fuelLeft < cost.fuel) break;
                plan[agentId].push_back(dir);
                used += cost.steps;
                fuelLeft -= cost.fuel;
                cur = nxt;
            }
            assignedStock[bestSpot]++;
            plannedBrands.insert(G.spots[bestSpot].brand);
            set<int> localSpots;
            localSpots.insert(bestSpot);
            int fastExtraLimit = isLargeMap() ? (dayBudget >= 90 ? 3 : 2) : (dayBudget >= 60 ? 4 : 3);
            for (int extra = 0; extra < fastExtraLimit; ++extra) {
                DijkstraResult nextPaths = dijkstraFrom(cur);
                int nextSpot = -1;
                long long nextRank = -1;
                vector<int> nextPath;
                for (size_t sid = 0; sid < G.spots.size(); ++sid) {
                    if (localSpots.count((int)sid)) continue;
                    const Spot& spot = G.spots[sid];
                    if (assignedStock[sid] >= max(1, spot.amount)) continue;
                    int d = nextPaths.dist[spot.pos];
                    int f = nextPaths.fuel[spot.pos];
                    if (d >= INF || f >= INF || used + d > dayBudget || f > fuelLeft) continue;
                    int slack = dayBudget - used - d;
                    if (slack < max(4, dayBudget / 10)) continue;
                    bool newBrand = !plannedBrands.count(spot.brand);
                    long long rank =
                        (newBrand ? 10000000LL : 0LL) +
                        500000LL * max(1, spot.amount) +
                        2200LL * spotDebt((int)sid) +
                        ((isLargeMap() || spotDebt((int)sid) > 0) ? strategicSpotBonus[sid] / 2 : 0) -
                        1000LL * d - 200LL * f + 100LL * slack;
                    if (rank > nextRank) {
                        nextRank = rank;
                        nextSpot = (int)sid;
                        nextPath = restorePath(cur, spot.pos, nextPaths.prev);
                    }
                }
                if (nextSpot < 0) break;
                bool ok = true;
                for (int nxt : nextPath) {
                    int dir = dirTo(cur, nxt);
                    MoveCost cost = moveCost(cur);
                    if (dir < 0 || !cost.passable || used + cost.steps > dayBudget || fuelLeft < cost.fuel) {
                        ok = false;
                        break;
                    }
                    plan[agentId].push_back(dir);
                    used += cost.steps;
                    fuelLeft -= cost.fuel;
                    cur = nxt;
                }
                if (!ok) break;
                assignedStock[nextSpot]++;
                plannedBrands.insert(G.spots[nextSpot].brand);
                localSpots.insert(nextSpot);
            }
        }
    }
    for (const Agent& agent : agents) {
        if (agent.kind != 1) continue;
        int targetPos = -1;
        string reason = "idle";
        int lowestFuel = INF;
        for (int patrolId : patrolIds) {
            const Agent& patrol = agents[patrolId];
            if (patrol.fuel < lowestFuel) {
                lowestFuel = patrol.fuel;
                targetPos = patrol.pos;
                reason = "low_fuel";
            }
        }
        if (lowestFuel > max(LOW_FUEL_ROUTE_LIMIT, G.fuelLimit / 4)) {
            targetPos = -1;
            int bestScore = -INF;
            int bestDebt = 0;
            DijkstraResult paths = dijkstraFrom(agent.pos);
            for (const Spot& spot : G.spots) {
                int d = paths.dist[spot.pos];
                if (d >= INF || d > dayBudget) continue;
                int score = 1000 * max(1, spot.amount) + 80 * desiredQuotaForSpot(spot.id) + 20 * spotDebt(spot.id) - 8 * d;
                if (isLargeMap() && d > dayBudget / 3) score += 300;
                if (score > bestScore) {
                    bestScore = score;
                    targetPos = spot.pos;
                    bestDebt = spotDebt(spot.id);
                    reason = bestDebt > 0 ? "debt_cluster" : "stock_cluster";
                }
            }
        }
        if (targetPos >= 0 && targetPos != agent.pos) {
            Spot pseudo;
            pseudo.id = -1;
            pseudo.pos = targetPos;
            pseudo.brand = -1;
            pseudo.amount = 0;
            Route tankerRoute = routeToSpot(agent, pseudo, dayBudget);
            if (tankerRoute.valid) plan[agent.id] = tankerRoute.actions;
        }
        if (reason == "idle") G.tankerIdleStreak++;
        else G.tankerIdleStreak = 0;
        if (!SUPPRESS_PLAN_LOGS) {
            cerr << "fast_tanker day=" << G.day
                 << " id=" << agent.id
                 << " target=" << targetPos
                 << " tanker_reason=" << reason
                 << " tanker_idle_streak=" << G.tankerIdleStreak
                 << "\n";
        }
    }
    return normalizePlan(plan, agents, dayBudget);
}

static int roadUseOfRoute(const Route& route);
static int explicitWaitOnRoadRisk(const Agent& agent, const vector<int>& actions, int dayBudget);
static bool routeEndsOnRoad(const Route& route);
static int conservativeSteps(const Route& route, const vector<int>& plannedRoadUse);

[[maybe_unused]] static int marginalGain(
    const Spot& spot,
    const set<int>& dailyBrands,
    const set<int>& localSpots,
    const vector<int>& assignedStock
) {
    if (localSpots.count(spot.id)) return 0;
    int remaining = max(0, spot.amount - assignedStock[spot.id]);
    if (remaining <= 0) return 0;

    int gain = 10 * remaining + 1;
    if (!dailyBrands.count(spot.brand)) gain += 100000;
    else gain += 100;
    return gain;
}

static Route routeToSpot(const Agent& agent, const Spot& target, int dayBudget) {
    vector<int> path = shortestPath(agent.pos, target.pos);
    vector<int> actions;
    int cur = agent.pos;
    int used = 0;
    int fuel = agent.fuel;

    for (int nxt : path) {
        MoveCost cost = moveCost(cur);
        int dir = dirTo(cur, nxt);
        if (dir < 0 || !cost.passable) break;
        int fuelCost = agent.kind == 1 ? 0 : cost.fuel;
        if (used + cost.steps > dayBudget || fuel < fuelCost) break;
        actions.push_back(dir);
        used += cost.steps;
        fuel -= fuelCost;
        cur = nxt;
    }
    return simulateRoute(agent, actions, dayBudget);
}

static PlanEval evaluatePlanForSelection(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    PlanEval eval;
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    if (!team.valid) return eval;
    vector<int> remainingStock(G.spots.size(), 0);
    for (size_t i = 0; i < G.spots.size(); ++i) remainingStock[i] = max(1, G.spots[i].amount);
    set<int> brands;
    vector<int> plannedRoadUse(G.nodeCount(), 0);
    vector<Route> routes = team.routes;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0 || !routes[i].valid) continue;
        if (routes[i].stepsUsed > 0) eval.activePatrols++;
        int routeRoadUse = roadUseOfRoute(routes[i]);
        int routeWaitOnRoad = explicitWaitOnRoadRisk(agents[i], plan[i], dayBudget);
        eval.roadUse += routeRoadUse;
        eval.waitOnRoadRisk += routeWaitOnRoad;
        if (routeEndsOnRoad(routes[i])) eval.endOnRoadRoutes++;
        int finalFuel = i < team.finalFuel.size() ? team.finalFuel[i] : routes[i].fuelLeft;
        eval.minFuelEnd = min(eval.minFuelEnd, finalFuel);
        int slack = dayBudget - conservativeSteps(routes[i], plannedRoadUse);
        if (slack < 0) eval.negativeSlackRoutes++;
        if (routeRoadUse >= max(6, dayBudget / 10)) eval.roadHeavyRoutes++;
        if (routes[i].stepsUsed >= dayBudget - 2 && (int)routes[i].visitedSpots.size() <= 1) eval.oversweepRoutes++;
        int softFuel = max(G.fuelLimit / 5, LOW_FUEL_ROUTE_LIMIT);
        int patrolFuelRisk = max(0, softFuel - finalFuel);
        eval.fuelRisk += patrolFuelRisk;
        if (finalFuel <= softFuel) eval.lowFuelPatrols++;
        for (int sid : routes[i].visitedSpots) {
            if (sid < 0 || sid >= (int)G.spots.size()) continue;
            eval.assignedPortions++;
            if (remainingStock[sid] <= 0) {
                eval.ghostVisits++;
                continue;
            }
            eval.spotDebtGain += spotDebtBonus(sid);
            remainingStock[sid]--;
            eval.cappedPortions++;
            brands.insert(G.spots[sid].brand);
        }
        for (int src : routes[i].moveSources) {
            if (terrainAt(src) == 1) plannedRoadUse[src]++;
        }
    }
    eval.dailyBrands = (int)brands.size();
    eval.feasibleRefuels = (int)team.refuels.size();
    int hardFuel = max(G.fuelLimit / 8, 8);
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0) continue;
        if (routes[i].fuelLeft <= hardFuel && team.finalFuel[i] < G.fuelLimit) {
            eval.failedRendezvous++;
        }
    }
    if (eval.minFuelEnd == INF) eval.minFuelEnd = 0;
    int lowFuelOverload = max(0, eval.lowFuelPatrols - max(1, CURRENT_CONFIG.patrols / 2));
    eval.teamFuelRisk = eval.fuelRisk + lowFuelOverload * max(20, G.fuelLimit / 4);
    int roadDivisor = (G.daySeconds > 0 && G.daySeconds <= 2) ? 10 : 18;
    int roadHeavyPenalty = (G.daySeconds > 0 && G.daySeconds <= 2) ? eval.roadHeavyRoutes : eval.roadHeavyRoutes / 2;
    int slackPenalty = (G.daySeconds > 0 && G.daySeconds <= 2) ? 2 * eval.negativeSlackRoutes : eval.negativeSlackRoutes;
    int roadWaitPenalty = eval.waitOnRoadRisk / ((G.daySeconds > 0 && G.daySeconds <= 2) ? 5 : 8);
    int endRoadPenalty = (G.day + 1 < (int)G.daySteps.size()) ? eval.endOnRoadRoutes / 2 : 0;
    eval.serverEst = max(0, eval.cappedPortions - eval.roadUse / roadDivisor - roadWaitPenalty - endRoadPenalty - eval.teamFuelRisk / 120 - slackPenalty - roadHeavyPenalty);
    int cap = max(1, stockCap());
    eval.executionEfficiency = eval.assignedPortions ? max(0, min(100, 100 * eval.serverEst / max(1, eval.assignedPortions))) : 100;
    eval.assignmentCoverage = min(100, 100 * eval.cappedPortions / cap);
    eval.effectiveCapture = min(100, 100 * eval.serverEst / cap);
    if (G.day + 1 < (int)G.daySteps.size()) {
        int nextBudget = G.daySteps[G.day + 1];
        set<int> nextBrands;
        for (size_t i = 0; i < agents.size(); ++i) {
            if (agents[i].kind != 0) continue;
            DijkstraResult paths = dijkstraFrom(routes[i].endPos);
            int reachable = 0;
            for (const Spot& spot : G.spots) {
                if (paths.dist[spot.pos] <= nextBudget &&
                    paths.fuel[spot.pos] <= team.finalFuel[i]) {
                    reachable++;
                    nextBrands.insert(spot.brand);
                }
            }
            if (reachable == 0) eval.strandedPatrols++;
            else eval.terminalPortions += min(3, reachable);
        }
        eval.terminalPortions = min(stockCap(), eval.terminalPortions);
    }
    return eval;
}

static ExactScore exactScoreForPlan(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    ExactScore score;
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    if (!team.valid) return score;

    PlanEval eval = evaluatePlanForSelection(plan, agents, dayBudget);
    score.valid = true;
    score.dailyBrands = eval.dailyBrands;
    score.actualPortions = eval.cappedPortions;
    score.robustSlack = -eval.negativeSlackRoutes;
    score.hardSlack = 0;
    score.futureSlack = eval.terminalPortions;
    score.fuelRisk = eval.teamFuelRisk;
    score.roadRisk = eval.roadUse + 2 * eval.waitOnRoadRisk + eval.endOnRoadRoutes * max(4, dayBudget / 10) + eval.roadHeavyRoutes * max(4, dayBudget / 10);
    score.failedRefuels = eval.failedRendezvous;
    score.terminalValue = eval.terminalPortions - eval.strandedPatrols * 3;
    score.serverEst = eval.serverEst;
    set<int> newGlobalBrands;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0 || i >= team.routes.size()) continue;
        for (int sid : team.routes[i].visitedSpots) {
            if (sid < 0 || sid >= (int)G.spots.size()) continue;
            int brand = G.spots[sid].brand;
            if (!G.globalBrandsSeen.count(brand)) newGlobalBrands.insert(brand);
        }
    }
    score.globalBrands = (int)newGlobalBrands.size();

    int optimistic = 0;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind == 0 && i < team.routes.size()) optimistic += (int)team.routes[i].visitedSpots.size();
    }
    int cap = stockCap();
    score.effectiveCaptureRatio = cap ? (100 * score.actualPortions / cap) : 0;
    score.ghostStock = max(0, optimistic - score.actualPortions);
    return score;
}

static bool exactScoreBetter(const ExactScore& a, const ExactScore& b) {
    if (a.valid != b.valid) return a.valid;
    if (!a.valid) return false;
    if (a.globalBrands != b.globalBrands) return a.globalBrands > b.globalBrands;
    if (a.dailyBrands != b.dailyBrands) return a.dailyBrands > b.dailyBrands;
    if (a.actualPortions != b.actualPortions) return a.actualPortions > b.actualPortions;
    bool aHardOk = a.hardSlack >= 0 && a.robustSlack >= 0;
    bool bHardOk = b.hardSlack >= 0 && b.robustSlack >= 0;
    if (aHardOk != bHardOk) return aHardOk;
    if (a.failedRefuels != b.failedRefuels) return a.failedRefuels < b.failedRefuels;
    if (a.fuelRisk != b.fuelRisk) return a.fuelRisk < b.fuelRisk;
    if (a.roadRisk != b.roadRisk) return a.roadRisk < b.roadRisk;
    if (a.terminalValue != b.terminalValue) return a.terminalValue > b.terminalValue;
    return a.serverEst > b.serverEst;
}

static bool exactScoreStrictlyBetter(const ExactScore& a, const ExactScore& b) {
    return exactScoreBetter(a, b) && !exactScoreBetter(b, a);
}

static bool strongPlanAcceptableExact(
    const ExactScore& strong,
    const ExactScore& fast,
    bool strongInterrupted
) {
    if (!strong.valid) return false;
    if (!fast.valid) return true;
    if (strongInterrupted) return exactScoreStrictlyBetter(strong, fast);
    return exactScoreBetter(strong, fast);
}

static bool exactScoreClearlyWorse(const ExactScore& candidate, const ExactScore& baseline) {
    if (!candidate.valid) return baseline.valid;
    if (!baseline.valid) return false;
    if (candidate.globalBrands < baseline.globalBrands) return true;
    if (candidate.globalBrands > baseline.globalBrands) return false;
    if (candidate.dailyBrands < baseline.dailyBrands) return true;
    if (candidate.dailyBrands > baseline.dailyBrands) return false;
    if (candidate.actualPortions + 1 < baseline.actualPortions) return true;
    if (candidate.serverEst + 2 < baseline.serverEst) return true;
    return false;
}

static bool strongPlanAcceptable(const PlanEval& strong, const PlanEval& fast, int dayBudget);

static bool strongPlanAcceptableGuarded(
    const PlanEval& strongEval,
    const PlanEval& fastEval,
    const ExactScore& strongExact,
    const ExactScore& fastExact,
    int dayBudget,
    bool strongInterrupted
) {
    if (!strongPlanAcceptable(strongEval, fastEval, dayBudget)) return false;
    if (exactScoreClearlyWorse(strongExact, fastExact)) return false;
    if (strongInterrupted && !exactScoreStrictlyBetter(strongExact, fastExact)) return false;
    return true;
}

static bool shouldRunStrongPlanner(const PlanEval& fast, int dayBudget) {
    (void)fast;
    (void)dayBudget;
    if (G.deadlineMarginMs < 0) return false;
    int required = CURRENT_CONFIG.ultraFastMode ? 18 : max(25, plannerBudgetMs() / 5);
    if (DEADLINE_ACTIVE) return ACTIVE_DEADLINE.remainingMs() > required;
    return !plannerTimeExceeded(required);
}

static bool strongPlanAcceptable(const PlanEval& strong, const PlanEval& fast, int dayBudget) {
    bool finalDay = G.day + 1 >= (int)G.daySteps.size();
    if (strong.dailyBrands < fast.dailyBrands) return false;
    int portionGain = strong.cappedPortions - fast.cappedPortions;
    int serverGain = strong.serverEst - fast.serverEst;
    bool debtRepairGain = strong.spotDebtGain > fast.spotDebtGain + 500 && serverGain >= 0;
    bool largeEffectiveOverride = isLargeMap() && portionGain >= -2 && serverGain >= 4;
    if (strong.cappedPortions < fast.cappedPortions && !largeEffectiveOverride) return false;
    if (strong.serverEst < fast.serverEst) return false;
    if (!finalDay && strong.failedRendezvous > fast.failedRendezvous) return false;
    if (!finalDay && strong.waitOnRoadRisk > fast.waitOnRoadRisk + max(4, dayBudget / 12) && serverGain <= 1 && !debtRepairGain) return false;
    if (!finalDay && strong.endOnRoadRoutes > fast.endOnRoadRoutes + 1 && serverGain <= 1 && !debtRepairGain) return false;
    if (!finalDay && strong.negativeSlackRoutes > fast.negativeSlackRoutes && portionGain <= 2 && serverGain <= 2 && !debtRepairGain) return false;
    if (!finalDay && strong.fuelRisk > fast.fuelRisk + max(8, G.fuelLimit / 12) && serverGain <= 1) return false;
    if (!finalDay && CURRENT_CONFIG.fuelRatio < 0.45) {
        int allowedLowFuel = max(1, CURRENT_CONFIG.patrols / 2);
        if (strong.lowFuelPatrols > allowedLowFuel && portionGain <= 2 && serverGain <= 1) return false;
        if (strong.lowFuelPatrols > fast.lowFuelPatrols && strong.teamFuelRisk > fast.teamFuelRisk + max(20, G.fuelLimit / 5) && serverGain <= 1) return false;
    }
    if (finalDay && strong.activePatrols + 1 < fast.activePatrols && portionGain <= 1) return false;
    if (strong.cappedPortions == fast.cappedPortions) {
        if (strong.failedRendezvous != fast.failedRendezvous) return strong.failedRendezvous < fast.failedRendezvous;
        if (strong.strandedPatrols != fast.strandedPatrols) return strong.strandedPatrols < fast.strandedPatrols;
        if (strong.feasibleRefuels != fast.feasibleRefuels) return strong.feasibleRefuels > fast.feasibleRefuels;
        if (strong.terminalPortions != fast.terminalPortions) return strong.terminalPortions > fast.terminalPortions;
        if (strong.serverEst != fast.serverEst) return strong.serverEst > fast.serverEst;
        if (strong.negativeSlackRoutes != fast.negativeSlackRoutes) return strong.negativeSlackRoutes < fast.negativeSlackRoutes;
        if (strong.waitOnRoadRisk != fast.waitOnRoadRisk) return strong.waitOnRoadRisk < fast.waitOnRoadRisk;
        if (strong.endOnRoadRoutes != fast.endOnRoadRoutes) return strong.endOnRoadRoutes < fast.endOnRoadRoutes;
        if (strong.roadUse != fast.roadUse) return strong.roadUse < fast.roadUse;
        return strong.teamFuelRisk < fast.teamFuelRisk;
    }
    if (!finalDay && strong.roadHeavyRoutes > fast.roadHeavyRoutes + max(1, CURRENT_CONFIG.patrols / 3) && serverGain <= 1 && !debtRepairGain) {
        return false;
    }
    (void)dayBudget;
    return true;
}

static string strongPlanDecisionReason(const PlanEval& strong, const PlanEval& fast, int dayBudget) {
    bool finalDay = G.day + 1 >= (int)G.daySteps.size();
    if (strong.dailyBrands < fast.dailyBrands) return "brand_guard";
    int portionGain = strong.cappedPortions - fast.cappedPortions;
    int serverGain = strong.serverEst - fast.serverEst;
    bool debtRepairGain = strong.spotDebtGain > fast.spotDebtGain + 500 && serverGain >= 0;
    bool largeEffectiveOverride = isLargeMap() && portionGain >= -2 && serverGain >= 4;
    if (strong.cappedPortions < fast.cappedPortions && !largeEffectiveOverride) return "portion_guard";
    if (strong.serverEst < fast.serverEst) return "fast_server_better";
    if (!finalDay && strong.failedRendezvous > fast.failedRendezvous) return "rendezvous_guard";
    if (!finalDay && strong.waitOnRoadRisk > fast.waitOnRoadRisk + max(4, dayBudget / 12) && serverGain <= 1 && !debtRepairGain) return "road_wait_guard";
    if (!finalDay && strong.endOnRoadRoutes > fast.endOnRoadRoutes + 1 && serverGain <= 1 && !debtRepairGain) return "end_road_guard";
    if (!finalDay && strong.negativeSlackRoutes > fast.negativeSlackRoutes && portionGain <= 2 && serverGain <= 2 && !debtRepairGain) return "road_slack_guard";
    if (!finalDay && strong.fuelRisk > fast.fuelRisk + max(8, G.fuelLimit / 12) && serverGain <= 1) return "fuel_guard";
    if (!finalDay && CURRENT_CONFIG.fuelRatio < 0.45) {
        int allowedLowFuel = max(1, CURRENT_CONFIG.patrols / 2);
        if (strong.lowFuelPatrols > allowedLowFuel && portionGain <= 2 && serverGain <= 1) return "fuel_guard";
        if (strong.lowFuelPatrols > fast.lowFuelPatrols && strong.teamFuelRisk > fast.teamFuelRisk + max(20, G.fuelLimit / 5) && serverGain <= 1) return "fuel_guard";
    }
    if (finalDay && strong.activePatrols + 1 < fast.activePatrols && portionGain <= 1) return "active_patrol_guard";
    if (!finalDay && strong.roadHeavyRoutes > fast.roadHeavyRoutes + max(1, CURRENT_CONFIG.patrols / 3) && serverGain <= 1 && !debtRepairGain) {
        return "road_slack_guard";
    }
    if (debtRepairGain && (portionGain > 0 || serverGain > 0 || strong.spotDebtGain > fast.spotDebtGain)) return "strong_debt_repair";
    if (portionGain < 0 && largeEffectiveOverride) return "strong_effective_gain";
    if (portionGain > 0 || serverGain > 0) return "strong_gain";
    if (strong.failedRendezvous < fast.failedRendezvous || strong.strandedPatrols < fast.strandedPatrols) return "strong_safer";
    if (strong.feasibleRefuels > fast.feasibleRefuels) return "strong_refuel";
    if (strong.terminalPortions > fast.terminalPortions) return "strong_terminal";
    if (strong.negativeSlackRoutes < fast.negativeSlackRoutes || strong.waitOnRoadRisk < fast.waitOnRoadRisk || strong.endOnRoadRoutes < fast.endOnRoadRoutes || strong.roadUse < fast.roadUse) return "strong_lower_risk";
    if (strong.teamFuelRisk < fast.teamFuelRisk) return "strong_lower_fuel_risk";
    (void)dayBudget;
    return "incumbent_tie";
}

static int spotRankGain(const Spot& spot, int mode, const set<int>& localBrands, const set<int>& localSpots) {
    if (localSpots.count(spot.id)) return 0;
    int stock = max(1, spot.amount);
    bool newLocalBrand = !localBrands.count(spot.brand);
    if (mode == 0) return (newLocalBrand ? 100000 : 100) + 20 * stock;
    if (mode == 1) return 1000 * stock + (newLocalBrand ? 500 : 0);
    if (mode == 2) return 1000 + (newLocalBrand ? 200 : 0);
    if (mode == 3) return (newLocalBrand ? 2000 : 300) + 30 * stock;
    if (mode == 4) return (newLocalBrand ? 3000 : 500) + 10 * stock;
    if (mode == 5) return (newLocalBrand ? 10000 : 800) + 100 * stock;
    if (mode == 6) return (newLocalBrand ? 5000 : 600) + 140 * stock;
    if (mode == 7) return (newLocalBrand ? 8000 : 500) + 60 * stock;
    if (mode == 8) return (newLocalBrand ? 12000 : 400) + 35 * stock;
    if (mode == 9) return (newLocalBrand ? 4000 : 1000) + 180 * stock;
    if (mode == 10) return (newLocalBrand ? 7000 : 700) + 90 * stock;
    if (mode == 11) return (newLocalBrand ? 6000 : 900) + 120 * stock;
    if (mode == 12) return (newLocalBrand ? 9000 : 1500) + 260 * stock; // single-heavy-harvest
    if (mode == 13) return (newLocalBrand ? 5000 : 700) + 110 * stock;  // cluster-sweep-short
    if (mode == 14) return (newLocalBrand ? 7000 : 1300) + 240 * stock; // remote-heavy
    if (mode == 15) return (newLocalBrand ? 8000 : 900) + 80 * stock;   // return-to-refuel-zone
    if (mode == 16) return (newLocalBrand ? 6500 : 1400) + 210 * stock; // road-light-heavy-repeat
    return (newLocalBrand ? 6000 : 900) + 120 * stock;
}

static int maxVisitsForMode(int mode, int dayBudget) {
    (void)dayBudget;
    int base = CURRENT_CONFIG.maxVisits;
    if (mode == 7 || mode == 10) return max(3, base - 4);
    if (mode == 9 || mode == 11) return base + 2;
    if (mode == 12 || mode == 13 || mode == 15) return max(3, base - 6);
    if (mode == 14) return max(2, base - 7);
    if (mode == 16) return max(2, min(base, 4));
    return base;
}

static Route generateRouteForMode(const Agent& agent, int mode, int dayBudget, const vector<Cluster>& clusters) {
    vector<int> actions;
    set<int> localBrands;
    set<int> localSpots;
    int cur = agent.pos;
    int used = 0;
    int fuelLeft = agent.fuel;
    int visits = 0;
    int maxVisits = maxVisitsForMode(mode, dayBudget);
    int focusCluster = -1;
    if (!clusters.empty() && (mode == 6 || mode == 9 || mode == 11 || mode == 12 || mode == 13 || mode == 14 || mode == 15 || mode == 16)) {
        if (mode == 6 || mode == 13 || mode == 15 || mode == 16) focusCluster = nearestClusterId(agent.pos, clusters);
        else if (mode == 14) {
            DijkstraResult fromAgent = dijkstraFrom(agent.pos);
            int best = -1;
            int bestScore = -INF;
            for (const Cluster& cluster : clusters) {
                int d = fromAgent.dist[cluster.center];
                if (d >= INF) continue;
                int score = 20 * cluster.totalStock + d;
                if (score > bestScore) {
                    bestScore = score;
                    best = cluster.id;
                }
            }
            focusCluster = best >= 0 ? best : clusters.front().id;
        } else {
            focusCluster = clusters.front().id;
        }
    }

    const Spot* startSpot = spotAt(cur);
    if (startSpot) {
        localBrands.insert(startSpot->brand);
        localSpots.insert(startSpot->id);
        visits++;
    }

    while (used < dayBudget && visits < maxVisits) {
        int bestSpot = -1;
        long long bestRank = -1;
        int bestStepCost = INF;
        vector<int> bestPath;

        DijkstraResult paths = dijkstraFrom(cur);
        vector<int> candidates(G.spots.size());
        for (size_t i = 0; i < G.spots.size(); ++i) candidates[i] = (int)i;
        sort(candidates.begin(), candidates.end(), [&](int a, int b) {
            int da = paths.dist[G.spots[a].pos];
            int db = paths.dist[G.spots[b].pos];
            if (mode == 1 && G.spots[a].amount != G.spots[b].amount) return G.spots[a].amount > G.spots[b].amount;
            if (da != db) return da < db;
            return G.spots[a].amount > G.spots[b].amount;
        });
        if ((int)candidates.size() > MAX_CANDIDATE_SPOTS) candidates.resize(MAX_CANDIDATE_SPOTS);

        for (int si : candidates) {
            const Spot& spot = G.spots[si];
            int gain = spotRankGain(spot, mode, localBrands, localSpots);
            gain += spotDebt(spot.id) * (mode == 14 ? 140 : 70);
            if (gain <= 0) continue;

            int steps = paths.dist[spot.pos];
            int fuel = paths.fuel[spot.pos];
            if (steps >= INF || fuel >= INF) continue;
            if (used + steps > dayBudget || fuel > fuelLeft) continue;
            if (agent.fuel <= max(LOW_FUEL_ROUTE_LIMIT, G.fuelLimit / 8) && fuel > max(1, fuelLeft - 4)) continue;
            bool finalDay = G.day + 1 >= (int)G.daySteps.size();
            bool newLocalBrand = !localBrands.count(spot.brand);
            if (!finalDay && CURRENT_CONFIG.daysLeft >= 2 && CURRENT_CONFIG.fuelRatio < 0.65 && !newLocalBrand) {
                int futureBuffer = max(LOW_FUEL_ROUTE_LIMIT, G.fuelLimit / 3);
                if (fuelLeft - fuel < futureBuffer) continue;
            }

            int roadPenalty = 0;
            vector<int> path = restorePath(cur, spot.pos, paths.prev);
            int source = cur;
            for (int nxt : path) {
                if (terrainAt(source) == 1) roadPenalty++;
                source = nxt;
            }

            long long denom = max(1, steps);
            if (mode == 3) denom += 3 * roadPenalty;
            if (mode == 4) denom += max(0, requiredSlack(dayBudget) - (dayBudget - used - steps)) * 4;
            if (mode == 7) denom += max(0, fuel - max(1, fuelLeft / 3)) * 6;
            if (mode == 8) denom += roadPenalty * 5;
            if (mode == 10) denom += max(0, steps - dayBudget / 4) * 3;
            if (mode == 12) denom += max(0, steps - dayBudget / 2);
            if (mode == 13) denom += max(0, steps - dayBudget / 4) * 5;
            if (mode == 14) denom = max(1LL, denom - max(0, steps - dayBudget / 3) / 3);
            if (mode == 15) denom += max(0, fuel - max(1, fuelLeft / 2)) * 8;
            if (mode == 16) denom += roadPenalty * 12 + max(0, fuel - max(1, fuelLeft / 3)) * 4;
            long long rank = 1000000LL * gain / denom;
            if (focusCluster >= 0) {
                for (int sid : clusters[focusCluster].spots) {
                    if (sid == spot.id) {
                        rank += (mode == 9 || mode == 11 || mode == 12 || mode == 14 || mode == 16) ? 800000LL : 350000LL;
                        break;
                    }
                }
            }
            rank += 7500LL * spotDebt(spot.id);
            if (rank > bestRank || (rank == bestRank && steps < bestStepCost)) {
                bestRank = rank;
                bestStepCost = steps;
                bestSpot = (int)si;
                bestPath = path;
            }
        }

        if (bestSpot < 0) break;

        for (int nxt : bestPath) {
            MoveCost cost = moveCost(cur);
            int dir = dirTo(cur, nxt);
            if (dir < 0 || !cost.passable || used + cost.steps > dayBudget || fuelLeft < cost.fuel) {
                bestPath.clear();
                break;
            }
            actions.push_back(dir);
            used += cost.steps;
            fuelLeft -= cost.fuel;
            cur = nxt;
        }
        if (bestPath.empty() && cur != G.spots[bestSpot].pos) break;
        localSpots.insert(G.spots[bestSpot].id);
        localBrands.insert(G.spots[bestSpot].brand);
        visits++;

        if ((mode == 7 || mode == 10) && fuelLeft < max(LOW_FUEL_ROUTE_LIMIT, G.fuelLimit / 5)) break;
        if (mode == 10 && dayBudget - used < requiredSlack(dayBudget) + dayBudget / 5) break;
        if (mode == 12 && visits >= 3) break;
        if (mode == 13 && visits >= 4) break;
        if (mode == 14 && visits >= 3) break;
        if (mode == 15 && (fuelLeft < max(G.fuelLimit / 3, 40) || visits >= 4)) break;
        if (mode == 16 && visits >= 3) break;
    }

    return simulateRoute(agent, actions, dayBudget);
}

static Route generateRouteToHeavySpot(
    const Agent& agent,
    int targetSpot,
    int dayBudget,
    const vector<Cluster>& clusters
) {
    if (targetSpot < 0 || targetSpot >= (int)G.spots.size()) return simulateRoute(agent, {}, dayBudget);

    vector<int> actions;
    int cur = agent.pos;
    int used = 0;
    int fuelLeft = agent.fuel;
    set<int> localBrands;
    set<int> localSpots;

    const Spot* startSpot = spotAt(cur);
    if (startSpot) {
        localBrands.insert(startSpot->brand);
        localSpots.insert(startSpot->id);
    }

    bool preferFuel =
        !CURRENT_CONFIG.ultraFastMode &&
        plannerBudgetMs() >= 140 &&
        (CURRENT_CONFIG.fuelRatio < 0.65 || agent.fuel < max(G.fuelLimit * 2 / 3, 1));
    FuelPathResult initialPath;
    if (preferFuel) {
        initialPath = fuelParetoPath(cur, G.spots[targetSpot].pos, dayBudget, fuelLeft, true);
    }
    vector<int> path = initialPath.valid ? initialPath.path : shortestPath(cur, G.spots[targetSpot].pos);
    for (int nxt : path) {
        MoveCost cost = moveCost(cur);
        int dir = dirTo(cur, nxt);
        if (dir < 0 || !cost.passable || used + cost.steps > dayBudget || fuelLeft < cost.fuel) {
            return simulateRoute(agent, actions, dayBudget);
        }
        actions.push_back(dir);
        used += cost.steps;
        fuelLeft -= cost.fuel;
        cur = nxt;
    }
    localBrands.insert(G.spots[targetSpot].brand);
    localSpots.insert(targetSpot);

    int clusterId = nearestClusterId(cur, clusters);
    int maxExtra = max(G.width, G.height) >= 24 ? 3 : 5;
    for (int extra = 0; extra < maxExtra; ++extra) {
        DijkstraResult pathsFromCur = dijkstraFrom(cur);
        int bestSpot = -1;
        long long bestRank = -1;
        vector<int> bestPath;

        vector<int> candidates;
        if (clusterId >= 0 && clusterId < (int)clusters.size()) {
            candidates = clusters[clusterId].spots;
        } else {
            candidates.resize(G.spots.size());
            iota(candidates.begin(), candidates.end(), 0);
        }

        for (int sid : candidates) {
            if (localSpots.count(sid)) continue;
            int steps = pathsFromCur.dist[G.spots[sid].pos];
            int fuel = pathsFromCur.fuel[G.spots[sid].pos];
            if (steps >= INF || fuel >= INF) continue;
            if (used + steps > dayBudget || fuel > fuelLeft) continue;
            int slackAfter = dayBudget - used - steps;
            if (G.day + 1 < (int)G.daySteps.size() && fuelLeft - fuel < max(12, G.fuelLimit / 12)) continue;

            bool newBrand = !localBrands.count(G.spots[sid].brand);
            long long rank =
                5000LL * max(1, G.spots[sid].amount) +
                6000LL * spotDebt(sid) +
                (newBrand ? 50000LL : 0) +
                100LL * slackAfter -
                1000LL * steps;
            if (rank > bestRank) {
                bestRank = rank;
                bestSpot = sid;
                bestPath = restorePath(cur, G.spots[sid].pos, pathsFromCur.prev);
            }
        }

        if (bestSpot < 0) break;
        for (int nxt : bestPath) {
            MoveCost cost = moveCost(cur);
            int dir = dirTo(cur, nxt);
            if (dir < 0 || !cost.passable || used + cost.steps > dayBudget || fuelLeft < cost.fuel) {
                bestPath.clear();
                break;
            }
            actions.push_back(dir);
            used += cost.steps;
            fuelLeft -= cost.fuel;
            cur = nxt;
        }
        if (bestPath.empty() && cur != G.spots[bestSpot].pos) break;
        localBrands.insert(G.spots[bestSpot].brand);
        localSpots.insert(bestSpot);
    }

    return simulateRoute(agent, actions, dayBudget);
}

static vector<int> buildBeamSpotCandidates(const Agent& agent, const vector<Cluster>& clusters) {
    DijkstraResult fromAgent = dijkstraFrom(agent.pos);
    vector<pair<int,int>> ranked;
    set<int> clusterEntry;
    for (const Cluster& cluster : clusters) {
        for (int sid : cluster.entrySpots) clusterEntry.insert(sid);
    }
    for (size_t sid = 0; sid < G.spots.size(); ++sid) {
        const Spot& spot = G.spots[sid];
        int d = fromAgent.dist[spot.pos];
        int f = fromAgent.fuel[spot.pos];
        if (d >= INF || f > agent.fuel) continue;
        int stock = max(1, spot.amount);
        int score =
            700 * stock +
            220 * desiredQuotaForSpot((int)sid) +
            90 * spotDebt((int)sid) +
            (clusterEntry.count((int)sid) ? 350 : 0) +
            (d > G.dayBudget() / 3 ? 180 : 0) -
            12 * d -
            8 * f;
        ranked.push_back({score, (int)sid});
    }
    sort(ranked.rbegin(), ranked.rend());
    vector<int> result;
    int limit = min((int)ranked.size(), isLargeMap() ? 12 : 10);
    if (G.dayBudget() <= 40) limit = min((int)ranked.size(), 8);
    for (int i = 0; i < limit; ++i) result.push_back(ranked[i].second);
    return result;
}

static vector<Route> generateBeamRoutesForAgent(
    const Agent& agent,
    int dayBudget,
    const vector<Cluster>& clusters
) {
    struct Label {
        int pos = 0;
        int steps = 0;
        int fuel = 0;
        int brandMask = 0;
        int endCluster = -1;
        uint64_t spotMask = 0;
        int portions = 0;
        int terminal = 0;
        int roadUse = 0;
        vector<int> actions;
    };

    vector<int> candidates = buildBeamSpotCandidates(agent, clusters);
    if (candidates.empty()) return {};
    vector<int> spotCluster(G.spots.size(), -1);
    for (const Cluster& cluster : clusters) {
        for (int sid : cluster.spots) if (sid >= 0 && sid < (int)spotCluster.size()) spotCluster[sid] = cluster.id;
    }
    map<int, DijkstraResult> pathCache;
    auto cachedPaths = [&](int pos) -> const DijkstraResult& {
        auto it = pathCache.find(pos);
        if (it == pathCache.end()) it = pathCache.insert({pos, dijkstraFrom(pos)}).first;
        return it->second;
    };

    auto labelRank = [&](const Label& label) {
        int slack = dayBudget - label.steps;
        int fuelSafe = label.fuel;
        return
            100000000LL * __builtin_popcount((unsigned)label.brandMask) +
            1000000LL * label.portions +
            20000LL * label.terminal +
            1000LL * fuelSafe +
            200LL * slack -
            2500LL * label.roadUse;
    };

    vector<Label> labels;
    Label start;
    start.pos = agent.pos;
    start.fuel = agent.fuel;
    start.endCluster = nearestClusterId(agent.pos, clusters);
    const Spot* startSpot = spotAt(agent.pos);
    if (startSpot && startSpot->id < 63) {
        start.spotMask |= (1ULL << startSpot->id);
        start.portions = 1;
        if (startSpot->brand >= 0 && startSpot->brand < 30) start.brandMask |= (1 << startSpot->brand);
    }
    labels.push_back(start);

    int maxDepth = max(3, min(CURRENT_CONFIG.maxVisits + 1, dayBudget / 7 + 2));
    int labelLimit = isLargeMap() ? 42 : 32;
    if (G.deadlineMode >= 1) labelLimit = min(labelLimit, 18);

    for (int depth = 0; depth < maxDepth; ++depth) {
        if (plannerTimeExceeded(5)) {
            if (PLANNER_STATS.timeoutStage.empty()) PLANNER_STATS.timeoutStage = "patrol_beam";
            break;
        }
        vector<Label> next = labels;
        for (const Label& label : labels) {
            if (plannerTimeExceeded(4)) break;
            PLANNER_STATS.patrolLabels++;
            const DijkstraResult& paths = cachedPaths(label.pos);
            for (int sid : candidates) {
                if ((PLANNER_STATS.patrolLabels & 127) == 0 && plannerTimeExceeded(4)) break;
                if (sid < 0 || sid >= (int)G.spots.size()) continue;
                if (sid < 63 && (label.spotMask & (1ULL << sid))) continue;
                const Spot& spot = G.spots[sid];
                int d = paths.dist[spot.pos];
                int f = paths.fuel[spot.pos];
                if (d >= INF || f >= INF) continue;
                if (label.steps + d > dayBudget || f > label.fuel) continue;
                int slackAfter = dayBudget - label.steps - d;
                if (G.day + 1 < (int)G.daySteps.size() && slackAfter < -CURRENT_CONFIG.minSlack) continue;

                vector<int> path = restorePath(label.pos, spot.pos, paths.prev);
                if (label.pos != spot.pos && path.empty()) continue;

                Label nl = label;
                int cur = label.pos;
                bool ok = true;
                for (int nxt : path) {
                    int dir = dirTo(cur, nxt);
                    MoveCost cost = moveCost(cur);
                    if (dir < 0 || !cost.passable || nl.steps + cost.steps > dayBudget || nl.fuel < cost.fuel) {
                        ok = false;
                        break;
                    }
                    if (terrainAt(cur) == 1) nl.roadUse++;
                    nl.actions.push_back(dir);
                    nl.steps += cost.steps;
                    nl.fuel -= cost.fuel;
                    cur = nxt;
                }
                if (!ok) continue;
                nl.pos = spot.pos;
                nl.endCluster = sid < (int)spotCluster.size() ? spotCluster[sid] : nearestClusterId(spot.pos, clusters);
                if (sid < 63) nl.spotMask |= (1ULL << sid);
                nl.portions++;
                if (spot.brand >= 0 && spot.brand < 30) nl.brandMask |= (1 << spot.brand);
                nl.terminal = (nl.endCluster >= 0 && nl.endCluster < (int)clusters.size()) ? clusters[nl.endCluster].totalStock : 0;
                next.push_back(std::move(nl));
            }
        }

        sort(next.begin(), next.end(), [&](const Label& a, const Label& b) {
            return labelRank(a) > labelRank(b);
        });
        vector<Label> pruned;
        for (const Label& label : next) {
            if ((pruned.size() & 31) == 0 && plannerTimeExceeded(3)) break;
            bool dominated = false;
            for (const Label& kept : pruned) {
                if (kept.pos != label.pos) continue;
                if (kept.endCluster != label.endCluster) continue;
                bool keptCovers = (kept.spotMask | label.spotMask) == kept.spotMask;
                bool keptBrands = (kept.brandMask | label.brandMask) == kept.brandMask;
                if (kept.steps <= label.steps &&
                    kept.fuel >= label.fuel &&
                    keptCovers &&
                    keptBrands &&
                    kept.portions >= label.portions &&
                    kept.terminal >= label.terminal) {
                    dominated = true;
                    PLANNER_STATS.dominancePruned++;
                    break;
                }
            }
            if (!dominated) pruned.push_back(label);
            if ((int)pruned.size() >= labelLimit) break;
        }
        labels.swap(pruned);
    }

    sort(labels.begin(), labels.end(), [&](const Label& a, const Label& b) {
        return labelRank(a) > labelRank(b);
    });
    vector<Route> routes;
    set<string> seen;
    int routeLimit = isLargeMap() ? 8 : 6;
    if (G.deadlineMode >= 1) routeLimit = min(routeLimit, 4);
    for (const Label& label : labels) {
        Route route = simulateRoute(agent, label.actions, dayBudget);
        if (!route.valid || route.stepsUsed <= 0 || route.visitedSpots.empty()) continue;
        ostringstream sig;
        sig << route.endPos << ":";
        for (int sid : route.visitedSpots) sig << sid << ".";
        if (!seen.insert(sig.str()).second) continue;
        routes.push_back(route);
        if ((int)routes.size() >= routeLimit) break;
    }
    return routes;
}

static int roadUseOfRoute(const Route& route) {
    int count = 0;
    for (int src : route.moveSources) {
        if (terrainAt(src) == 1) count++;
    }
    return count;
}

static int explicitWaitOnRoadRisk(const Agent& agent, const vector<int>& actions, int dayBudget) {
    int cur = agent.pos;
    int used = 0;
    int risk = 0;
    for (int action : actions) {
        if (action < 0) {
            int wait = -action;
            if (wait <= 0) break;
            int bounded = min(wait, max(0, dayBudget - used));
            if (terrainAt(cur) == 1) risk += bounded;
            used += bounded;
            if (used >= dayBudget) break;
            continue;
        }
        if (action >= 6 || used >= dayBudget) break;
        int nxt = neighbor(cur, action);
        MoveCost cost = moveCost(cur);
        if (nxt < 0 || terrainAt(nxt) == 3 || !cost.passable) break;
        if (used + cost.steps > dayBudget) break;
        used += cost.steps;
        cur = nxt;
    }
    return risk;
}

static bool routeEndsOnRoad(const Route& route) {
    return inBounds(route.endPos) && terrainAt(route.endPos) == 1;
}

static int conservativeSteps(const Route& route, const vector<int>& plannedRoadUse) {
    int extra = 0;
    for (int src : route.moveSources) {
        if (terrainAt(src) != 1) continue;
        int status = 0;
        auto it = G.traffic.find(src);
        if (it != G.traffic.end()) status = it->second;
        int projected = status + plannedRoadUse[src];
        if (projected >= G.jammedThreshold - 1) extra += isLargeMap() ? 5 : 3;
        else if (projected >= G.busyThreshold - 1) extra += isLargeMap() ? 2 : 1;
        extra += plannedRoadUse[src] / (isLargeMap() ? 2 : 3);
    }
    return route.stepsUsed + extra;
}

static string routeSignature(const RouteCandidate& candidate) {
    vector<int> marked;
    for (int sid : candidate.route.visitedSpots) {
        if (sid < 0 || sid >= (int)G.spots.size()) continue;
        if (G.spots.size() <= 16 || G.spots[sid].amount >= 4) marked.push_back(sid);
    }
    sort(marked.begin(), marked.end());
    marked.erase(unique(marked.begin(), marked.end()), marked.end());
    int bucket = candidate.fuelEnd / max(1, G.fuelLimit / 8);
    int roadBucket = min(9, candidate.roadUse / max(1, G.dayBudget() / 12));
    ostringstream sig;
    sig << candidate.clusterIdEnd << ":" << bucket << ":" << roadBucket << ":" << candidate.mode << ":";
    for (int sid : marked) sig << sid << ".";
    return sig.str();
}

static RouteCandidate makeCandidate(
    const Agent& agent,
    const Route& route,
    int mode,
    int dayBudget,
    const vector<int>& plannedRoadUse,
    const vector<Cluster>& clusters,
    const vector<int>& deficits,
    const vector<Agent>& agents
) {
    RouteCandidate candidate;
    candidate.route = route;
    candidate.mode = mode;
    candidate.roadUse = roadUseOfRoute(route);
    candidate.waitOnRoadRisk = explicitWaitOnRoadRisk(agent, route.actions, dayBudget);
    candidate.endOnRoad = routeEndsOnRoad(route);
    candidate.conservativeSteps = conservativeSteps(route, plannedRoadUse);
    candidate.slack = dayBudget - candidate.conservativeSteps;
    bool finalDay = G.day + 1 >= (int)G.daySteps.size();
    candidate.truncated = !finalDay && candidate.slack < 0;
    candidate.lostVisitsAfterTruncate = candidate.truncated ? (int)route.visitedSpots.size() : 0;
    candidate.terminalValue = estimateTerminalValue(route, clusters);
    candidate.clusterId = nearestClusterId(route.endPos, clusters);
    candidate.clusterIdStart = nearestClusterId(agent.pos, clusters);
    candidate.clusterIdEnd = candidate.clusterId;
    candidate.spotDeficitGain = spotDeficitGainOfRoute(route, deficits);
    candidate.remoteDeficitGain = candidate.spotDeficitGain;
    candidate.futureFuelDebt = estimateFutureFuelDebt(route, clusters);
    candidate.refuelReachable = isRefuelReachable(route, agents, dayBudget);
    auto refuelWindow = refuelWindowForRoute(route, agents, dayBudget);
    candidate.refuelWindowStart = refuelWindow.first;
    candidate.refuelWindowEnd = refuelWindow.first < INF ? dayBudget : -1;
    candidate.refuelWindowPos = refuelWindow.second;
    candidate.minFuelAlongRoute = minFuelAlongRoute(agent, route);
    candidate.fuelEnd = route.fuelLeft;
    candidate.needsRefuel = candidate.fuelEnd < max(G.fuelLimit / 4, LOW_FUEL_ROUTE_LIMIT)
        || candidate.minFuelAlongRoute < max(G.fuelLimit / 8, LOW_FUEL_ROUTE_LIMIT / 2);
    candidate.meetPoint = route.endPos;
    candidate.conservativePortions = (int)route.visitedSpots.size();
    candidate.executedPortions = candidate.truncated ? 0 : candidate.conservativePortions;
    candidate.clusterQuotaGain = 0;
    if (candidate.clusterIdEnd >= 0 && candidate.clusterIdEnd < (int)clusters.size()) {
        candidate.clusterQuotaGain = clusters[candidate.clusterIdEnd].patrolDemand * 120 + clusters[candidate.clusterIdEnd].totalStock * 8;
    }
    if (mode == ROUTE_MODES + 1 && !route.visitedSpots.empty()) {
        candidate.paretoPathUsed = isLargeMap() || CURRENT_CONFIG.fuelRatio < 0.65;
        const Spot& firstSpot = G.spots[route.visitedSpots.front()];
        DijkstraResult fastest = dijkstraFrom(agent.pos);
        candidate.fuelEfficientSteps = route.stepsUsed;
        if (fastest.fuel[firstSpot.pos] < INF) {
            candidate.fuelSavedVsFastest = max(0, fastest.fuel[firstSpot.pos] - route.fuelUsed);
        }
    }
    candidate.brandMask = 0;
    candidate.topSpotMask = 0;
    for (int sid : route.visitedSpots) {
        if (sid < 0 || sid >= (int)G.spots.size()) continue;
        int brand = G.spots[sid].brand;
        if (brand >= 0 && brand < 30) candidate.brandMask |= (1 << brand);
        if (sid < 30) candidate.topSpotMask |= (1 << sid);
    }
    candidate.conservativeScore =
        candidate.executedPortions * 1000 +
        candidate.spotDeficitGain * 12 +
        candidate.remoteDeficitGain * (isLargeMap() ? 16 : 6) +
        candidate.clusterQuotaGain +
        candidate.terminalValue / 12 +
        candidate.slack * 20 +
        candidate.fuelEnd * 2 -
        candidate.futureFuelDebt * 110 -
        30 * candidate.roadUse -
        70 * candidate.waitOnRoadRisk -
        (candidate.endOnRoad && G.day + 1 < (int)G.daySteps.size() ? 180 : 0) -
        (candidate.truncated ? 1000000 : 0) -
        (candidate.needsRefuel && !candidate.refuelReachable ? 1600 : 0);
    candidate.signature = routeSignature(candidate);
    return candidate;
}

static vector<vector<RouteCandidate>> generateRoutePools(const vector<Agent>& agents, int dayBudget, const vector<Cluster>& clusters) {
    vector<vector<RouteCandidate>> pools(agents.size());
    vector<int> emptyRoadUse(G.nodeCount(), 0);
    vector<int> initialStock(G.spots.size(), 0);
    for (size_t i = 0; i < G.spots.size(); ++i) initialStock[i] = max(1, G.spots[i].amount);
    vector<int> deficits = computeSpotDeficits(initialStock);
    vector<DijkstraResult> pathsFromPatrol(agents.size());
    vector<pair<int,int>> remoteTargets;
    for (const Agent& agent : agents) {
        if (agent.kind == 0) pathsFromPatrol[agent.id] = dijkstraFrom(agent.pos);
    }
    for (size_t sid = 0; sid < G.spots.size(); ++sid) {
        int bestDist = INF;
        for (const Agent& other : agents) {
            if (other.kind != 0 || other.id >= (int)pathsFromPatrol.size()) continue;
            bestDist = min(bestDist, pathsFromPatrol[other.id].dist[G.spots[sid].pos]);
        }
        if (bestDist >= INF) continue;
        int stock = max(1, G.spots[sid].amount);
        int score =
            180 * stock +
            120 * spotDebt((int)sid) +
            bestDist +
            (stock >= 3 ? 350 : 0) +
            (isLargeMap() && bestDist > dayBudget / 3 ? 180 : 0);
        remoteTargets.push_back({score, (int)sid});
    }
    sort(remoteTargets.rbegin(), remoteTargets.rend());
    for (const Agent& agent : agents) {
        if (agent.kind != 0) continue;
        if (plannerTimeExceeded(max(5, plannerBudgetMs() / 5))) break;
        for (int mode = 0; mode < adaptiveRouteModeCount(dayBudget); ++mode) {
            if (plannerTimeExceeded(max(5, plannerBudgetMs() / 6))) break;
            Route route = generateRouteForMode(agent, mode, dayBudget, clusters);
            if (route.valid) {
                RouteCandidate candidate = makeCandidate(agent, route, mode, dayBudget, emptyRoadUse, clusters, deficits, agents);
                if (!candidate.truncated || route.stepsUsed == 0) pools[agent.id].push_back(std::move(candidate));
            }
        }

        bool beamUseful = CURRENT_CONFIG.patrolBeamEnabled && !(CURRENT_CONFIG.patrols >= 5 && CURRENT_CONFIG.stockCap <= 27);
        if (beamUseful && !plannerTimeExceeded(max(8, plannerBudgetMs() / 4))) {
            vector<Route> beamRoutes = generateBeamRoutesForAgent(agent, dayBudget, clusters);
            for (const Route& route : beamRoutes) {
                RouteCandidate candidate = makeCandidate(agent, route, ROUTE_MODES + 2, dayBudget, emptyRoadUse, clusters, deficits, agents);
                if (!candidate.truncated || route.stepsUsed == 0) pools[agent.id].push_back(std::move(candidate));
            }
        }

        vector<int> heavyTargets;
        vector<int> byStock(G.spots.size());
        iota(byStock.begin(), byStock.end(), 0);
        sort(byStock.begin(), byStock.end(), [](int a, int b) {
            if (G.spots[a].amount != G.spots[b].amount) return G.spots[a].amount > G.spots[b].amount;
            return G.spots[a].id < G.spots[b].id;
        });
        int heavyLimit = adaptiveHeavyTargetLimit(dayBudget);
        for (int sid : byStock) {
            if ((int)heavyTargets.size() >= heavyLimit) break;
            if (find(heavyTargets.begin(), heavyTargets.end(), sid) == heavyTargets.end()) heavyTargets.push_back(sid);
        }
        for (const Cluster& cluster : clusters) {
            for (int sid : cluster.entrySpots) {
                if ((int)heavyTargets.size() >= heavyLimit + 4) break;
                if (find(heavyTargets.begin(), heavyTargets.end(), sid) == heavyTargets.end()) heavyTargets.push_back(sid);
            }
        }
        int targetLimit = heavyLimit + (isLargeMap() ? 7 : 3);
        if (G.spots.size() <= 12) targetLimit = (int)G.spots.size();
        for (int sid : topDebtSpotIds(isLargeMap() ? 4 : 3)) {
            if ((int)heavyTargets.size() >= targetLimit) break;
            if (find(heavyTargets.begin(), heavyTargets.end(), sid) == heavyTargets.end()) heavyTargets.push_back(sid);
        }
        for (const auto& item : remoteTargets) {
            if ((int)heavyTargets.size() >= targetLimit) break;
            int sid = item.second;
            if (find(heavyTargets.begin(), heavyTargets.end(), sid) == heavyTargets.end()) heavyTargets.push_back(sid);
        }
        for (int sid : heavyTargets) {
            if (plannerTimeExceeded(max(5, plannerBudgetMs() / 7))) break;
            Route route = generateRouteToHeavySpot(agent, sid, dayBudget, clusters);
            if (route.valid && route.stepsUsed > 0) {
                RouteCandidate candidate = makeCandidate(agent, route, ROUTE_MODES + 1, dayBudget, emptyRoadUse, clusters, deficits, agents);
                if (!candidate.truncated) pools[agent.id].push_back(std::move(candidate));
            }
        }

        Route wait = simulateRoute(agent, {}, dayBudget);
        pools[agent.id].push_back(makeCandidate(agent, wait, ROUTE_MODES, dayBudget, emptyRoadUse, clusters, deficits, agents));
        sort(pools[agent.id].begin(), pools[agent.id].end(), [](const RouteCandidate& a, const RouteCandidate& b) {
            if (a.conservativeScore != b.conservativeScore) return a.conservativeScore > b.conservativeScore;
            return a.slack > b.slack;
        });
        vector<RouteCandidate> pruned;
        set<string> seenSignatures;
        set<int> endpointClustersKept;
        bool keptDebt = false;
        bool keptBrandSafe = false;
        bool keptFuelSafe = false;
        bool keptRoadLight = false;
        for (const RouteCandidate& candidate : pools[agent.id]) {
            string sig = candidate.signature.empty() ? routeSignature(candidate) : candidate.signature;
            bool signatureNew = seenSignatures.insert(sig).second || candidate.route.stepsUsed == 0;
            bool diversityNeed =
                (candidate.spotDeficitGain > 0 && !keptDebt) ||
                (candidate.brandMask && !keptBrandSafe) ||
                (candidate.fuelEnd >= max(G.fuelLimit / 2, LOW_FUEL_ROUTE_LIMIT) && !keptFuelSafe) ||
                (candidate.mode == 16 && !keptRoadLight) ||
                (candidate.clusterIdEnd >= 0 && endpointClustersKept.insert(candidate.clusterIdEnd).second && isLargeMap());
            if (signatureNew || diversityNeed) {
                pruned.push_back(candidate);
                if (candidate.spotDeficitGain > 0) keptDebt = true;
                if (candidate.brandMask) keptBrandSafe = true;
                if (candidate.fuelEnd >= max(G.fuelLimit / 2, LOW_FUEL_ROUTE_LIMIT)) keptFuelSafe = true;
                if (candidate.mode == 16) keptRoadLight = true;
            }
            if ((int)pruned.size() >= adaptivePoolLimit(dayBudget)) break;
        }
        pools[agent.id].swap(pruned);
    }
    return pools;
}

struct MarginalEval {
    int brandGain = 0;
    int portions = 0;
    int highStockDeficit = 0;
    int remoteDeficitGain = 0;
    int clusterBalance = 0;
    int clusterQuotaGain = 0;
    int slack = -INF;
    int fuelLeft = 0;
    int fuelRisk = 0;
    int terminalValue = 0;
    int futureFuelDebt = 0;
    int roadPenalty = 0;
    long long rank = -1;
};

static MarginalEval evaluateCandidate(
    const RouteCandidate& candidate,
    const set<int>& dailyBrands,
    const vector<int>& remainingStock,
    const vector<int>& plannedRoadUse,
    int dayBudget
) {
    MarginalEval eval;
    vector<int> rem = remainingStock;
    set<int> brands = dailyBrands;
    vector<int> deficits = computeSpotDeficits(remainingStock);
    set<int> routeClusters;
    for (int sid : candidate.route.visitedSpots) {
        if (sid < 0 || sid >= (int)G.spots.size()) continue;
        if (rem[sid] <= 0) continue;
        int beforeDeficit = sid < (int)deficits.size() ? deficits[sid] : 0;
        rem[sid]--;
        eval.portions++;
        if (beforeDeficit > 0) eval.highStockDeficit += (60 + 15 * max(1, G.spots[sid].amount));
        if (!brands.count(G.spots[sid].brand)) {
            brands.insert(G.spots[sid].brand);
            eval.brandGain++;
        }
        if (candidate.clusterIdEnd >= 0) routeClusters.insert(candidate.clusterIdEnd);
    }
    eval.clusterBalance = (int)routeClusters.size();
    eval.remoteDeficitGain = candidate.remoteDeficitGain;
    eval.clusterQuotaGain = candidate.clusterQuotaGain;
    eval.roadPenalty = 0;
    for (int src : candidate.route.moveSources) {
        if (terrainAt(src) != 1) continue;
        eval.roadPenalty += 1 + plannedRoadUse[src];
    }
    eval.roadPenalty += 3 * candidate.waitOnRoadRisk;
    if (candidate.endOnRoad && G.day + 1 < (int)G.daySteps.size()) eval.roadPenalty += max(2, dayBudget / 12);
    eval.slack = dayBudget - conservativeSteps(candidate.route, plannedRoadUse);
    eval.fuelLeft = candidate.route.fuelLeft;
    eval.terminalValue = candidate.terminalValue;
    eval.futureFuelDebt = candidate.futureFuelDebt;
    int softFuel = max(G.fuelLimit / 5, LOW_FUEL_ROUTE_LIMIT);
    int hardFuel = max(G.fuelLimit / 10, 8);
    eval.fuelRisk = max(0, softFuel - candidate.fuelEnd) + 2 * max(0, hardFuel - candidate.minFuelAlongRoute);
    if (G.day + 1 < (int)G.daySteps.size() && candidate.fuelEnd <= 10 && !candidate.refuelReachable && eval.brandGain == 0) {
        eval.fuelRisk += 200;
    }
    if (candidate.route.stepsUsed == 0 && eval.portions <= 0) {
        eval.rank = 0;
        return eval;
    }
    bool finalDay = G.day + 1 >= (int)G.daySteps.size();
    if ((!finalDay && eval.slack < 0) || eval.portions <= 0) return eval;
    if (isLargeMap() && G.day + 1 < (int)G.daySteps.size() && eval.brandGain == 0) {
        int minSlack = minSlackForRoute(dayBudget);
        if (eval.slack <= 0 && candidate.roadUse >= 8) return eval;
        eval.fuelRisk += max(0, minSlack - eval.slack) * (candidate.roadUse >= 8 ? 8 : 3);
    }
    eval.rank =
        1000000000LL * eval.brandGain +
        1000000LL * eval.portions +
        20000LL * eval.remoteDeficitGain +
        2500LL * eval.highStockDeficit +
        3000LL * eval.clusterQuotaGain +
        75000LL * eval.clusterBalance +
        2500LL * max(0, eval.slack) +
        20LL * eval.fuelLeft +
        15LL * eval.terminalValue -
        15000LL * eval.futureFuelDebt -
        ((isLargeMap() && G.day + 1 < (int)G.daySteps.size()) ? 18000LL : 5000LL) * eval.fuelRisk -
        ((G.daySeconds > 0 && G.daySeconds <= 2) ? 2200LL : 1500LL) * eval.roadPenalty -
        5000LL * max(0, requiredSlack(dayBudget) - eval.slack);
    return eval;
}

static vector<Route> combineRoutesStockAware(const vector<Agent>& agents, const vector<vector<RouteCandidate>>& pools, int dayBudget) {
    struct BeamState {
        vector<Route> selected;
        vector<int> remainingStock;
        vector<int> plannedRoadUse;
        vector<int> endpointClusterUse;
        set<int> dailyBrands;
        TeamPlanEval eval;
        long long rank = 0;
    };

    auto recomputeRank = [dayBudget](const TeamPlanEval& e) {
        long long heuristic =
            30000LL * e.remoteDeficitGain +
            5000LL * e.highStockDeficit +
            30000LL * e.clusterQuotaGain +
            50000LL * e.clusterBalance +
            200LL * e.terminalValue -
            ((isLargeMap() && G.day + 1 < (int)G.daySteps.size()) ? 1000000LL : 300000LL) * e.fuelRisk -
            100000LL * e.roadPenalty -
            1000LL * e.responseCost;
        heuristic = max(-400000000LL, min(400000000LL, heuristic));
        return
            1000000000000LL * e.dailyBrands +
            1000000000LL * e.cappedPortions +
            500000000LL + heuristic;
    };

    BeamState initial;
    initial.selected.resize(agents.size());
    initial.remainingStock.assign(G.spots.size(), 0);
    initial.plannedRoadUse.assign(G.nodeCount(), 0);
    int clusterSlots = 1;
    for (const vector<RouteCandidate>& pool : pools) {
        for (const RouteCandidate& candidate : pool) clusterSlots = max(clusterSlots, candidate.clusterIdEnd + 1);
    }
    initial.endpointClusterUse.assign(max(1, clusterSlots), 0);
    for (size_t i = 0; i < G.spots.size(); ++i) initial.remainingStock[i] = max(1, G.spots[i].amount);

    vector<BeamState> beam(1, initial);
    vector<int> patrolIds;
    for (const Agent& agent : agents) if (agent.kind == 0) patrolIds.push_back(agent.id);
    sort(patrolIds.begin(), patrolIds.end(), [&](int a, int b) {
        int besta = pools[a].empty() ? -INF : pools[a][0].conservativeScore;
        int bestb = pools[b].empty() ? -INF : pools[b][0].conservativeScore;
        return besta > bestb;
    });

    int beamWidth = adaptiveBeamWidth(dayBudget);
    for (int agentId : patrolIds) {
        if (plannerTimeExceeded(max(8, plannerBudgetMs() / 5))) break;
        vector<BeamState> next;
        const vector<RouteCandidate>& choices = pools[agentId];
        for (const BeamState& state : beam) {
            if (plannerTimeExceeded(max(6, plannerBudgetMs() / 8))) break;
            for (const RouteCandidate& choice : choices) {
                if (plannerTimeExceeded(max(6, plannerBudgetMs() / 10))) break;
                MarginalEval delta = evaluateCandidate(choice, state.dailyBrands, state.remainingStock, state.plannedRoadUse, dayBudget);
                PLANNER_STATS.teamStates++;
                if (choice.route.stepsUsed > 0 && delta.rank <= 0) continue;

                BeamState ns = state;
                ns.selected[agentId] = choice.route;
                ns.eval.terminalValue += max(0, delta.terminalValue);
                ns.eval.highStockDeficit += delta.highStockDeficit;
                ns.eval.remoteDeficitGain += delta.remoteDeficitGain;
                ns.eval.clusterBalance += delta.clusterBalance;
                ns.eval.clusterQuotaGain += delta.clusterQuotaGain;
                ns.eval.fuelRisk += delta.fuelRisk;
                int debtWeight = ((int)G.daySteps.size() >= 6 && G.day + 1 < (int)G.daySteps.size()) ? 2 : 1;
                ns.eval.fuelRisk += debtWeight * delta.futureFuelDebt;
                ns.eval.roadPenalty += delta.roadPenalty;
                if (choice.clusterIdEnd >= 0 && choice.clusterIdEnd < (int)ns.endpointClusterUse.size()) {
                    int alreadyThere = ns.endpointClusterUse[choice.clusterIdEnd]++;
                    if (alreadyThere > 0 && choice.route.stepsUsed > 0 && delta.remoteDeficitGain <= 0) {
                        ns.eval.roadPenalty += isLargeMap() ? 8 * alreadyThere : 4 * alreadyThere;
                    }
                }
                if (G.day + 1 < (int)G.daySteps.size()) {
                    ns.eval.responseCost += max(0, requiredSlack(dayBudget) - delta.slack);
                    if (choice.route.stepsUsed >= dayBudget - 2 && delta.portions <= 1) ns.eval.responseCost += 5;
                }

                for (int sid : choice.route.visitedSpots) {
                    if (sid < 0 || sid >= (int)G.spots.size() || ns.remainingStock[sid] <= 0) continue;
                    ns.remainingStock[sid]--;
                    ns.eval.cappedPortions++;
                    ns.eval.official.cappedPortions = ns.eval.cappedPortions;
                    if (!ns.dailyBrands.count(G.spots[sid].brand)) {
                        ns.dailyBrands.insert(G.spots[sid].brand);
                        ns.eval.dailyBrands = (int)ns.dailyBrands.size();
                        ns.eval.official.dailyBrands = ns.eval.dailyBrands;
                    }
                }
                for (int src : choice.route.moveSources) {
                    if (terrainAt(src) == 1) ns.plannedRoadUse[src]++;
                }
                ns.rank = recomputeRank(ns.eval);
                next.push_back(std::move(ns));
            }
        }
        if (next.empty()) next = beam;
        sort(next.begin(), next.end(), [](const BeamState& a, const BeamState& b) {
            return a.rank > b.rank;
        });
        vector<BeamState> pruned;
        for (BeamState& state : next) {
            if ((pruned.size() & 31) == 0 && plannerTimeExceeded(3)) break;
            bool duplicate = false;
            for (const BeamState& kept : pruned) {
                if (kept.eval.dailyBrands == state.eval.dailyBrands &&
                    kept.eval.cappedPortions == state.eval.cappedPortions &&
                    kept.eval.highStockDeficit >= state.eval.highStockDeficit &&
                    kept.eval.remoteDeficitGain >= state.eval.remoteDeficitGain &&
                    kept.eval.fuelRisk <= state.eval.fuelRisk &&
                    kept.eval.terminalValue >= state.eval.terminalValue) {
                    duplicate = true;
                    PLANNER_STATS.dominancePruned++;
                    break;
                }
            }
            if (!duplicate) pruned.push_back(std::move(state));
            if ((int)pruned.size() >= beamWidth) break;
        }
        beam.swap(pruned);
    }

    auto betterTeamState = [](const BeamState& a, const BeamState& b) {
        if (a.eval.dailyBrands != b.eval.dailyBrands) return a.eval.dailyBrands > b.eval.dailyBrands;
        if (a.eval.cappedPortions != b.eval.cappedPortions) return a.eval.cappedPortions > b.eval.cappedPortions;
        if (a.eval.remoteDeficitGain != b.eval.remoteDeficitGain) return a.eval.remoteDeficitGain > b.eval.remoteDeficitGain;
        if (a.eval.highStockDeficit != b.eval.highStockDeficit) return a.eval.highStockDeficit > b.eval.highStockDeficit;
        if (a.eval.clusterQuotaGain != b.eval.clusterQuotaGain) return a.eval.clusterQuotaGain > b.eval.clusterQuotaGain;
        if (a.eval.fuelRisk != b.eval.fuelRisk) return a.eval.fuelRisk < b.eval.fuelRisk;
        if (a.eval.terminalValue != b.eval.terminalValue) return a.eval.terminalValue > b.eval.terminalValue;
        if (a.eval.responseCost != b.eval.responseCost) return a.eval.responseCost < b.eval.responseCost;
        return a.rank > b.rank;
    };

    auto buildGreedyState = [&]() {
        BeamState greedy = initial;
        vector<bool> assigned(agents.size(), false);
        for (size_t iter = 0; iter < patrolIds.size(); ++iter) {
            if (plannerTimeExceeded(4)) break;
            int bestAgent = -1;
            int bestChoice = -1;
            MarginalEval bestEval;
            for (int agentId : patrolIds) {
                if (plannerTimeExceeded(3)) break;
                if (assigned[agentId]) continue;
                for (size_t ci = 0; ci < pools[agentId].size(); ++ci) {
                    if (plannerTimeExceeded(3)) break;
                    MarginalEval delta = evaluateCandidate(pools[agentId][ci], greedy.dailyBrands, greedy.remainingStock, greedy.plannedRoadUse, dayBudget);
                    if (delta.rank > bestEval.rank) {
                        bestEval = delta;
                        bestAgent = agentId;
                        bestChoice = (int)ci;
                    }
                }
            }
            if (bestAgent < 0 || bestEval.rank <= 0) break;
            const RouteCandidate& choice = pools[bestAgent][bestChoice];
            assigned[bestAgent] = true;
            greedy.selected[bestAgent] = choice.route;
            greedy.eval.terminalValue += max(0, bestEval.terminalValue);
            greedy.eval.highStockDeficit += bestEval.highStockDeficit;
            greedy.eval.remoteDeficitGain += bestEval.remoteDeficitGain;
            greedy.eval.clusterBalance += bestEval.clusterBalance;
            greedy.eval.clusterQuotaGain += bestEval.clusterQuotaGain;
            greedy.eval.fuelRisk += bestEval.fuelRisk;
            int debtWeight = ((int)G.daySteps.size() >= 6 && G.day + 1 < (int)G.daySteps.size()) ? 2 : 1;
            greedy.eval.fuelRisk += debtWeight * bestEval.futureFuelDebt;
            greedy.eval.roadPenalty += bestEval.roadPenalty;
            if (choice.clusterIdEnd >= 0 && choice.clusterIdEnd < (int)greedy.endpointClusterUse.size()) {
                int alreadyThere = greedy.endpointClusterUse[choice.clusterIdEnd]++;
                if (alreadyThere > 0 && choice.route.stepsUsed > 0 && bestEval.remoteDeficitGain <= 0) {
                    greedy.eval.roadPenalty += isLargeMap() ? 8 * alreadyThere : 4 * alreadyThere;
                }
            }
            if (G.day + 1 < (int)G.daySteps.size()) {
                greedy.eval.responseCost += max(0, requiredSlack(dayBudget) - bestEval.slack);
                if (choice.route.stepsUsed >= dayBudget - 2 && bestEval.portions <= 1) greedy.eval.responseCost += 5;
            }
            for (int sid : choice.route.visitedSpots) {
                if (sid < 0 || sid >= (int)G.spots.size() || greedy.remainingStock[sid] <= 0) continue;
                greedy.remainingStock[sid]--;
                greedy.eval.cappedPortions++;
                greedy.eval.official.cappedPortions = greedy.eval.cappedPortions;
                if (!greedy.dailyBrands.count(G.spots[sid].brand)) {
                    greedy.dailyBrands.insert(G.spots[sid].brand);
                    greedy.eval.dailyBrands = (int)greedy.dailyBrands.size();
                    greedy.eval.official.dailyBrands = greedy.eval.dailyBrands;
                }
            }
            for (int src : choice.route.moveSources) {
                if (terrainAt(src) == 1) greedy.plannedRoadUse[src]++;
            }
        }
        greedy.rank = recomputeRank(greedy.eval);
        return greedy;
    };

    BeamState best = beam.empty() ? initial : beam.front();
    BeamState greedy = buildGreedyState();
    if (betterTeamState(greedy, best)) {
        if (!SUPPRESS_PLAN_LOGS) cerr << "combiner day=" << G.day << " selected=greedy\n";
        best = std::move(greedy);
    } else {
        if (!SUPPRESS_PLAN_LOGS) cerr << "combiner day=" << G.day << " selected=beam\n";
    }
    vector<Route> selected = best.selected;
    for (const Agent& agent : agents) {
        if (agent.kind != 0) continue;
        if (!selected[agent.id].valid) selected[agent.id] = simulateRoute(agent, {}, dayBudget);
        if (!SUPPRESS_PLAN_LOGS) {
            cerr << "patrol day=" << G.day
                 << " id=" << agent.id
                 << " start=" << agent.pos
                 << " fuel=" << agent.fuel
                 << " actions=" << selected[agent.id].actions.size()
                 << " steps=" << selected[agent.id].stepsUsed
                 << " visits=" << selected[agent.id].visitedSpots.size()
                 << " end=" << selected[agent.id].endPos
                 << " fuel_end=" << selected[agent.id].fuelLeft
                 << " slack=" << (dayBudget - conservativeSteps(selected[agent.id], best.plannedRoadUse))
                 << " road_use=" << roadUseOfRoute(selected[agent.id])
                 << "\n";
        }
    }

    int assignedVisits = best.eval.cappedPortions;
    int optimisticVisits = 0;
    for (const Agent& agent : agents) {
        if (agent.kind != 0 || agent.id >= (int)selected.size() || !selected[agent.id].valid) continue;
        optimisticVisits += (int)selected[agent.id].visitedSpots.size();
    }
    int ghostVisits = max(0, optimisticVisits - assignedVisits);
    int cap = stockCap();
    int conservativeEst = max(0, assignedVisits - best.eval.roadPenalty / 18 - best.eval.fuelRisk / 120);
    int selectedWaitOnRoad = 0;
    int selectedEndOnRoad = 0;
    for (const Agent& agent : agents) {
        if (agent.kind != 0 || agent.id >= (int)selected.size() || !selected[agent.id].valid) continue;
        selectedWaitOnRoad += explicitWaitOnRoadRisk(agent, selected[agent.id].actions, dayBudget);
        if (routeEndsOnRoad(selected[agent.id])) selectedEndOnRoad++;
    }
    vector<pair<int,int>> missingBySpot;
    for (size_t i = 0; i < G.spots.size(); ++i) {
        int missing = max(0, best.remainingStock[i]);
        if (missing > 0) missingBySpot.push_back({missing * max(1, G.spots[i].amount), (int)i});
    }
    sort(missingBySpot.rbegin(), missingBySpot.rend());
    if (!SUPPRESS_PLAN_LOGS) {
        cerr << "daily_summary day=" << G.day
             << " active_patrols=" << count_if(agents.begin(), agents.end(), [&](const Agent& a) {
                    return a.kind == 0 && selected[a.id].stepsUsed > 0;
                })
             << " daily_brands=" << best.eval.dailyBrands
             << " assigned_stock=" << assignedVisits
             << " assigned_visits=" << optimisticVisits
             << " ghost_visits=" << ghostVisits
             << " static_est=" << assignedVisits
             << " conservative_est=" << conservativeEst
             << " server_est=" << conservativeEst
             << " stock_cap=" << cap
             << " capture_ratio=" << (cap ? (100 * assignedVisits / cap) : 0)
             << " assignment_coverage=" << (cap ? (100 * assignedVisits / cap) : 0)
             << " execution_efficiency=" << (optimisticVisits ? (100 * conservativeEst / optimisticVisits) : 100)
             << " effective_capture=" << (cap ? (100 * conservativeEst / cap) : 0)
             << " high_deficit=" << best.eval.highStockDeficit
             << " remote_deficit=" << best.eval.remoteDeficitGain
             << " cluster_quota=" << best.eval.clusterQuotaGain
             << " fuel_risk=" << best.eval.fuelRisk
             << " wait_on_road=" << selectedWaitOnRoad
             << " end_on_road=" << selectedEndOnRoad
             << " terminal=" << best.eval.terminalValue
             << " low_fuel_patrols=" << count_if(agents.begin(), agents.end(), [](const Agent& a) {
                    return a.kind == 0 && a.fuel <= max(LOW_FUEL_ROUTE_LIMIT, G.fuelLimit / 5);
                })
             << " top_missing=";
        for (int i = 0; i < (int)missingBySpot.size() && i < 4; ++i) {
            int sid = missingBySpot[i].second;
            if (i) cerr << ",";
            cerr << G.spots[sid].pos << ":" << best.remainingStock[sid];
        }
        cerr << "\n";
    }
    return selected;
}

static vector<RefuelRequest> makeRefuelRequests(const vector<Agent>& agents, const vector<Route>& routes, const vector<Cluster>& clusters, int dayBudget) {
    vector<RefuelRequest> requests;
    int softFuel = max(G.fuelLimit / 4, LOW_FUEL_ROUTE_LIMIT);
    int hardFuel = max(G.fuelLimit / 8, 8);
    for (const Agent& agent : agents) {
        if (agent.kind != 0) continue;
        const Route& route = routes[agent.id];
        if (!route.valid) continue;
        int minFuel = minFuelAlongRoute(agent, route);
        int fuelEnd = route.fuelLeft;
        int risk = max(0, softFuel - fuelEnd) + 2 * max(0, hardFuel - minFuel);
        if (risk <= 0 && agent.fuel > softFuel) continue;

        RefuelRequest request;
        request.patrolId = agent.id;
        request.routeId = agent.id;
        request.earliestStep = max(0, route.stepsUsed / 2);
        request.latestStep = dayBudget;
        request.serviceZone = nearestClusterId(route.endPos, clusters);
        if (request.serviceZone >= 0 && request.serviceZone < (int)clusters.size()) {
            request.lostFutureStock = clusters[request.serviceZone].totalStock;
            request.meetPositions.push_back(clusters[request.serviceZone].center);
            for (int sid : clusters[request.serviceZone].entrySpots) request.meetPositions.push_back(G.spots[sid].pos);
        }
        vector<OccupancyInterval> intervals = buildOccupancyIntervals(route);
        for (const OccupancyInterval& interval : intervals) {
            if (interval.endStep - interval.startStep < 1) continue;
            if (interval.endStep < request.earliestStep || interval.startStep > request.latestStep) continue;
            request.meetPositions.push_back(interval.pos);
        }
        request.meetPositions.push_back(route.endPos);
        request.meetPositions.push_back(agent.pos);
        bool finalDay = G.day + 1 >= (int)G.daySteps.size();
        request.mustServe = !finalDay && (route.fuelLeft <= 10 || agent.fuel <= hardFuel);
        request.priority =
            1000 * risk +
            80 * request.lostFutureStock +
            max(0, dayBudget - route.stepsUsed) +
            (route.visitedSpots.empty() ? 300 : 0) +
            (request.mustServe ? 200000 : 0);
        requests.push_back(request);
    }
    sort(requests.begin(), requests.end(), [](const RefuelRequest& a, const RefuelRequest& b) {
        return a.priority > b.priority;
    });
    return requests;
}

static int chooseRefuelServiceZone(const vector<RefuelRequest>& requests, const vector<Cluster>& clusters) {
    if (clusters.empty()) return -1;
    vector<long long> zoneScore(clusters.size(), 0);
    for (const RefuelRequest& request : requests) {
        if (request.serviceZone >= 0 && request.serviceZone < (int)clusters.size()) {
            zoneScore[request.serviceZone] += request.priority + 100LL * request.lostFutureStock;
        }
    }
    for (const Cluster& cluster : clusters) {
        long long clusterDebt = 0;
        for (int sid : cluster.spots) clusterDebt += spotDebt(sid) * (100 + 20 * max(1, G.spots[sid].amount));
        zoneScore[cluster.id] += 1000LL * cluster.totalStock + 18LL * clusterDebt - 20LL * cluster.avgDistanceFromStarts;
    }
    int best = 0;
    for (int i = 1; i < (int)zoneScore.size(); ++i) {
        if (zoneScore[i] > zoneScore[best]) best = i;
    }
    return best;
}

static Route planRefuelRoute(
    const Agent& refuel,
    const vector<RefuelRequest>& requests,
    const vector<Route>& patrolRoutes,
    const vector<Cluster>& clusters,
    int dayBudget
) {
    struct RefuelTarget {
        int pos = -1;
        long long priority = 0;
        int latestStep = 0;
    };
    vector<RefuelTarget> targets;
    bool hasFuelRequest = any_of(requests.begin(), requests.end(), [](const RefuelRequest& request) {
        return request.mustServe || request.priority >= 200000;
    });
    int serviceZone = chooseRefuelServiceZone(requests, clusters);
    if (serviceZone >= 0 && serviceZone < (int)clusters.size()) {
        long long zoneDebt = 0;
        for (int sid : clusters[serviceZone].spots) zoneDebt += spotDebt(sid) * (100 + 20 * max(1, G.spots[sid].amount));
        long long zonePriority = 1400000LL + 1000LL * clusters[serviceZone].totalStock + 24LL * zoneDebt;
        targets.push_back({clusters[serviceZone].center, zonePriority, dayBudget});
        for (int sid : clusters[serviceZone].entrySpots) {
            targets.push_back({G.spots[sid].pos, zonePriority + 500LL * G.spots[sid].amount + 900LL * spotDebt(sid), dayBudget});
        }
    }
    for (const RefuelRequest& request : requests) {
        set<int> seenPositions;
        for (int pos : request.meetPositions) {
            if (inBounds(pos) && seenPositions.insert(pos).second) {
                targets.push_back({pos, 1000000LL + request.priority, request.latestStep});
            }
        }
    }
    if (hasFuelRequest) {
        for (int sid : topDebtSpotIds(4)) {
            targets.push_back({G.spots[sid].pos, 1250000LL + 4500LL * spotDebt(sid) + 500LL * max(1, G.spots[sid].amount), dayBudget});
        }
    }
    for (const Cluster& cluster : clusters) {
        long long clusterDebt = 0;
        for (int sid : cluster.spots) clusterDebt += spotDebt(sid) * (100 + 20 * max(1, G.spots[sid].amount));
        targets.push_back({cluster.center, 200000LL + 1000LL * cluster.totalStock + 14LL * clusterDebt, dayBudget});
    }
    if (targets.empty()) return simulateRoute(refuel, {}, dayBudget);

    DijkstraResult paths = dijkstraFrom(refuel.pos);
    int bestPos = -1;
    long long bestRank = -1;
    for (size_t i = 0; i < targets.size(); ++i) {
        if ((i & 15) == 0 && plannerTimeExceeded(2)) break;
        int pos = targets[i].pos;
        int d = paths.dist[pos];
        if (d >= INF || d > dayBudget || d > targets[i].latestStep) continue;
        Spot pseudo;
        pseudo.id = -1;
        pseudo.pos = pos;
        pseudo.brand = -1;
        pseudo.amount = 0;
        Route candidateRoute = routeToSpot(refuel, pseudo, dayBudget);
        long long servedPriority = 0;
        int served = 0;
        for (const RefuelRequest& request : requests) {
            if (request.patrolId < 0 || request.patrolId >= (int)patrolRoutes.size()) continue;
            int eventStep = -1;
            int eventPos = -1;
            if (overlapForRefuel(patrolRoutes[request.patrolId], candidateRoute, eventStep, eventPos)) {
                served++;
                servedPriority += request.priority;
            }
        }
        long long priority = targets[i].priority;
        long long rank =
            100000000LL * served +
            1000LL * servedPriority +
            priority -
            1000LL * d +
            30LL * (targets[i].latestStep - d);
        if (rank > bestRank) {
            bestRank = rank;
            bestPos = pos;
        }
    }
    if (bestPos < 0) return simulateRoute(refuel, {}, dayBudget);

    Spot pseudo;
    pseudo.id = -1;
    pseudo.pos = bestPos;
    pseudo.brand = -1;
    pseudo.amount = 0;
    return routeToSpot(refuel, pseudo, dayBudget);
}

static vector<vector<int>> combineRoutesDiversityFirst(const vector<Agent>& agents, const vector<Route>& routes, const vector<Cluster>& clusters, int dayBudget) {
    vector<vector<int>> plan(agents.size());
    vector<RefuelRequest> requests = makeRefuelRequests(agents, routes, clusters, dayBudget);

    for (const Agent& agent : agents) {
        if (agent.kind == 0) {
            const Route& r = routes[agent.id];
            plan[agent.id] = r.valid ? r.actions : vector<int>();
        }
    }

    for (const Agent& agent : agents) {
        if (agent.kind != 1) continue;
        Route refuelRoute = planRefuelRoute(agent, requests, routes, clusters, dayBudget);
        plan[agent.id] = refuelRoute.valid ? refuelRoute.actions : vector<int>();
        bool hasUrgentRequest = any_of(requests.begin(), requests.end(), [](const RefuelRequest& request) {
            return request.mustServe || request.priority >= 200000;
        });
        bool endsNearDebt = false;
        if (refuelRoute.stepsUsed > 0) {
            DijkstraResult fromEnd = dijkstraFrom(refuelRoute.endPos);
            for (int sid : topDebtSpotIds(4)) {
                if (fromEnd.dist[G.spots[sid].pos] <= max(4, dayBudget / 8)) {
                    endsNearDebt = true;
                    break;
                }
            }
        }
        string tankerReason = hasUrgentRequest ? "low_fuel" : (refuelRoute.stepsUsed > 0 ? (endsNearDebt ? "debt_cluster" : "stock_cluster") : "idle");
        if (tankerReason == "idle") G.tankerIdleStreak++;
        else G.tankerIdleStreak = 0;
        cerr << "refuel day=" << G.day
             << " id=" << agent.id
             << " start=" << agent.pos
             << " fuel=" << agent.fuel
             << " target=" << refuelRoute.endPos
             << " actions=" << refuelRoute.actions.size()
             << " requests=" << requests.size()
             << " service_zone=" << chooseRefuelServiceZone(requests, clusters)
             << " tanker_reason=" << tankerReason
             << " tanker_idle_streak=" << G.tankerIdleStreak
             << "\n";
    }

    return normalizePlan(plan, agents, dayBudget);
}

static vector<vector<int>> scheduleRefuelForPlan(
    const vector<vector<int>>& inputPlan,
    const vector<Agent>& agents,
    const vector<Cluster>& clusters,
    int dayBudget
) {
    vector<vector<int>> plan = normalizePlan(inputPlan, agents, dayBudget);
    vector<Route> routes(agents.size());
    for (size_t i = 0; i < agents.size(); ++i) routes[i] = simulateRoute(agents[i], plan[i], dayBudget);
    vector<RefuelRequest> requests = makeRefuelRequests(agents, routes, clusters, dayBudget);
    for (const Agent& agent : agents) {
        if (agent.kind != 1) continue;
        Route tanker = planRefuelRoute(agent, requests, routes, clusters, dayBudget);
        plan[agent.id] = tanker.valid ? tanker.actions : vector<int>();
    }
    plan = normalizePlan(plan, agents, dayBudget);
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    set<int> served;
    for (const RefuelEvent& event : team.refuels) served.insert(event.patrolId);
    int tankerEnd = -1;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind == 1 && i < team.routes.size()) {
            tankerEnd = team.routes[i].endPos;
            break;
        }
    }
    bool repaired = false;
    if (tankerEnd >= 0) {
        for (const RefuelRequest& request : requests) {
            if (!request.mustServe || served.count(request.patrolId)) continue;
            if (request.patrolId < 0 || request.patrolId >= (int)agents.size()) continue;
            Spot meet;
            meet.id = -1;
            meet.pos = tankerEnd;
            meet.brand = -1;
            meet.amount = 0;
            Route repair = routeToSpot(agents[request.patrolId], meet, dayBudget);
            if (!repair.valid || repair.endPos != tankerEnd) continue;
            plan[request.patrolId] = repair.actions;
            repaired = true;
        }
    }
    if (repaired) {
        plan = normalizePlan(plan, agents, dayBudget);
        team = simulateTeamPlan(plan, agents, dayBudget);
    }
    return plan;
}

static void logFinalRefuelTimeline(
    const vector<vector<int>>& plan,
    const vector<Agent>& agents,
    const vector<Cluster>& clusters,
    int dayBudget
) {
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    vector<RefuelRequest> requests;
    if (team.valid) requests = makeRefuelRequests(agents, team.routes, clusters, dayBudget);
    int mustServe = 0;
    for (const RefuelRequest& request : requests) if (request.mustServe) mustServe++;
    set<int> served;
    for (const RefuelEvent& event : team.refuels) served.insert(event.patrolId);
    int servedMust = 0;
    for (const RefuelRequest& request : requests) {
        if (request.mustServe && served.count(request.patrolId)) servedMust++;
    }
    bool hasTanker = any_of(agents.begin(), agents.end(), [](const Agent& agent) { return agent.kind == 1; });
    cerr << "refuel_timeline day=" << G.day
         << " planned=" << requests.size()
         << " must_serve=" << mustServe
         << " feasible=" << team.refuels.size()
         << " failed=" << max(0, mustServe - servedMust)
         << " planned_refuels=" << requests.size()
         << " feasible_refuels=" << team.refuels.size()
         << " actual_refuels=" << team.refuels.size()
         << " tanker_idle=" << (hasTanker && team.refuels.empty() ? 1 : 0)
         << "\n";
}

static void updateSpotMissDebtFromPlan(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    if (G.spotMissDebt.size() != G.spots.size()) G.spotMissDebt.assign(G.spots.size(), 0);
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    if (!team.valid) return;
    vector<int> remaining(G.spots.size(), 0);
    vector<int> served(G.spots.size(), 0);
    for (size_t sid = 0; sid < G.spots.size(); ++sid) remaining[sid] = max(1, G.spots[sid].amount);
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0 || i >= team.routes.size()) continue;
        for (int sid : team.routes[i].visitedSpots) {
            if (sid < 0 || sid >= (int)G.spots.size()) continue;
            served[sid]++;
            if (remaining[sid] > 0) remaining[sid]--;
        }
    }
    vector<pair<int,int>> debtLog;
    for (size_t sid = 0; sid < G.spots.size(); ++sid) {
        int stock = max(1, G.spots[sid].amount);
        int miss = max(0, remaining[sid]);
        if (miss > 0) {
            G.spotMissDebt[sid] = min(200, G.spotMissDebt[sid] + miss + (miss >= 2 ? 1 : 0));
        } else {
            G.spotMissDebt[sid] = max(0, G.spotMissDebt[sid] - max(1, served[sid]));
        }
        if (G.spotMissDebt[sid] > 0) {
            debtLog.push_back({G.spotMissDebt[sid] * (100 + 20 * stock), (int)sid});
        }
    }
    sort(debtLog.rbegin(), debtLog.rend());
    if (!SUPPRESS_PLAN_LOGS) {
        cerr << "spot_debt day=" << G.day << " top=";
        for (int i = 0; i < (int)debtLog.size() && i < 5; ++i) {
            int sid = debtLog[i].second;
            if (i) cerr << ",";
            cerr << G.spots[sid].pos << ":" << G.spotMissDebt[sid];
        }
        cerr << "\n";
    }
}

static vector<pair<int,int>> missingSpotsForPlan(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    vector<int> remaining(G.spots.size(), 0);
    for (size_t sid = 0; sid < G.spots.size(); ++sid) remaining[sid] = max(1, G.spots[sid].amount);
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    if (!team.valid) return {};
    for (size_t i = 0; i < agents.size() && i < team.routes.size(); ++i) {
        if (agents[i].kind != 0) continue;
        for (int sid : team.routes[i].visitedSpots) {
            if (sid >= 0 && sid < (int)remaining.size() && remaining[sid] > 0) remaining[sid]--;
        }
    }
    vector<pair<int,int>> ranked;
    for (size_t sid = 0; sid < G.spots.size(); ++sid) {
        int miss = remaining[sid];
        if (miss <= 0) continue;
        int stock = max(1, G.spots[sid].amount);
        int score = miss * (1000 + 150 * stock) + spotDebt((int)sid) * (180 + 30 * stock);
        if (miss <= 2 && stock >= 4) score += 700;
        ranked.push_back({score, (int)sid});
    }
    sort(ranked.rbegin(), ranked.rend());
    return ranked;
}

static bool repairCandidateAcceptable(const PlanEval& candidate, const PlanEval& baseline) {
    if (candidate.dailyBrands < baseline.dailyBrands) return false;
    if (candidate.cappedPortions < baseline.cappedPortions) return false;
    if (candidate.serverEst < baseline.serverEst) return false;
    if (candidate.roadHeavyRoutes > baseline.roadHeavyRoutes + 1 && candidate.serverEst <= baseline.serverEst) return false;
    if (candidate.negativeSlackRoutes > baseline.negativeSlackRoutes + 1 && candidate.serverEst <= baseline.serverEst) return false;
    if (G.day + 1 < (int)G.daySteps.size() && candidate.teamFuelRisk > baseline.teamFuelRisk + max(25, G.fuelLimit / 5)) return false;
    return candidate.cappedPortions > baseline.cappedPortions ||
           candidate.serverEst > baseline.serverEst;
}

static int planNegativeSlackCount(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    TeamSimulation team = simulateTeamPlan(plan, agents, dayBudget);
    if (!team.valid) return INF;
    vector<int> plannedRoadUse(G.nodeCount(), 0);
    int count = 0;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0 || i >= team.routes.size() || !team.routes[i].valid) continue;
        int slack = dayBudget - conservativeSteps(team.routes[i], plannedRoadUse);
        if (slack < 0) count++;
        for (int src : team.routes[i].moveSources) {
            if (terrainAt(src) == 1) plannedRoadUse[src]++;
        }
    }
    return count;
}

static vector<vector<int>> repairRiskySlackRoutes(
    const vector<vector<int>>& inputPlan,
    const vector<Agent>& agents,
    int dayBudget
) {
    vector<vector<int>> bestPlan = normalizePlan(inputPlan, agents, dayBudget);
    if (!validatePlan(bestPlan, agents, dayBudget)) return inputPlan;
    PlanEval bestEval = evaluatePlanForSelection(bestPlan, agents, dayBudget);
    int bestNeg = planNegativeSlackCount(bestPlan, agents, dayBudget);
    if (bestNeg <= 0) return bestPlan;

    TeamSimulation team = simulateTeamPlan(bestPlan, agents, dayBudget);
    if (!team.valid) return bestPlan;
    vector<int> plannedRoadUse(G.nodeCount(), 0);
    vector<int> riskyAgents;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0 || i >= team.routes.size() || !team.routes[i].valid) continue;
        int slack = dayBudget - conservativeSteps(team.routes[i], plannedRoadUse);
        if (slack < 0) riskyAgents.push_back((int)i);
        for (int src : team.routes[i].moveSources) {
            if (terrainAt(src) == 1) plannedRoadUse[src]++;
        }
    }

    int attempts = 0;
    for (int aid : riskyAgents) {
        if (++attempts > 4 || plannerTimeExceeded(18)) break;
        vector<int> trimmed = bestPlan[aid];
        while (!trimmed.empty() && trimmed.back() < 0) trimmed.pop_back();
        int trims = 0;
        while (!trimmed.empty() && trims++ < 12 && !plannerTimeExceeded(12)) {
            while (!trimmed.empty() && trimmed.back() < 0) trimmed.pop_back();
            if (trimmed.empty()) break;
            trimmed.pop_back();
            vector<vector<int>> candidatePlan = bestPlan;
            candidatePlan[aid] = trimmed;
            candidatePlan = normalizePlan(candidatePlan, agents, dayBudget);
            if (!validatePlan(candidatePlan, agents, dayBudget)) continue;
            PlanEval candidateEval = evaluatePlanForSelection(candidatePlan, agents, dayBudget);
            int candidateNeg = planNegativeSlackCount(candidatePlan, agents, dayBudget);
            bool acceptable =
                candidateEval.dailyBrands >= bestEval.dailyBrands &&
                candidateEval.serverEst >= bestEval.serverEst &&
                candidateEval.cappedPortions + 1 >= bestEval.cappedPortions &&
                candidateNeg < bestNeg;
            if (!acceptable) continue;
            bestPlan = std::move(candidatePlan);
            bestEval = candidateEval;
            bestNeg = candidateNeg;
            if (!SUPPRESS_PLAN_LOGS) {
                cerr << "risk_repair day=" << G.day
                     << " agent=" << aid
                     << " portions=" << bestEval.cappedPortions
                     << " server=" << bestEval.serverEst
                     << " negative_slack=" << bestNeg
                     << "\n";
            }
            break;
        }
        if (bestNeg <= 0) break;
    }
    return bestPlan;
}

static vector<vector<int>> repairLastPortions(
    const vector<vector<int>>& inputPlan,
    const vector<Agent>& agents,
    int dayBudget
) {
    vector<vector<int>> bestPlan = inputPlan;
    if (!validatePlan(bestPlan, agents, dayBudget)) return inputPlan;
    PlanEval bestEval = evaluatePlanForSelection(bestPlan, agents, dayBudget);
    vector<pair<int,int>> missing = missingSpotsForPlan(bestPlan, agents, dayBudget);
    int tries = 0;
    int maxRepairSpots = (G.daySeconds > 0 && G.daySeconds <= 2) ? 2 : 3;
    for (const auto& item : missing) {
        if (++tries > maxRepairSpots || plannerTimeExceeded(25)) break;
        int sid = item.second;
        for (const Agent& agent : agents) {
            if (agent.kind != 0 || plannerTimeExceeded(20)) continue;
            Route oldRoute = simulateRoute(agent, bestPlan[agent.id], dayBudget);
            if (!oldRoute.valid) continue;
            DijkstraResult fromStart = dijkstraFrom(agent.pos);
            int d = fromStart.dist[G.spots[sid].pos];
            int f = fromStart.fuel[G.spots[sid].pos];
            if (d >= INF || f >= INF || d > dayBudget || f > agent.fuel) continue;
            if (G.day + 1 < (int)G.daySteps.size() && agent.fuel - f < max(LOW_FUEL_ROUTE_LIMIT, G.fuelLimit / 6)) continue;

            vector<Route> repairRoutes;
            for (const PathOption& option : pathOptionsToSpot(agent, sid, dayBudget)) {
                Route direct = simulateRoute(agent, option.actions, dayBudget);
                if (direct.valid && find(direct.visitedSpots.begin(), direct.visitedSpots.end(), sid) != direct.visitedSpots.end()) {
                    repairRoutes.push_back(std::move(direct));
                }
            }
            Route heavy = generateRouteToHeavySpot(agent, sid, dayBudget, {});
            if (heavy.valid && heavy.stepsUsed > 0 &&
                find(heavy.visitedSpots.begin(), heavy.visitedSpots.end(), sid) != heavy.visitedSpots.end()) {
                repairRoutes.push_back(std::move(heavy));
            }

            bool acceptedRepair = false;
            for (const Route& repairRoute : repairRoutes) {
                vector<vector<int>> candidatePlan = bestPlan;
                candidatePlan[agent.id] = repairRoute.actions;
                candidatePlan = normalizePlan(candidatePlan, agents, dayBudget);
                if (!validatePlan(candidatePlan, agents, dayBudget)) continue;
                PlanEval candidateEval = evaluatePlanForSelection(candidatePlan, agents, dayBudget);
                if (repairCandidateAcceptable(candidateEval, bestEval)) {
                    bestPlan = std::move(candidatePlan);
                    bestEval = candidateEval;
                    acceptedRepair = true;
                    if (!SUPPRESS_PLAN_LOGS) {
                        cerr << "last_portion_repair day=" << G.day
                             << " spot=" << G.spots[sid].pos
                             << " agent=" << agent.id
                             << " portions=" << bestEval.cappedPortions
                             << " server=" << bestEval.serverEst
                             << " wait_on_road=" << bestEval.waitOnRoadRisk
                             << " end_on_road=" << bestEval.endOnRoadRoutes
                             << "\n";
                    }
                    break;
                }
            }
            if (acceptedRepair) break;
        }
    }
    return bestPlan;
}

[[maybe_unused]] static void rememberPredictedVisits(const vector<vector<int>>& plan, const vector<Agent>& agents, int dayBudget) {
    set<int> daySpots;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].kind != 0) continue;
        Route r = simulateRoute(agents[i], plan[i], dayBudget);
        if (!r.valid) continue;
        for (int sid : r.visitedSpots) daySpots.insert(sid);
    }
    for (int sid : daySpots) {
        if (sid >= 0 && sid < (int)G.spots.size()) {
            G.globalSpotsSeen.insert(sid);
            G.globalBrandsSeen.insert(G.spots[sid].brand);
        }
    }
}

[[maybe_unused]] static vector<vector<int>> planDayAfterTimeline(const vector<Agent>& agents) {
    auto started = chrono::steady_clock::now();
    PLANNER_STARTED = started;
    PLANNER_STATS = PlannerStats();
    DAY_PATH_CACHE.clear();
    int dayBudget = G.dayBudget();
    CURRENT_CONFIG = makePlannerConfig(agents);
    PlanProfile profile = selectPlanProfile();
    ACTIVE_DEADLINE.startedAt = started;
    ACTIVE_DEADLINE.hardBudgetMs = max(10, CURRENT_CONFIG.computeBudgetMs);
    ACTIVE_DEADLINE.reserveMs = CURRENT_CONFIG.deadlineReserveMs;
    ACTIVE_DEADLINE.stopAt = started + chrono::milliseconds(ACTIVE_DEADLINE.hardBudgetMs);
    DEADLINE_ACTIVE = true;
    cerr << "plan day=" << G.day
         << " budget=" << dayBudget
         << " agents=" << agents.size()
         << " spots=" << G.spots.size()
         << " traffic=" << G.traffic.size()
         << " profile=" << profileName(profile)
         << "\n";
    cerr << "planner_config day=" << G.day
         << " budget=" << CURRENT_CONFIG.dayBudget
         << " median_steps=" << CURRENT_CONFIG.medianDaySteps
         << " step_ratio=" << CURRENT_CONFIG.stepRatio
         << " map_scale=" << CURRENT_CONFIG.mapScale
         << " fuel_ratio=" << CURRENT_CONFIG.fuelRatio
         << " stock_cap=" << CURRENT_CONFIG.stockCap
         << " patrols=" << CURRENT_CONFIG.patrols
         << " beam=" << CURRENT_CONFIG.beamWidth
         << " pool=" << CURRENT_CONFIG.poolLimit
         << " modes=" << CURRENT_CONFIG.routeModeCount
         << " heavy=" << CURRENT_CONFIG.heavyTargetLimit
         << " min_slack=" << CURRENT_CONFIG.minSlack
         << " max_visits=" << CURRENT_CONFIG.maxVisits
         << " traffic_reserve=" << CURRENT_CONFIG.trafficRiskReserveSteps
         << " deadline_mode=" << CURRENT_CONFIG.deadlineMode
         << " ultra_fast=" << (CURRENT_CONFIG.ultraFastMode ? 1 : 0)
         << " day_seconds=" << CURRENT_CONFIG.daySeconds
         << " deadline_margin_ms=" << G.deadlineMarginMs
         << " hard_budget_ms=" << CURRENT_CONFIG.computeBudgetMs
         << "\n";
    if (CURRENT_CONFIG.ultraFastMode && G.deadlineMarginMs < 0) {
        vector<vector<int>> fastPlan = buildUltraFastPlan(agents, dayBudget);
        if (!validatePlan(fastPlan, agents, dayBudget)) {
            cerr << "ultra_fast validator rejected candidate, using safe plan\n";
            fastPlan = buildSafePlan(agents, dayBudget);
        }
        int computeMs = (int)chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - started).count();
        cerr << "planner_timing day=" << G.day
             << " compute_ms=" << computeMs
             << " budget_ms=" << plannerBudgetMs()
             << " emergency=1\n";
        logFinalRefuelTimeline(fastPlan, agents, {}, dayBudget);
        return fastPlan;
    }
    auto fastStarted = chrono::steady_clock::now();
    vector<vector<int>> fastBaselinePlan = buildUltraFastPlan(agents, dayBudget);
    PlanEval fastEval = evaluatePlanForSelection(fastBaselinePlan, agents, dayBudget);
    ExactScore fastExact = exactScoreForPlan(fastBaselinePlan, agents, dayBudget);
    vector<Cluster> baselineClusters;
    bool hasTanker = any_of(agents.begin(), agents.end(), [](const Agent& agent) { return agent.kind == 1; });
    if (hasTanker && !plannerTimeExceeded(8)) {
        baselineClusters = buildClusters(agents);
        vector<vector<int>> scheduledFast = scheduleRefuelForPlan(fastBaselinePlan, agents, baselineClusters, dayBudget);
        PlanEval scheduledEval = evaluatePlanForSelection(scheduledFast, agents, dayBudget);
        ExactScore scheduledExact = exactScoreForPlan(scheduledFast, agents, dayBudget);
        if (strongPlanAcceptableExact(scheduledExact, fastExact, false)) {
            fastBaselinePlan = std::move(scheduledFast);
            fastEval = scheduledEval;
            fastExact = scheduledExact;
        }
    }
    int fastMs = (int)chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - fastStarted).count();
    if (CURRENT_CONFIG.ultraFastMode && G.deadlineMarginMs >= 100 && !plannerTimeExceeded(max(8, plannerBudgetMs() / 4))) {
        vector<Cluster> fastClusters = baselineClusters.empty() ? buildClusters(agents) : baselineClusters;
        vector<vector<RouteCandidate>> fastPools = generateRoutePools(agents, dayBudget, fastClusters);
        SUPPRESS_PLAN_LOGS = true;
        vector<Route> fastRoutes = combineRoutesStockAware(agents, fastPools, dayBudget);
        SUPPRESS_PLAN_LOGS = false;
        vector<vector<int>> shallowFastPlan = combineRoutesDiversityFirst(agents, fastRoutes, fastClusters, dayBudget);
        if (hasTanker && !plannerTimeExceeded(5)) {
            shallowFastPlan = scheduleRefuelForPlan(shallowFastPlan, agents, fastClusters, dayBudget);
        }
        if (validatePlan(shallowFastPlan, agents, dayBudget)) {
            PlanEval shallowFastEval = evaluatePlanForSelection(shallowFastPlan, agents, dayBudget);
            ExactScore shallowFastExact = exactScoreForPlan(shallowFastPlan, agents, dayBudget);
            if (strongPlanAcceptableExact(shallowFastExact, fastExact, false)) {
                fastBaselinePlan = shallowFastPlan;
                fastEval = shallowFastEval;
                fastExact = shallowFastExact;
            }
        }
    }
    if (!shouldRunStrongPlanner(fastEval, dayBudget)) {
        if (!validatePlan(fastBaselinePlan, agents, dayBudget)) {
            cerr << "fast_baseline validator rejected candidate, using safe plan\n";
            fastBaselinePlan = buildSafePlan(agents, dayBudget);
            fastEval = evaluatePlanForSelection(fastBaselinePlan, agents, dayBudget);
            fastExact = exactScoreForPlan(fastBaselinePlan, agents, dayBudget);
        }
        cerr << "plan_compare day=" << G.day
             << " fast_brands=" << fastEval.dailyBrands
             << " fast_portions=" << fastEval.cappedPortions
             << " fast_server=" << fastEval.serverEst
             << " fast_debt=" << fastEval.spotDebtGain
             << " fast_exact_portions=" << fastExact.actualPortions
             << " fast_exact_global=" << fastExact.globalBrands
             << " fast_effective_capture=" << fastExact.effectiveCaptureRatio
             << " fast_ghost_stock=" << fastExact.ghostStock
             << " fast_neg_slack=" << fastEval.negativeSlackRoutes
             << " fast_road_heavy=" << fastEval.roadHeavyRoutes
             << " fast_wait_on_road=" << fastEval.waitOnRoadRisk
             << " fast_end_on_road=" << fastEval.endOnRoadRoutes
             << " fast_low_fuel=" << fastEval.lowFuelPatrols
             << " fast_fuel_risk=" << fastEval.teamFuelRisk
             << " strong_brands=-1 strong_portions=-1 strong_server=-1"
             << " strong_debt=0"
             << " strong_neg_slack=0 strong_road_heavy=0"
             << " strong_wait_on_road=0 strong_end_on_road=0"
             << " strong_low_fuel=0 strong_fuel_risk=0\n";
        cerr << "plan_compare day=" << G.day
             << " selected=fast_baseline reason=deadline_gate"
             << " selected_exact_score=" << fastExact.globalBrands << "," << fastExact.dailyBrands << "," << fastExact.actualPortions
             << " selected_exact_server=" << fastExact.serverEst
             << " effective_capture_ratio=" << fastExact.effectiveCaptureRatio
             << " ghost_stock=" << fastExact.ghostStock
             << "\n";
        int computeMs = (int)chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - started).count();
        cerr << "planner_timing day=" << G.day
             << " compute_ms=" << computeMs
             << " budget_ms=" << plannerBudgetMs()
             << " fast_ms=" << fastMs
             << " fast_only=1\n";
        cerr << "planner_stats day=" << G.day
             << " dijkstra_calls=" << PLANNER_STATS.dijkstraCalls
             << " cache_hits=" << PLANNER_STATS.dijkstraCacheHits
             << " pareto_labels=" << PLANNER_STATS.paretoLabels
             << " patrol_labels=" << PLANNER_STATS.patrolLabels
             << " team_states=" << PLANNER_STATS.teamStates
             << " dominance_pruned=" << PLANNER_STATS.dominancePruned
             << " deadline_checks=" << PLANNER_STATS.deadlineChecks
             << " timeout_stage=" << (PLANNER_STATS.timeoutStage.empty() ? "none" : PLANNER_STATS.timeoutStage)
             << "\n";
        logFinalRefuelTimeline(fastBaselinePlan, agents, baselineClusters, dayBudget);
        return fastBaselinePlan;
    }
    auto strongStarted = chrono::steady_clock::now();
    vector<Cluster> clusters = baselineClusters.empty() ? buildClusters(agents) : baselineClusters;
    int cap = stockCap();
    cerr << "map_metrics day=" << G.day
         << " clusters=" << clusters.size()
         << " daily_stock_cap=" << cap
         << " total_stock_cap=" << (cap * (int)G.daySteps.size())
         << " cluster_stock=";
    for (size_t i = 0; i < clusters.size(); ++i) {
        if (i) cerr << ",";
        cerr << clusters[i].center << ":" << clusters[i].totalStock;
    }
    cerr << "\n";
    vector<vector<RouteCandidate>> pools = generateRoutePools(agents, dayBudget, clusters);
    vector<Route> routes = combineRoutesStockAware(agents, pools, dayBudget);
    vector<vector<int>> plan = combineRoutesDiversityFirst(agents, routes, clusters, dayBudget);
    if (hasTanker && !plannerTimeExceeded(5)) {
        plan = scheduleRefuelForPlan(plan, agents, clusters, dayBudget);
    }
    if (!validatePlan(plan, agents, dayBudget)) {
        cerr << "validator rejected candidate, using safe plan\n";
        plan = buildSafePlan(agents, dayBudget);
    }
    PlanEval strongEval = evaluatePlanForSelection(plan, agents, dayBudget);
    ExactScore strongExact = exactScoreForPlan(plan, agents, dayBudget);
    bool strongInterrupted = !PLANNER_STATS.timeoutStage.empty();
    cerr << "plan_compare day=" << G.day
         << " fast_brands=" << fastEval.dailyBrands
         << " fast_portions=" << fastEval.cappedPortions
         << " fast_server=" << fastEval.serverEst
         << " fast_debt=" << fastEval.spotDebtGain
         << " fast_exact_global=" << fastExact.globalBrands
         << " fast_exact_portions=" << fastExact.actualPortions
         << " fast_effective_capture=" << fastExact.effectiveCaptureRatio
         << " fast_ghost_stock=" << fastExact.ghostStock
         << " fast_neg_slack=" << fastEval.negativeSlackRoutes
         << " fast_road_heavy=" << fastEval.roadHeavyRoutes
         << " fast_wait_on_road=" << fastEval.waitOnRoadRisk
         << " fast_end_on_road=" << fastEval.endOnRoadRoutes
         << " fast_low_fuel=" << fastEval.lowFuelPatrols
         << " fast_fuel_risk=" << fastEval.teamFuelRisk
         << " strong_brands=" << strongEval.dailyBrands
         << " strong_portions=" << strongEval.cappedPortions
         << " strong_server=" << strongEval.serverEst
         << " strong_debt=" << strongEval.spotDebtGain
         << " strong_exact_global=" << strongExact.globalBrands
         << " strong_exact_portions=" << strongExact.actualPortions
         << " strong_effective_capture=" << strongExact.effectiveCaptureRatio
         << " strong_ghost_stock=" << strongExact.ghostStock
         << " strong_neg_slack=" << strongEval.negativeSlackRoutes
         << " strong_road_heavy=" << strongEval.roadHeavyRoutes
         << " strong_wait_on_road=" << strongEval.waitOnRoadRisk
         << " strong_end_on_road=" << strongEval.endOnRoadRoutes
         << " strong_low_fuel=" << strongEval.lowFuelPatrols
         << " strong_fuel_risk=" << strongEval.teamFuelRisk
         << "\n";
    ExactScore selectedExact = strongExact;
    int selectionRegret = 0;
    if (!strongPlanAcceptableGuarded(strongEval, fastEval, strongExact, fastExact, dayBudget, strongInterrupted)) {
        selectedExact = fastExact;
        selectionRegret = max(0, strongExact.actualPortions - fastExact.actualPortions);
        cerr << "plan_compare day=" << G.day
             << " selected=fast_baseline"
             << " reason=legacy_policy_guard"
             << " selected_exact_score=" << selectedExact.globalBrands << "," << selectedExact.dailyBrands << "," << selectedExact.actualPortions
             << " selected_exact_server=" << selectedExact.serverEst
             << " best_candidate_exact_server=" << max(fastExact.serverEst, strongExact.serverEst)
             << " selection_regret=" << selectionRegret
             << " effective_capture_ratio=" << selectedExact.effectiveCaptureRatio
             << " ghost_stock=" << selectedExact.ghostStock
             << "\n";
        plan = fastBaselinePlan;
    } else {
        selectionRegret = max(0, fastExact.actualPortions - strongExact.actualPortions);
        cerr << "plan_compare day=" << G.day
             << " selected=strong"
             << " reason=legacy_policy_guard"
             << " selected_exact_score=" << selectedExact.globalBrands << "," << selectedExact.dailyBrands << "," << selectedExact.actualPortions
             << " selected_exact_server=" << selectedExact.serverEst
             << " best_candidate_exact_server=" << max(fastExact.serverEst, strongExact.serverEst)
             << " selection_regret=" << selectionRegret
             << " effective_capture_ratio=" << selectedExact.effectiveCaptureRatio
             << " ghost_stock=" << selectedExact.ghostStock
             << "\n";
    }
    int computeMs = (int)chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - started).count();
    int strongMs = (int)chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - strongStarted).count();
    cerr << "planner_timing day=" << G.day
         << " compute_ms=" << computeMs
         << " budget_ms=" << plannerBudgetMs()
         << " fast_ms=" << fastMs
         << " strong_ms=" << strongMs
         << "\n";
    cerr << "planner_stats day=" << G.day
         << " dijkstra_calls=" << PLANNER_STATS.dijkstraCalls
         << " cache_hits=" << PLANNER_STATS.dijkstraCacheHits
         << " pareto_labels=" << PLANNER_STATS.paretoLabels
         << " patrol_labels=" << PLANNER_STATS.patrolLabels
         << " team_states=" << PLANNER_STATS.teamStates
         << " dominance_pruned=" << PLANNER_STATS.dominancePruned
         << " deadline_checks=" << PLANNER_STATS.deadlineChecks
         << " timeout_stage=" << (PLANNER_STATS.timeoutStage.empty() ? "none" : PLANNER_STATS.timeoutStage)
         << "\n";
    logFinalRefuelTimeline(plan, agents, clusters, dayBudget);
    return plan;
}

static vector<vector<int>> planDay(const vector<Agent>& agents) {
    auto started = chrono::steady_clock::now();
    PLANNER_STARTED = started;
    PLANNER_STATS = PlannerStats();
    DAY_PATH_CACHE.clear();
    BASELINE_0564_MODE = true;
    DEADLINE_ACTIVE = false;

    int dayBudget = G.dayBudget();
    CURRENT_CONFIG = makePlannerConfig(agents);
    PlanProfile profile = selectPlanProfile();
    cerr << "plan day=" << G.day
         << " budget=" << dayBudget
         << " agents=" << agents.size()
         << " spots=" << G.spots.size()
         << " traffic=" << G.traffic.size()
         << " profile=" << profileName(profile)
         << "\n";
    cerr << "planner_config day=" << G.day
         << " budget=" << CURRENT_CONFIG.dayBudget
         << " median_steps=" << CURRENT_CONFIG.medianDaySteps
         << " step_ratio=" << CURRENT_CONFIG.stepRatio
         << " map_scale=" << CURRENT_CONFIG.mapScale
         << " fuel_ratio=" << CURRENT_CONFIG.fuelRatio
         << " stock_cap=" << CURRENT_CONFIG.stockCap
         << " patrols=" << CURRENT_CONFIG.patrols
         << " beam=" << CURRENT_CONFIG.beamWidth
         << " pool=" << CURRENT_CONFIG.poolLimit
         << " modes=" << CURRENT_CONFIG.routeModeCount
         << " heavy=" << CURRENT_CONFIG.heavyTargetLimit
         << " min_slack=" << CURRENT_CONFIG.minSlack
         << " max_visits=" << CURRENT_CONFIG.maxVisits
         << " deadline_mode=" << CURRENT_CONFIG.deadlineMode
         << " ultra_fast=" << (CURRENT_CONFIG.ultraFastMode ? 1 : 0)
         << " day_seconds=" << CURRENT_CONFIG.daySeconds
         << " deadline_margin_ms=" << G.deadlineMarginMs
         << "\n";

    if (CURRENT_CONFIG.ultraFastMode && G.deadlineMarginMs < 0) {
        vector<vector<int>> emergency = buildUltraFastPlan(agents, dayBudget);
        if (!validatePlan(emergency, agents, dayBudget)) {
            cerr << "ultra_fast validator rejected candidate, using safe plan\n";
            emergency = buildSafePlan(agents, dayBudget);
        }
        int computeMs = (int)chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now() - started).count();
        cerr << "planner_timing day=" << G.day
             << " compute_ms=" << computeMs
             << " budget_ms=" << plannerBudgetMs()
             << " emergency=1\n";
        updateSpotMissDebtFromPlan(emergency, agents, dayBudget);
        return emergency;
    }

    auto fastStarted = chrono::steady_clock::now();
    vector<vector<int>> fastPlan = buildUltraFastPlan(agents, dayBudget);
    if (!validatePlan(fastPlan, agents, dayBudget)) {
        cerr << "fast_baseline validator rejected candidate, using safe plan\n";
        fastPlan = buildSafePlan(agents, dayBudget);
    }
    PlanEval fastEval = evaluatePlanForSelection(fastPlan, agents, dayBudget);
    int fastMs = (int)chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now() - fastStarted).count();

    if (!shouldRunStrongPlanner(fastEval, dayBudget)) {
        cerr << "plan_compare day=" << G.day
             << " fast_brands=" << fastEval.dailyBrands
             << " fast_portions=" << fastEval.cappedPortions
             << " fast_server=" << fastEval.serverEst
             << " fast_debt=" << fastEval.spotDebtGain
             << " fast_neg_slack=" << fastEval.negativeSlackRoutes
             << " fast_road_heavy=" << fastEval.roadHeavyRoutes
             << " fast_low_fuel=" << fastEval.lowFuelPatrols
             << " fast_fuel_risk=" << fastEval.teamFuelRisk
             << " strong_brands=-1 strong_portions=-1 strong_server=-1"
             << " strong_debt=0"
             << " strong_neg_slack=0 strong_road_heavy=0"
             << " strong_low_fuel=0 strong_fuel_risk=0\n";
        cerr << "plan_compare day=" << G.day
             << " selected=fast_baseline selected_reason=deadline_gate\n";
        int computeMs = (int)chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now() - started).count();
        cerr << "planner_timing day=" << G.day
             << " compute_ms=" << computeMs
             << " budget_ms=" << plannerBudgetMs()
             << " fast_ms=" << fastMs
             << " fast_only=1\n";
        if (!plannerTimeExceeded(35)) {
            fastPlan = repairRiskySlackRoutes(fastPlan, agents, dayBudget);
        }
        if (!plannerTimeExceeded(35)) {
            fastPlan = repairLastPortions(fastPlan, agents, dayBudget);
        }
        if (!plannerTimeExceeded(20)) {
            fastPlan = repairRiskySlackRoutes(fastPlan, agents, dayBudget);
        }
        updateSpotMissDebtFromPlan(fastPlan, agents, dayBudget);
        return fastPlan;
    }

    auto strongStarted = chrono::steady_clock::now();
    vector<Cluster> clusters = buildClusters(agents);
    int cap = stockCap();
    cerr << "map_metrics day=" << G.day
         << " clusters=" << clusters.size()
         << " daily_stock_cap=" << cap
         << " total_stock_cap=" << (cap * (int)G.daySteps.size())
         << " cluster_stock=";
    for (size_t i = 0; i < clusters.size(); ++i) {
        if (i) cerr << ",";
        cerr << clusters[i].center << ":" << clusters[i].totalStock;
    }
    cerr << "\n";

    vector<vector<RouteCandidate>> pools = generateRoutePools(agents, dayBudget, clusters);
    vector<Route> routes = combineRoutesStockAware(agents, pools, dayBudget);
    vector<vector<int>> strongPlan =
        combineRoutesDiversityFirst(agents, routes, clusters, dayBudget);
    if (!validatePlan(strongPlan, agents, dayBudget)) {
        cerr << "validator rejected candidate, using safe plan\n";
        strongPlan = buildSafePlan(agents, dayBudget);
    }
    PlanEval strongEval = evaluatePlanForSelection(strongPlan, agents, dayBudget);

    cerr << "plan_compare day=" << G.day
         << " fast_brands=" << fastEval.dailyBrands
         << " fast_portions=" << fastEval.cappedPortions
         << " fast_server=" << fastEval.serverEst
         << " fast_debt=" << fastEval.spotDebtGain
         << " fast_neg_slack=" << fastEval.negativeSlackRoutes
         << " fast_road_heavy=" << fastEval.roadHeavyRoutes
         << " fast_low_fuel=" << fastEval.lowFuelPatrols
         << " fast_fuel_risk=" << fastEval.teamFuelRisk
         << " strong_brands=" << strongEval.dailyBrands
         << " strong_portions=" << strongEval.cappedPortions
         << " strong_server=" << strongEval.serverEst
         << " strong_debt=" << strongEval.spotDebtGain
         << " strong_neg_slack=" << strongEval.negativeSlackRoutes
         << " strong_road_heavy=" << strongEval.roadHeavyRoutes
         << " strong_low_fuel=" << strongEval.lowFuelPatrols
         << " strong_fuel_risk=" << strongEval.teamFuelRisk
         << "\n";

    vector<vector<int>> selectedPlan;
    string selectedReason = strongPlanDecisionReason(strongEval, fastEval, dayBudget);
    if (strongPlanAcceptable(strongEval, fastEval, dayBudget)) {
        selectedPlan = std::move(strongPlan);
        cerr << "plan_compare day=" << G.day << " selected=strong selected_reason=" << selectedReason << "\n";
    } else {
        selectedPlan = std::move(fastPlan);
        cerr << "plan_compare day=" << G.day << " selected=fast_baseline selected_reason=" << selectedReason << "\n";
    }
    if (!validatePlan(selectedPlan, agents, dayBudget)) {
        cerr << "selected plan invalid, using safe plan\n";
        selectedPlan = buildSafePlan(agents, dayBudget);
    }
    if (!plannerTimeExceeded(35)) {
        selectedPlan = repairRiskySlackRoutes(selectedPlan, agents, dayBudget);
    }
    if (!plannerTimeExceeded(35)) {
        selectedPlan = repairLastPortions(selectedPlan, agents, dayBudget);
    }
    if (!plannerTimeExceeded(20)) {
        selectedPlan = repairRiskySlackRoutes(selectedPlan, agents, dayBudget);
    }

    int computeMs = (int)chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now() - started).count();
    int strongMs = (int)chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now() - strongStarted).count();
    cerr << "planner_timing day=" << G.day
         << " compute_ms=" << computeMs
         << " budget_ms=" << plannerBudgetMs()
         << " fast_ms=" << fastMs
         << " strong_ms=" << strongMs
         << "\n";
    updateSpotMissDebtFromPlan(selectedPlan, agents, dayBudget);
    return selectedPlan;
}

static string serializePlan(const vector<vector<int>>& plan) {
    ostringstream out;
    out << "[";
    for (size_t i = 0; i < plan.size(); ++i) {
        if (i) out << ",";
        out << "[";
        for (size_t j = 0; j < plan[i].size(); ++j) {
            if (j) out << ",";
            out << plan[i][j];
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

static SetupForecast forecastSetupAssignment(
    const vector<int>& startPositions,
    const vector<pair<long long,int>>& refuelRank,
    int tankerCount
) {
    SetupForecast forecast;
    int nAgents = (int)startPositions.size();
    vector<int> tankerChosen(nAgents, 0);
    for (int i = 0; i < tankerCount && i < (int)refuelRank.size(); ++i) {
        tankerChosen[refuelRank[i].second] = 1;
    }

    vector<Agent> simAgents;
    for (int i = 0; i < nAgents; ++i) {
        Agent agent;
        agent.id = i;
        agent.kind = tankerChosen[i];
        agent.pos = startPositions[i];
        agent.fuel = tankerChosen[i] ? INF : G.fuelLimit;
        simAgents.push_back(agent);
    }

    int savedDay = G.day;
    vector<int> savedDebt = G.spotMissDebt;
    int savedIdleStreak = G.tankerIdleStreak;
    PlannerConfig savedConfig = CURRENT_CONFIG;
    bool savedSuppress = SUPPRESS_PLAN_LOGS;
    SUPPRESS_PLAN_LOGS = true;
    int horizon = min(3, max(1, (int)G.daySteps.size()));
    for (int d = 0; d < horizon; ++d) {
        G.day = d;
        CURRENT_CONFIG = makePlannerConfig(simAgents);
        vector<vector<int>> plan = buildUltraFastPlan(simAgents, G.dayBudget());
        PlanEval eval = evaluatePlanForSelection(plan, simAgents, G.dayBudget());
        TeamSimulation team = simulateTeamPlan(plan, simAgents, G.dayBudget());
        forecast.serverEst += eval.serverEst;
        forecast.cappedPortions += eval.cappedPortions;
        forecast.dailyBrandsSum += eval.dailyBrands;
        forecast.lowFuelPatrols += eval.lowFuelPatrols;
        forecast.teamFuelRisk += eval.teamFuelRisk;
        forecast.minFuelEnd = min(forecast.minFuelEnd, eval.minFuelEnd);
        forecast.reachableCap += min(stockCap(), eval.terminalPortions + eval.cappedPortions);
        if (team.valid) {
            forecast.feasibleRefuels += (int)team.refuels.size();
            forecast.successfulRefuels += (int)team.refuels.size();
            for (size_t i = 0; i < simAgents.size() && i < team.routes.size(); ++i) {
                simAgents[i].pos = team.routes[i].endPos;
                if (simAgents[i].kind == 0) simAgents[i].fuel = max(0, team.finalFuel[i]);
                else simAgents[i].fuel = INF;
                if (simAgents[i].kind == 1 && team.routes[i].stepsUsed == 0) forecast.tankerIdleDays++;
            }
        }
    }
    if (forecast.minFuelEnd == INF) forecast.minFuelEnd = 0;
    forecast.idlePenalty = 2 * forecast.tankerIdleDays;
    G.day = savedDay;
    G.spotMissDebt = savedDebt;
    G.tankerIdleStreak = savedIdleStreak;
    CURRENT_CONFIG = savedConfig;
    SUPPRESS_PLAN_LOGS = savedSuppress;
    return forecast;
}

static void handleSetup(const mj::Value& m) {
    const mj::Value& mp = m["map"];
    G.width = mp["width"].asInt();
    G.height = mp["height"].asInt();
    G.cells.assign(G.width * G.height, 0);
    for (int r = 0; r < G.height; ++r) {
        for (int c = 0; c < G.width; ++c) {
            G.cells[r * G.width + c] = mp["cells"][r][c].asInt();
        }
    }

    G.spots.clear();
    for (size_t i = 0; i < m["spots"].size(); ++i) {
        Spot s;
        s.id = (int)i;
        s.pos = m["spots"][i]["pos"].asInt();
        s.brand = firstValueOrDefault(m["spots"][i], {"brand", "type", "udonType"}, (int)i);
        s.amount = firstValueOrDefault(m["spots"][i], {"stocks", "amount", "stock", "count"}, 1);
        G.spots.push_back(s);
    }
    G.spotMissDebt.assign(G.spots.size(), 0);
    G.tankerIdleStreak = 0;

    G.daySteps.clear();
    for (size_t i = 0; i < m["daySteps"].size(); ++i) G.daySteps.push_back(m["daySteps"][i].asInt());
    G.fuelLimit = valueOrDefault(m, "fuelLimits", G.fuelLimit);
    G.busyThreshold = valueOrDefault(m, "busyThreshold", G.busyThreshold);
    G.jammedThreshold = valueOrDefault(m, "jammedThreshold", G.jammedThreshold);

    int nAgents = (int)m["agents"].size();
    vector<int> startPositions(nAgents, 0);
    for (int i = 0; i < nAgents; ++i) startPositions[i] = valueOrDefault(m["agents"][i], "pos", 0);
    G.assignment.assign(nAgents, 0);
    int maxDim = max(G.width, G.height);
    bool legacySetup = maxDim <= 12 || (int)G.spots.size() <= 8;
    bool hybridSetup = !legacySetup && maxDim <= 16;
    int refuelCount = nAgents > 1 ? 1 : 0;
    int totalStepBudget = accumulate(G.daySteps.begin(), G.daySteps.end(), 0);
    bool fuelEnoughWithoutTanker = G.fuelLimit >= max(1, totalStepBudget * 4 / 5);
    if (fuelEnoughWithoutTanker && nAgents >= 4) refuelCount = 0;
    bool compactAllPatrolLikelyBetter =
        maxDim <= 16 &&
        nAgents <= 6 &&
        stockCap() <= 32 &&
        ((int)G.daySteps.size() <= 4 || totalStepBudget * 10 <= G.fuelLimit * 22);
    if (compactAllPatrolLikelyBetter) refuelCount = 0;
    bool longFuelTightMatch =
        (int)G.daySteps.size() >= 6 &&
        nAgents >= 6 &&
        G.fuelLimit * 2 <= totalStepBudget;
    if (longFuelTightMatch) refuelCount = max(refuelCount, 1);
    bool tinyShortMatch = maxDim <= 8 && nAgents <= 4 && (int)G.daySteps.size() <= 4 &&
        totalStepBudget <= G.fuelLimit * 2;
    if (tinyShortMatch) refuelCount = 0;

    vector<pair<long long,int>> refuelRank;
    DAY_PATH_CACHE.clear();
    vector<DijkstraResult> startPaths(nAgents);
    for (int i = 0; i < nAgents; ++i) startPaths[i] = dijkstraFrom(startPositions[i]);
    for (int i = 0; i < nAgents; ++i) {
        const DijkstraResult& paths = startPaths[i];
        long long distToSpots = 0;
        int reachable = 0;
        int uniqueClosestStock = 0;
        for (size_t sid = 0; sid < G.spots.size(); ++sid) {
            int d = paths.dist[G.spots[sid].pos];
            if (d < INF) {
                distToSpots += d;
                reachable++;
            } else {
                distToSpots += 1000;
            }
            int best = INF;
            int second = INF;
            for (int j = 0; j < nAgents; ++j) {
                int od = startPaths[j].dist[G.spots[sid].pos];
                if (od < best) {
                    second = best;
                    best = od;
                } else if (od < second) {
                    second = od;
                }
            }
            if (d == best && second - best > max(4, G.dayBudget() / 6)) uniqueClosestStock += max(1, G.spots[sid].amount);
        }
        long long rank = distToSpots - 35LL * reachable + 90LL * uniqueClosestStock;
        refuelRank.push_back({rank, i});
    }
    sort(refuelRank.begin(), refuelRank.end());
    string selectedAssignmentReason = "default";
    if (nAgents >= 6 && (int)G.daySteps.size() >= 4 && refuelCount <= 1 && !refuelRank.empty()) {
        SetupForecast allForecast = forecastSetupAssignment(startPositions, refuelRank, 0);
        SetupForecast tankerForecast = forecastSetupAssignment(startPositions, refuelRank, 1);
        tankerForecast.savedPortions = tankerForecast.serverEst - allForecast.serverEst;
        bool allFuelOk =
            allForecast.minFuelEnd >= max(G.fuelLimit / 5, LOW_FUEL_ROUTE_LIMIT) &&
            allForecast.teamFuelRisk <= tankerForecast.teamFuelRisk + max(40, G.fuelLimit / 3) &&
            (allForecast.lowFuelPatrols <= 1 || allForecast.teamFuelRisk == 0);
        bool tankerPreventsCollapse =
            !allFuelOk ||
            (allForecast.teamFuelRisk > 0 && allForecast.lowFuelPatrols > tankerForecast.lowFuelPatrols + max(1, nAgents / 3)) ||
            allForecast.teamFuelRisk > tankerForecast.teamFuelRisk + max(60, G.fuelLimit / 2);
        bool tankerHasValue =
            tankerForecast.feasibleRefuels > 0 ||
            tankerForecast.successfulRefuels > 0 ||
            tankerPreventsCollapse;
        bool tankerFarmGain =
            tankerHasValue &&
            (tankerForecast.serverEst >= allForecast.serverEst + (isLargeMap() ? 8 : 5) ||
             (tankerForecast.dailyBrandsSum > allForecast.dailyBrandsSum && tankerForecast.serverEst + 2 >= allForecast.serverEst));
        bool tankerIdleBad = tankerForecast.tankerIdleDays >= 2 && !tankerPreventsCollapse && !tankerFarmGain;
        if ((tankerPreventsCollapse && tankerHasValue) || tankerFarmGain) {
            refuelCount = 1;
            selectedAssignmentReason = tankerPreventsCollapse ? "fuel_horizon" : "tanker_server_gain";
        } else if (!tankerHasValue || allForecast.serverEst >= tankerForecast.serverEst + 2 || tankerIdleBad) {
            refuelCount = 0;
            selectedAssignmentReason = !tankerHasValue ? "no_feasible_refuel" : (tankerIdleBad ? "tanker_idle_forecast" : "all_patrol_server_gain");
        } else {
            selectedAssignmentReason = "assignment_tie_keep_default";
        }
        int tankerValueEst = tankerForecast.serverEst - allForecast.serverEst - tankerForecast.idlePenalty;
        cerr << "setup_ab_3day"
             << " all_server=" << allForecast.serverEst
             << " all_portions=" << allForecast.cappedPortions
             << " all_brands=" << allForecast.dailyBrandsSum
             << " all_min_fuel=" << allForecast.minFuelEnd
             << " all_low_fuel=" << allForecast.lowFuelPatrols
             << " all_fuel_risk=" << allForecast.teamFuelRisk
             << " tanker_server=" << tankerForecast.serverEst
             << " tanker_portions=" << tankerForecast.cappedPortions
             << " tanker_brands=" << tankerForecast.dailyBrandsSum
             << " tanker_min_fuel=" << tankerForecast.minFuelEnd
             << " tanker_low_fuel=" << tankerForecast.lowFuelPatrols
             << " tanker_fuel_risk=" << tankerForecast.teamFuelRisk
             << " tanker_idle_days=" << tankerForecast.tankerIdleDays
             << " tanker_feasible_refuels=" << tankerForecast.feasibleRefuels
             << " tanker_successful_refuels=" << tankerForecast.successfulRefuels
             << " tanker_saved_portions=" << tankerForecast.savedPortions
             << " tanker_idle_penalty=" << tankerForecast.idlePenalty
             << " tanker_value_est=" << tankerValueEst
             << " selected_refuels=" << refuelCount
             << " selected_assignment_reason=" << selectedAssignmentReason
             << "\n";
    }
    for (int i = 0; i < refuelCount && i < (int)refuelRank.size(); ++i) {
        G.assignment[refuelRank[i].second] = 1;
    }
    cerr << "setup width=" << G.width
         << " height=" << G.height
         << " agents=" << nAgents
         << " spots=" << G.spots.size()
         << " days=" << G.daySteps.size()
         << " profile=" << (legacySetup ? "legacySmallMap" : (hybridSetup ? "hybrid16" : "generalLarge"))
         << " refuels=" << refuelCount
         << " all_patrol_counterfactual=" << (nAgents > 0 ? nAgents : 0)
         << " tanker_refuel_gain=0"
         << " tanker_idle_steps=0"
         << " selected_assignment_reason=" << selectedAssignmentReason
         << "\n";
    DAY_PATH_CACHE.clear();

    ostringstream out;
    out << "[";
    for (int i = 0; i < nAgents; ++i) {
        if (i) out << ",";
        out << G.assignment[i];
    }
    out << "]";
    cout << out.str() << "\n" << flush;
}

static void handleDayState(const mj::Value& m) {
    G.day = m["day"].asInt();
    G.deadlineMarginMs = firstValueOrDefault(m, {"_deadlineMarginMs", "deadlineMarginMs"}, 1000000);
    G.deadlineMode = firstValueOrDefault(m, {"_deadlineMode", "deadlineMode"}, 0);
    G.daySeconds = firstValueOrDefault(m, {"_daySeconds", "daySeconds"}, 0);
    G.ultraFastMode = firstValueOrDefault(m, {"_ultraFastMode", "ultraFastMode"}, 0) != 0;
    G.hardBudgetMs = firstValueOrDefault(m, {"_hardBudgetMs", "hardBudgetMs"}, 0);
    G.traffic.clear();
    for (size_t i = 0; i < m["traffics"].size(); ++i) {
        G.traffic[m["traffics"][i]["pos"].asInt()] = m["traffics"][i]["status"].asInt();
    }

    G.agents.clear();
    const mj::Value& ags = m["agents"];
    for (size_t i = 0; i < ags.size(); ++i) {
        Agent a;
        a.id = (int)i;
        a.kind = valueOrDefault(ags[i], "kind", (i < G.assignment.size() ? G.assignment[i] : 0));
        a.pos = ags[i]["pos"].asInt();
        a.fuel = ags[i]["fuel"].isNull() ? INF : ags[i]["fuel"].asInt();
        G.agents.push_back(a);
    }

    vector<vector<int>> plan = planDay(G.agents);
    cout << serializePlan(plan) << "\n" << flush;
}

int main() {
    string line;
    while (getline(cin, line)) {
        if (line.empty()) continue;
        mj::ValuePtr v;
        try {
            v = mj::parse(line);
        } catch (const exception& e) {
            cerr << "json parse error: " << e.what() << "\n";
            continue;
        }

        if (v->obj.count("daySteps")) {
            handleSetup(*v);
        } else if (v->obj.count("standings")) {
            cerr << "match finished\n";
            return 0;
        } else if (v->obj.count("agents")) {
            handleDayState(*v);
        }
    }
    return 0;
}
