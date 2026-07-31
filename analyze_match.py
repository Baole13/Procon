#!/usr/bin/env python3
import argparse
import heapq
import json
import re
from datetime import datetime, timezone
from pathlib import Path

DIR_EVEN = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 0)]
DIR_ODD = [(-1, -1), (0, -1), (1, 0), (0, 1), (-1, 1), (-1, 0)]


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def accepted_response(payload):
    if payload is None or not isinstance(payload, dict):
        return False
    for key in ("valid", "accepted", "ok", "success"):
        if key in payload:
            return bool(payload.get(key))
    status = str(payload.get("status", "")).lower()
    if status in ("ok", "accepted", "success", "valid"):
        return True
    if status in ("error", "invalid", "rejected", "failed", "failure"):
        return False
    if "error" in payload:
        return False
    if "reason" in payload and str(payload.get("reason", "")).strip():
        return False
    response_type = str(payload.get("type", "")).lower()
    return "result" in response_type or "accepted" in response_type or "action" in response_type or "assignment" in response_type


def main():
    parser = argparse.ArgumentParser(description="Analyze saved HEXUDON match replay/actions.")
    parser.add_argument("match_id")
    parser.add_argument("--team-id", default=None, help="Optional team id to report actual score for, e.g. team-A.")
    parser.add_argument("--spots", action="store_true", help="Print per-spot visits/missing for each day.")
    parser.add_argument("--compare-result", action="store_true", help="Warn when estimated-vs-actual gap is large.")
    parser.add_argument("--rules-check", action="store_true", help="Check exact day step budget, action range, fuel, terrain, and pond rules.")
    parser.add_argument("--benchmark-version", default=None, help="Write benchmarks/<version>.json entry for this match.")
    args = parser.parse_args()

    base = Path("matches") / args.match_id
    setup = load_json(base / "setup.json")
    width = setup["map"]["width"]
    height = setup["map"]["height"]
    cells = setup["map"]["cells"]
    day_steps = setup["daySteps"]
    spots = setup["spots"]
    fuel_limit = setup.get("fuelLimits", 120)
    spot_by_pos = {
        s["pos"]: (i, s["brand"], s.get("stocks", s.get("stock", s.get("amount", 1))))
        for i, s in enumerate(spots)
    }

    def terrain(pos):
        return cells[pos // width][pos % width]

    def neighbor(pos, direction):
        row, col = divmod(pos, width)
        dc, dr = (DIR_ODD if row % 2 else DIR_EVEN)[direction]
        nc, nr = col + dc, row + dr
        if nc < 0 or nc >= width or nr < 0 or nr >= height:
            return -1
        return nr * width + nc

    def move_cost(pos, status):
        cell = terrain(pos)
        if cell == 0:
            return 2, 1
        if cell == 2:
            return 3, 2
        if cell == 1:
            return (2, 2) if status == 1 else (4, 2) if status == 2 else (1, 2)
        return None

    def dijkstra(src, traffic):
        n = width * height
        dist = [10**9] * n
        fuel = [10**9] * n
        if src < 0 or src >= n or terrain(src) == 3:
            return dist, fuel
        dist[src] = 0
        fuel[src] = 0
        pq = [(0, 0, src)]
        while pq:
            du, fu, pos = heapq.heappop(pq)
            if du != dist[pos] or fu != fuel[pos]:
                continue
            cost = move_cost(pos, traffic.get(pos, 0))
            if cost is None:
                continue
            for direction in range(6):
                nxt = neighbor(pos, direction)
                if nxt < 0 or terrain(nxt) == 3:
                    continue
                nd = du + cost[0]
                nf = fu + cost[1]
                if nd < dist[nxt] or (nd == dist[nxt] and nf < fuel[nxt]):
                    dist[nxt] = nd
                    fuel[nxt] = nf
                    heapq.heappush(pq, (nd, nf, nxt))
        return dist, fuel

    daily_cap = sum(s.get("stocks", s.get("stock", s.get("amount", 1))) for s in spots)
    total_cap = daily_cap * len(day_steps)
    max_dim = max(width, height)
    if max_dim >= 32:
        map_group = "max_large"
    elif max_dim >= 24:
        map_group = "large"
    elif max_dim >= 16:
        map_group = "medium"
    else:
        map_group = "small"
    print(f"match={args.match_id} map_group={map_group} size={width}x{height} daily_cap={daily_cap} total_cap={total_cap}")

    action_days = {
        int(match.group(1))
        for path in base.glob("day-*-actions.raw.json")
        for match in [re.search(r"day-(\d+)-actions", path.name)]
        if match
    }
    expected_days = set(range(len(day_steps)))
    missing_action_days = sorted(expected_days - action_days)
    action_response_days = {
        int(match.group(1))
        for path in base.glob("day-*-action-response.json")
        for match in [re.search(r"day-(\d+)-action-response", path.name)]
        if match
    }
    assignment_response_path = base / "assignment-response.json"
    response_artifacts_expected = bool(action_response_days) or assignment_response_path.exists()
    missing_action_response_days = sorted(expected_days - action_response_days) if response_artifacts_expected else []
    invalid_action_response_days = []
    for day in sorted(action_response_days):
        response_path = base / f"day-{day}-action-response.json"
        try:
            if not accepted_response(load_json(response_path)):
                invalid_action_response_days.append(day)
        except Exception:
            invalid_action_response_days.append(day)
    assignment_response_valid = None
    if assignment_response_path.exists():
        try:
            assignment_response_valid = accepted_response(load_json(assignment_response_path))
        except Exception:
            assignment_response_valid = False
    stderr_quality = {
        day: {
            "negative_slack_routes": 0,
            "road_heavy_routes": 0,
            "oversweep_routes": 0,
        }
        for day in range(len(day_steps))
    }
    setup_assignment_reason = ""
    tanker_value_est = None
    setup_ab_3day = {}
    stderr_path = base / "bot.stderr.log"
    if stderr_path.exists():
        for line in stderr_path.read_text(encoding="utf-8", errors="replace").splitlines():
            setup_ab_match = re.search(r"setup_ab_3day", line)
            if setup_ab_match:
                setup_ab_3day = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", line))
                reason_match = re.search(r"selected_assignment_reason=([a-z_]+)", line)
                if reason_match:
                    setup_assignment_reason = reason_match.group(1)
                if "tanker_value_est" in setup_ab_3day:
                    tanker_value_est = setup_ab_3day["tanker_value_est"]
            setup_match = re.search(r"setup width=.* selected_assignment_reason=([a-z_]+)", line)
            if setup_match and not setup_assignment_reason:
                setup_assignment_reason = setup_match.group(1)
            planner_config_match = re.search(r"planner_config day=(\d+) (.*)", line)
            if planner_config_match:
                day = int(planner_config_match.group(1))
                if day in stderr_quality:
                    tail = planner_config_match.group(2)
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", tail))
                    float_pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=(-?\d+\.\d+)", tail))
                    text_pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=([A-Za-z0-9_]+)", tail))
                    for key in (
                        "cluster_count", "set_packing_topk", "route_scope", "beam", "pool",
                        "modes", "heavy", "max_visits", "deadline_mode", "ultra_fast",
                    ):
                        if key in pairs:
                            stderr_quality[day][key] = pairs[key]
                    for key in ("map_family", "time_profile"):
                        if key in text_pairs:
                            stderr_quality[day][key] = text_pairs[key]
                    if "map_scale" in float_pairs:
                        stderr_quality[day]["map_scale"] = float(float_pairs["map_scale"])
            brand_constraint_match = re.search(r"brand_constraint day=(\d+) .*max_reachable=(\d+)", line)
            if brand_constraint_match:
                day = int(brand_constraint_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["brand_max_reachable"] = int(brand_constraint_match.group(2))
            stage_timing_match = re.search(r"stage_timing day=(\d+) (.*)", line)
            if stage_timing_match:
                day = int(stage_timing_match.group(1))
                if day in stderr_quality:
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", stage_timing_match.group(2)))
                    for key in ("cluster_ms", "route_gen_ms", "set_packing_ms", "rollout_ms", "hub_forecast_ms"):
                        if key in pairs:
                            stderr_quality[day][key] = pairs[key]
            set_packing_match = re.search(r"set_packing day=(\d+) .*states=(\d+).*selected_exact=(\d+).*selected_brands=(\d+).*interrupted=(\d+)", line)
            if set_packing_match:
                day = int(set_packing_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["set_packing_states"] = int(set_packing_match.group(2))
                    stderr_quality[day]["set_packing_exact"] = int(set_packing_match.group(3))
                    stderr_quality[day]["set_packing_brands"] = int(set_packing_match.group(4))
                    stderr_quality[day]["set_packing_interrupted"] = int(set_packing_match.group(5))
                    mode_match = re.search(r"mode=([a-z_]+)", line)
                    if mode_match:
                        stderr_quality[day]["set_packing_mode"] = mode_match.group(1)
                    feasible_match = re.search(r"feasible_complete_states=(\d+)", line)
                    if feasible_match:
                        stderr_quality[day]["set_packing_feasible_states"] = int(feasible_match.group(1))
            preservation_match = re.search(r"set_packing_preservation day=(\d+) .*selected_exact=(\d+).*simulated_exact=(\d+).*simulated_server=(\d+).*simulated_negative_slack=(-?\d+)", line)
            if preservation_match:
                day = int(preservation_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["set_packing_selected_exact"] = int(preservation_match.group(2))
                    stderr_quality[day]["set_packing_simulated_exact"] = int(preservation_match.group(3))
                    stderr_quality[day]["set_packing_simulated_server"] = int(preservation_match.group(4))
                    stderr_quality[day]["set_packing_simulated_negative_slack"] = int(preservation_match.group(5))
                    stderr_quality[day]["set_packing_preservation_gap"] = max(
                        0,
                        int(preservation_match.group(2)) - int(preservation_match.group(3)),
                    )
            topk_match = re.search(r"set_packing_topk day=(\d+) .*leaves=(\d+).*exact_rescore_limit=(\d+).*exact_rescored=(\d+).*feasible_exact_found=(\d+)", line)
            if topk_match:
                day = int(topk_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["set_packing_topk_leaves"] = int(topk_match.group(2))
                    stderr_quality[day]["set_packing_exact_rescore_limit"] = int(topk_match.group(3))
                    stderr_quality[day]["set_packing_exact_rescored"] = int(topk_match.group(4))
                    stderr_quality[day]["set_packing_feasible_exact_found"] = int(topk_match.group(5))
            rendezvous_match = re.search(r"rendezvous_candidate day=(\d+).*shared_step=(-?\d+).*portions_saved=(-?\d+)", line)
            if rendezvous_match:
                day = int(rendezvous_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["rendezvous_candidates"] = stderr_quality[day].get("rendezvous_candidates", 0) + 1
                    if int(rendezvous_match.group(2)) >= 0:
                        stderr_quality[day]["rendezvous_executable"] = stderr_quality[day].get("rendezvous_executable", 0) + 1
                    stderr_quality[day]["rendezvous_saved_portions"] = stderr_quality[day].get("rendezvous_saved_portions", 0) + int(rendezvous_match.group(3))
            patrol_match = re.search(r"patrol day=(\d+) .* steps=(\d+) visits=(\d+) .* slack=(-?\d+) road_use=(\d+)", line)
            if patrol_match:
                day = int(patrol_match.group(1))
                if day in stderr_quality:
                    steps = int(patrol_match.group(2))
                    visits = int(patrol_match.group(3))
                    slack = int(patrol_match.group(4))
                    road_use = int(patrol_match.group(5))
                    if slack < 0:
                        stderr_quality[day]["negative_slack_routes"] += 1
                    if road_use >= max(6, day_steps[day] // 10):
                        stderr_quality[day]["road_heavy_routes"] += 1
                    if steps >= day_steps[day] - 2 and visits <= 1:
                        stderr_quality[day]["oversweep_routes"] += 1
            compare_match = re.search(r"plan_compare day=(\d+) fast_brands=", line)
            if compare_match:
                day = int(compare_match.group(1))
                if day in stderr_quality:
                    pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", line))
                    for key in (
                        "fast_brands", "fast_portions", "fast_server", "fast_debt",
                        "fast_neg_slack", "fast_road_heavy", "fast_low_fuel", "fast_fuel_risk",
                        "strong_brands", "strong_portions", "strong_server", "strong_debt",
                        "strong_neg_slack", "strong_road_heavy", "strong_low_fuel", "strong_fuel_risk",
                    ):
                        if key in pairs:
                            stderr_quality[day][key] = int(pairs[key])
            selected_match = re.search(r"plan_compare day=(\d+) selected=([a-z_]+)(?: selected_reason=([a-z_]+))?", line)
            if selected_match:
                day = int(selected_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["selected_profile"] = selected_match.group(2)
                    if selected_match.group(3):
                        stderr_quality[day]["selected_reason"] = selected_match.group(3)
            pre_summary_match = re.search(r"pre_repair_summary day=(\d+)", line)
            if pre_summary_match:
                day = int(pre_summary_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["has_pre_repair_summary"] = 1
            final_summary_match = re.search(r"final_plan_summary day=(\d+) (.*)", line)
            if final_summary_match:
                day = int(final_summary_match.group(1))
                if day in stderr_quality:
                    tail = final_summary_match.group(2)
                    pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", tail))
                    text_pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=([a-z_]+)", tail))
                    stderr_quality[day]["has_final_plan_summary"] = 1
                    for key in (
                        "final_assigned_stock", "final_server_est", "final_negative_slack",
                        "final_ghost_visits", "final_active_patrols", "final_daily_brands",
                        "final_road_use", "final_team_fuel_risk", "final_fuel_opportunity_cost",
                        "reachable_stock_next_day",
                        "final_stock_execution_efficiency", "final_visit_usefulness",
                        "final_end_to_end_visit_yield",
                        "final_assigned_visits", "final_exact_collected",
                    ):
                        if key in pairs:
                            stderr_quality[day][key] = int(pairs[key])
                    if "selected_plan" in text_pairs:
                        stderr_quality[day]["selected_profile"] = text_pairs["selected_plan"]
                    if "selected_reason" in text_pairs:
                        stderr_quality[day]["selected_reason"] = text_pairs["selected_reason"]
                    if "serialized_action_hash" in pairs:
                        stderr_quality[day]["serialized_action_hash"] = int(pairs["serialized_action_hash"])
            improvement_match = re.search(r"best_plan_improvement_timeline day=(\d+) stage=([a-z_]+) (.*)", line)
            if improvement_match:
                day = int(improvement_match.group(1))
                if day in stderr_quality:
                    stage = improvement_match.group(2)
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", improvement_match.group(3)))
                    timeline = stderr_quality[day].setdefault("best_plan_improvement_timeline", [])
                    timeline.append({"stage": stage, **pairs})
            rollout_match = re.search(r"rollout_summary day=(\d+) (.*)", line)
            if rollout_match:
                day = int(rollout_match.group(1))
                if day in stderr_quality:
                    tail = rollout_match.group(2)
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", tail))
                    text_pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=([a-z_]+)", tail))
                    summaries = stderr_quality[day].setdefault("rollout_summaries", [])
                    summaries.append({**text_pairs, **pairs})
                    if "future_server" in pairs:
                        stderr_quality[day]["rollout_future_server"] = max(
                            stderr_quality[day].get("rollout_future_server", 0),
                            pairs["future_server"],
                        )
                    if "reachable_next" in pairs:
                        stderr_quality[day]["rollout_reachable_next"] = max(
                            stderr_quality[day].get("rollout_reachable_next", 0),
                            pairs["reachable_next"],
                        )
            selected_rollout_match = re.search(r"selected_rollout_policy day=(\d+) (.*)", line)
            if selected_rollout_match:
                day = int(selected_rollout_match.group(1))
                if day in stderr_quality:
                    tail = selected_rollout_match.group(2)
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", tail))
                    text_pairs = dict((key, value) for key, value in re.findall(r"([a-z_]+)=([a-z_]+)", tail))
                    if "policy" in text_pairs:
                        stderr_quality[day]["selected_rollout_policy"] = text_pairs["policy"]
                    if "future_server" in pairs:
                        stderr_quality[day]["selected_rollout_future_server"] = pairs["future_server"]
                    if "total_server" in pairs:
                        stderr_quality[day]["selected_rollout_total_server"] = pairs["total_server"]
                    if "collapse_days" in pairs:
                        stderr_quality[day]["selected_rollout_collapse_days"] = pairs["collapse_days"]
            rollout_exact_match = re.search(r"rollout_exact day=(\d+) (.*)", line)
            if rollout_exact_match:
                day = int(rollout_exact_match.group(1))
                if day in stderr_quality:
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", rollout_exact_match.group(2)))
                    stderr_quality[day]["rollout_exact_future_brand_days"] = max(
                        stderr_quality[day].get("rollout_exact_future_brand_days", 0),
                        pairs.get("future_brand_days", 0),
                    )
                    stderr_quality[day]["rollout_exact_future_portions"] = max(
                        stderr_quality[day].get("rollout_exact_future_portions", 0),
                        pairs.get("future_portions", 0),
                    )
            exact_sim_match = re.search(r"exact_sim_summary day=(\d+) (.*)", line)
            if exact_sim_match:
                day = int(exact_sim_match.group(1))
                if day in stderr_quality:
                    pairs = dict((key, int(value)) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", exact_sim_match.group(2)))
                    for key in (
                        "valid", "exact_collected", "daily_brands",
                        "final_negative_slack", "ghost_visits", "server_est",
                        "effective_est", "assigned_stock", "min_final_fuel",
                        "final_positions_hash", "assigned_visits", "exact_gap",
                    ):
                        if key in pairs:
                            stderr_quality[day][f"exact_{key}"] = pairs[key]
            ghost_compression_match = re.search(r"ghost_compression day=(\d+)", line)
            if ghost_compression_match:
                day = int(ghost_compression_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["ghost_compressions"] = stderr_quality[day].get("ghost_compressions", 0) + 1
            ghost_reassign_match = re.search(r"ghost_reassign_repair day=(\d+)", line)
            if ghost_reassign_match:
                day = int(ghost_reassign_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["ghost_reassign_repairs"] = stderr_quality[day].get("ghost_reassign_repairs", 0) + 1
            timing_match = re.search(r"planner_timing day=(\d+) compute_ms=(\d+) budget_ms=(\d+)(.*)", line)
            if timing_match:
                day = int(timing_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["compute_ms"] = int(timing_match.group(2))
                    stderr_quality[day]["budget_ms"] = int(timing_match.group(3))
                    tail = timing_match.group(4)
                    if "emergency=1" in tail:
                        stderr_quality[day]["selected_profile"] = stderr_quality[day].get("selected_profile") or "emergency"
                    if "fast_only=1" in tail:
                        stderr_quality[day]["fast_only"] = 1
            repair_match = re.search(r"last_portion_repair day=(\d+) spot=(\d+) agent=(\d+) portions=(\d+) server=(\d+)", line)
            if repair_match:
                day = int(repair_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["last_portion_repairs"] = stderr_quality[day].get("last_portion_repairs", 0) + 1
            risk_repair_match = re.search(r"risk_repair day=(\d+) agent=(\d+) portions=(\d+) server=(\d+) negative_slack=(\d+)", line)
            if risk_repair_match:
                day = int(risk_repair_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["risk_repairs"] = stderr_quality[day].get("risk_repairs", 0) + 1
                    stderr_quality[day]["last_risk_repair_agent"] = int(risk_repair_match.group(2))
            anytime_repair_match = re.search(r"anytime_repair day=(\d+) .*type=([a-z_]+).*accepted=(\d+).*exact_delta=(-?\d+)", line)
            if anytime_repair_match:
                day = int(anytime_repair_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["anytime_repairs"] = stderr_quality[day].get("anytime_repairs", 0) + int(anytime_repair_match.group(3))
                    stderr_quality[day]["anytime_exact_delta"] = stderr_quality[day].get("anytime_exact_delta", 0) + int(anytime_repair_match.group(4))
            cache_match = re.search(r"state_cache day=(\d+) .*retry_index=(\d+) cache_hit=(\d+)", line)
            if cache_match:
                day = int(cache_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["retry_count"] = max(stderr_quality[day].get("retry_count", 0), int(cache_match.group(2)))
                    stderr_quality[day]["cache_hits"] = stderr_quality[day].get("cache_hits", 0) + int(cache_match.group(3))
            cached_return_match = re.search(r"state_cache day=(\d+) returned_cached_plan=1", line)
            if cached_return_match:
                day = int(cached_return_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["cached_plan_returns"] = stderr_quality[day].get("cached_plan_returns", 0) + 1
            debt_match = re.search(r"spot_debt day=(\d+) .*debt_committed=(\d+)", line)
            if debt_match:
                day = int(debt_match.group(1))
                if day in stderr_quality:
                    committed = int(debt_match.group(2))
                    stderr_quality[day]["debt_commits"] = stderr_quality[day].get("debt_commits", 0) + committed
                    if committed == 0:
                        stderr_quality[day]["debt_duplicate_updates"] = stderr_quality[day].get("debt_duplicate_updates", 0) + 1
            stats_match = re.search(
                r"planner_stats day=(\d+) dijkstra_calls=(\d+) cache_hits=(\d+) "
                r"pareto_labels=(\d+) patrol_labels=(\d+) team_states=(\d+) "
                r"dominance_pruned=(\d+) deadline_checks=(\d+) timeout_stage=(\S+)",
                line,
            )
            if stats_match:
                day = int(stats_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day].update({
                        "dijkstra_calls": int(stats_match.group(2)),
                        "path_cache_hits": int(stats_match.group(3)),
                        "pareto_labels": int(stats_match.group(4)),
                        "patrol_labels": int(stats_match.group(5)),
                        "team_states": int(stats_match.group(6)),
                        "dominance_pruned": int(stats_match.group(7)),
                        "deadline_checks": int(stats_match.group(8)),
                        "timeout_stage": stats_match.group(9),
                    })
            refuel_match = re.search(
                r"refuel_timeline day=(\d+) planned=(\d+) must_serve=(\d+) "
                r"feasible=(\d+) failed=(\d+)",
                line,
            )
            if refuel_match:
                day = int(refuel_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day].update({
                        "planned_refuels": int(refuel_match.group(2)),
                        "must_serve_refuels": int(refuel_match.group(3)),
                        "feasible_refuels": int(refuel_match.group(4)),
                        "failed_rendezvous": int(refuel_match.group(5)),
                    })
            refuel_reason_match = re.search(r"refuel day=(\d+) .* tanker_reason=([a-z_]+)", line)
            if refuel_reason_match:
                day = int(refuel_reason_match.group(1))
                if day in stderr_quality:
                    stderr_quality[day]["tanker_reason"] = refuel_reason_match.group(2)
                    streak_match = re.search(r"tanker_idle_streak=(\d+)", line)
                    if streak_match:
                        stderr_quality[day]["logged_tanker_idle_streak"] = int(streak_match.group(1))
            fast_tanker_match = re.search(r"fast_tanker day=(\d+) .* tanker_reason=([a-z_]+)", line)
            if fast_tanker_match:
                day = int(fast_tanker_match.group(1))
                if day in stderr_quality and "tanker_reason" not in stderr_quality[day]:
                    stderr_quality[day]["tanker_reason"] = fast_tanker_match.group(2)
                    streak_match = re.search(r"tanker_idle_streak=(\d+)", line)
                    if streak_match:
                        stderr_quality[day]["logged_tanker_idle_streak"] = int(streak_match.group(1))

    total = 0
    conservative_total = 0
    reachable_cap_est = 0
    total_missing_by_spot = {i: 0 for i in range(len(spots))}
    repeat_missing_days_by_spot = {i: 0 for i in range(len(spots))}
    day_rows = []
    for day in range(len(day_steps)):
        state_path = base / f"day-{day}-state.json"
        action_path = base / f"day-{day}-actions.raw.json"
        if not state_path.exists() or not action_path.exists():
            continue
        state = load_json(state_path)
        actions = load_json(action_path)
        traffic = {t["pos"]: t["status"] for t in state["traffics"]}
        reachable_today = 0
        reachable_spots = set()
        for agent in state["agents"]:
            if agent.get("kind", 0) != 0:
                continue
            dist, fuel_dist = dijkstra(agent["pos"], traffic)
            agent_fuel = agent.get("fuel", fuel_limit)
            for i, spot in enumerate(spots):
                if dist[spot["pos"]] <= day_steps[day] and fuel_dist[spot["pos"]] <= agent_fuel:
                    reachable_spots.add(i)
        for i in reachable_spots:
            reachable_today += spots[i].get("stocks", spots[i].get("stock", spots[i].get("amount", 1)))
        reachable_cap_est += min(daily_cap, reachable_today)
        by_spot = {i: 0 for i in range(len(spots))}
        brands = set()
        active = 0
        low_fuel = 0
        idle_patrols = 0
        tanker_idle = 0
        fuel_ends = []
        fuel_end_by_agent = {}
        pos_end_by_agent = {}
        fuel_used = 0
        road_uses = 0
        road_wait_steps = 0
        end_on_road_routes = 0
        patrol_end_positions = []
        invalid = []
        budget_violations = []

        for agent_id, (agent, route) in enumerate(zip(state["agents"], actions)):
            pos = agent["pos"]
            fuel = agent.get("fuel", fuel_limit)
            used = 0
            seen = set()
            moved = False
            if agent["kind"] == 0 and pos in spot_by_pos:
                sid, brand, _stock = spot_by_pos[pos]
                seen.add(sid)
                by_spot[sid] += 1
                brands.add(brand)

            for action in route:
                if action < 0:
                    if terrain(pos) == 1:
                        road_wait_steps += -action
                    used += -action
                    continue
                if action >= 6:
                    invalid.append((agent_id, pos, action))
                    break
                moved = True
                nxt = neighbor(pos, action)
                cost = move_cost(pos, traffic.get(pos, 0))
                if cost is None or nxt < 0 or terrain(nxt) == 3:
                    invalid.append((agent_id, pos, action))
                    break
                fuel_cost = 0 if agent["kind"] == 1 else cost[1]
                if fuel < fuel_cost:
                    invalid.append((agent_id, pos, action))
                    break
                if terrain(pos) == 1:
                    road_uses += 1
                used += cost[0]
                fuel -= fuel_cost
                pos = nxt
                if agent["kind"] == 0 and pos in spot_by_pos:
                    sid, brand, _stock = spot_by_pos[pos]
                    if sid not in seen:
                        seen.add(sid)
                        by_spot[sid] += 1
                        brands.add(brand)
            if used != day_steps[day]:
                budget_violations.append((agent_id, used, day_steps[day]))
            if agent["kind"] == 0 and terrain(pos) == 1:
                end_on_road_routes += 1
            if agent["kind"] == 0:
                patrol_end_positions.append(pos)

            if agent["kind"] == 0 and moved:
                active += 1
            if agent["kind"] == 0:
                if not moved:
                    idle_patrols += 1
                fuel_ends.append(fuel)
                fuel_used += max(0, agent.get("fuel", fuel_limit) - fuel)
                if fuel <= max(18, fuel_limit // 5):
                    low_fuel += 1
                fuel_end_by_agent[agent_id] = fuel
                pos_end_by_agent[agent_id] = pos
            elif not moved:
                tanker_idle += 1

        actual_refuels = 0
        position_mismatch = 0
        fuel_mismatch = 0
        next_state_path = base / f"day-{day + 1}-state.json"
        if next_state_path.exists():
            next_state = load_json(next_state_path)
            for agent_id, pos_end in pos_end_by_agent.items():
                if agent_id >= len(next_state.get("agents", [])):
                    continue
                if next_state["agents"][agent_id].get("pos") != pos_end:
                    position_mismatch += 1
            for agent_id, fuel_end in fuel_end_by_agent.items():
                if agent_id >= len(next_state.get("agents", [])):
                    continue
                next_fuel = next_state["agents"][agent_id].get("fuel", fuel_end)
                if next_fuel == fuel_limit and next_fuel > fuel_end:
                    actual_refuels += 1
                elif next_fuel != fuel_end:
                    fuel_mismatch += 1

        assigned_visits = sum(by_spot.values())
        capped = sum(min(by_spot[i], spots[i].get("stocks", spots[i].get("stock", spots[i].get("amount", 1)))) for i in by_spot)
        ghost_visits = max(0, assigned_visits - capped)
        missing = sum(max(0, spots[i].get("stocks", 1) - by_spot[i]) for i in by_spot)
        for i in by_spot:
            stock = spots[i].get("stocks", spots[i].get("stock", spots[i].get("amount", 1)))
            spot_missing = max(0, stock - by_spot[i])
            total_missing_by_spot[i] += spot_missing
            if spot_missing > 0:
                repeat_missing_days_by_spot[i] += 1
        top_missing = sorted(
            (
                (
                    max(0, spots[i].get("stocks", spots[i].get("stock", spots[i].get("amount", 1))) - by_spot[i]),
                    spots[i].get("stocks", spots[i].get("stock", spots[i].get("amount", 1))),
                    i,
                )
                for i in by_spot
            ),
            reverse=True,
        )
        top_missing_text = ",".join(
            f"{spots[i]['pos']}:{miss}/{stock}"
            for miss, stock, i in top_missing[:4]
            if miss > 0
        )
        last_portion_missing = sum(1 for miss, stock, _i in top_missing if miss == 1 and stock >= 4)
        road_wait_penalty = road_wait_steps // 8
        end_road_penalty = end_on_road_routes // 2
        conservative = max(0, capped - max(0, road_uses // 12) - road_wait_penalty - end_road_penalty - max(0, low_fuel - 1))
        min_fuel_end = min(fuel_ends) if fuel_ends else 0
        quality = stderr_quality.get(day, {})
        total_patrols = active + idle_patrols
        if quality.get("has_final_plan_summary"):
            capped = quality.get("final_assigned_stock", capped)
            conservative = quality.get("final_server_est", conservative)
            ghost_visits = quality.get("final_ghost_visits", ghost_visits)
            active = quality.get("final_active_patrols", active)
            road_uses = quality.get("final_road_use", road_uses)
            reachable_today = quality.get("reachable_stock_next_day", reachable_today)
            idle_patrols = max(0, total_patrols - active)
        total += capped
        conservative_total += conservative
        static_vs_server_gap = capped - conservative
        optimistic_capture = int(100 * capped / daily_cap) if daily_cap else 0
        assignment_coverage = optimistic_capture
        stock_execution_efficiency = int(100 * conservative / max(1, capped)) if capped else 100
        visit_usefulness = int(100 * capped / max(1, assigned_visits)) if assigned_visits else 100
        end_to_end_visit_yield = int(100 * conservative / max(1, assigned_visits)) if assigned_visits else 100
        execution_efficiency = end_to_end_visit_yield
        effective_capture = int(100 * conservative / daily_cap) if daily_cap else 0
        ghost_stock = max(0, capped - conservative)
        fuel_used_per_portion = fuel_used // max(1, capped)
        duplicate_endpoints = len(patrol_end_positions) - len(set(patrol_end_positions))
        day_rows.append({
            "day": day,
            "assigned_visits": assigned_visits,
            "capped": capped,
            "conservative": conservative,
            "assignment_coverage": assignment_coverage,
            "execution_efficiency": execution_efficiency,
            "stock_execution_efficiency": stock_execution_efficiency,
            "visit_usefulness": visit_usefulness,
            "end_to_end_visit_yield": end_to_end_visit_yield,
            "optimistic_capture": optimistic_capture,
            "effective_capture": effective_capture,
            "ghost_stock": ghost_stock,
            "fuel_used": fuel_used,
            "fuel_used_per_portion": fuel_used_per_portion,
            "ghost_visits": ghost_visits,
            "static_vs_server_gap": static_vs_server_gap,
            "brands": quality.get("final_daily_brands", len(brands)),
            "active": active,
            "idle": idle_patrols,
            "patrols": active + idle_patrols,
            "missing": missing,
            "last_portion_missing": last_portion_missing,
            "low_fuel": low_fuel,
            "min_fuel_end": min_fuel_end,
            "road_uses": road_uses,
            "road_wait_steps": road_wait_steps,
            "end_on_road_routes": end_on_road_routes,
            "route_signature_duplicates": duplicate_endpoints,
            "invalid": len(invalid),
            "budget_invalid": len(budget_violations),
            "reachable_cap_est": min(daily_cap, reachable_today),
            "negative_slack_routes": quality.get("negative_slack_routes", 0),
            "negative_slack_final": quality.get("final_negative_slack", quality.get("negative_slack_routes", 0)),
            "road_heavy_routes": quality.get("road_heavy_routes", 0),
            "oversweep_routes": quality.get("oversweep_routes", 0),
            "selected_profile": quality.get("selected_profile"),
            "selected_reason": quality.get("selected_reason"),
            "map_family": quality.get("map_family", ""),
            "time_profile": quality.get("time_profile", ""),
            "cluster_count": quality.get("cluster_count", 0),
            "set_packing_topk": quality.get("set_packing_topk", 0),
            "route_scope": quality.get("route_scope", 0),
            "compute_ms": quality.get("compute_ms"),
            "budget_ms": quality.get("budget_ms"),
            "fast_low_fuel": quality.get("fast_low_fuel"),
            "strong_low_fuel": quality.get("strong_low_fuel"),
            "fast_fuel_risk": quality.get("fast_fuel_risk"),
            "strong_fuel_risk": quality.get("strong_fuel_risk"),
            "fast_debt": quality.get("fast_debt"),
            "strong_debt": quality.get("strong_debt"),
            "planned_refuels": quality.get("planned_refuels", 0),
            "must_serve_refuels": quality.get("must_serve_refuels", 0),
            "feasible_refuels": quality.get("feasible_refuels", 0),
            "failed_rendezvous": quality.get("failed_rendezvous", 0),
            "dijkstra_calls": quality.get("dijkstra_calls", 0),
            "path_cache_hits": quality.get("path_cache_hits", 0),
            "pareto_labels": quality.get("pareto_labels", 0),
            "team_states": quality.get("team_states", 0),
            "timeout_stage": quality.get("timeout_stage", "none"),
            "actual_refuels": actual_refuels,
            "position_mismatch": position_mismatch,
            "fuel_mismatch": fuel_mismatch,
            "tanker_idle": tanker_idle,
            "tanker_reason": quality.get("tanker_reason"),
            "logged_tanker_idle_streak": quality.get("logged_tanker_idle_streak"),
            "last_portion_repairs": quality.get("last_portion_repairs", 0),
            "risk_repairs": quality.get("risk_repairs", 0),
            "ghost_compressions": quality.get("ghost_compressions", 0),
            "ghost_reassign_repairs": quality.get("ghost_reassign_repairs", 0),
            "retry_count": quality.get("retry_count", 0),
            "cached_plan_returns": quality.get("cached_plan_returns", 0),
            "debt_duplicate_updates": quality.get("debt_duplicate_updates", 0),
            "has_final_plan_summary": quality.get("has_final_plan_summary", 0),
            "serialized_action_hash": quality.get("serialized_action_hash"),
            "final_team_fuel_risk": quality.get("final_team_fuel_risk"),
            "final_fuel_opportunity_cost": quality.get("final_fuel_opportunity_cost"),
            "selected_rollout_policy": quality.get("selected_rollout_policy", ""),
            "selected_rollout_future_server": quality.get("selected_rollout_future_server", 0),
            "selected_rollout_total_server": quality.get("selected_rollout_total_server", 0),
            "selected_rollout_collapse_days": quality.get("selected_rollout_collapse_days", 0),
            "rollout_future_server": quality.get("rollout_future_server", 0),
            "rollout_reachable_next": quality.get("rollout_reachable_next", 0),
            "exact_valid": quality.get("exact_valid"),
            "exact_collected": quality.get("exact_exact_collected"),
            "exact_assigned_stock": quality.get("exact_assigned_stock"),
            "exact_assigned_visits": quality.get("exact_assigned_visits"),
            "exact_gap": quality.get("exact_exact_gap", max(0, quality.get("exact_assigned_stock", capped) - quality.get("exact_exact_collected", capped))),
            "exact_server_est": quality.get("exact_server_est"),
            "exact_effective_est": quality.get("exact_effective_est"),
            "brand_max_reachable": quality.get("brand_max_reachable", 0),
            "set_packing_states": quality.get("set_packing_states", 0),
            "set_packing_mode": quality.get("set_packing_mode", ""),
            "set_packing_feasible_states": quality.get("set_packing_feasible_states", 0),
            "set_packing_exact_rescored": quality.get("set_packing_exact_rescored", 0),
            "set_packing_feasible_exact_found": quality.get("set_packing_feasible_exact_found", 0),
            "set_packing_preservation_gap": quality.get("set_packing_preservation_gap", 0),
            "set_packing_simulated_exact": quality.get("set_packing_simulated_exact", 0),
            "set_packing_interrupted": quality.get("set_packing_interrupted", 0),
            "rendezvous_executable": quality.get("rendezvous_executable", 0),
            "rendezvous_candidates": quality.get("rendezvous_candidates", 0),
            "rollout_exact_future_brand_days": quality.get("rollout_exact_future_brand_days", 0),
            "rollout_exact_future_portions": quality.get("rollout_exact_future_portions", 0),
            "anytime_repairs": quality.get("anytime_repairs", 0),
            "anytime_exact_delta": quality.get("anytime_exact_delta", 0),
            "best_plan_improvement_timeline": quality.get("best_plan_improvement_timeline", []),
        })
        print(
            f"day={day} assigned={assigned_visits} capped={capped}/{daily_cap} conservative={conservative} "
            f"assignment_coverage={assignment_coverage}% stock_execution_efficiency={stock_execution_efficiency}% "
            f"visit_usefulness={visit_usefulness}% end_to_end_visit_yield={end_to_end_visit_yield}% execution_efficiency={execution_efficiency}% "
            f"capture={optimistic_capture}% effective_capture={effective_capture}% ghost_stock={ghost_stock} ghost_visits={ghost_visits} "
            f"fuel_used_per_portion={fuel_used_per_portion} "
            f"brands={quality.get('final_daily_brands', len(brands))} active={active} idle={idle_patrols} "
            f"missing={missing} low_fuel={low_fuel} min_fuel_end={min_fuel_end} "
            f"road_uses={road_uses} road_wait_steps={road_wait_steps} end_on_road={end_on_road_routes} invalid={len(invalid)} "
            f"route_signature_duplicates={duplicate_endpoints} "
            f"budget_invalid={len(budget_violations)} static_vs_server_gap={static_vs_server_gap} "
            f"negative_slack_routes={quality.get('negative_slack_routes', 0)} "
            f"negative_slack_final={quality.get('final_negative_slack', quality.get('negative_slack_routes', 0))} "
            f"road_heavy_routes={quality.get('road_heavy_routes', 0)} "
            f"oversweep_routes={quality.get('oversweep_routes', 0)} "
            f"selected_profile={quality.get('selected_profile', '')} "
            f"selected_reason={quality.get('selected_reason', '')} "
            f"map_family={quality.get('map_family', '')} time_profile={quality.get('time_profile', '')} "
            f"clusters={quality.get('cluster_count', '')} set_packing_topk={quality.get('set_packing_topk', '')} route_scope={quality.get('route_scope', '')} "
            f"selected_rollout_policy={quality.get('selected_rollout_policy', '')} "
            f"rollout_future_server={quality.get('selected_rollout_future_server', '')} "
            f"rollout_total_server={quality.get('selected_rollout_total_server', '')} "
            f"rollout_collapse_days={quality.get('selected_rollout_collapse_days', '')} "
            f"compute_ms={quality.get('compute_ms', '')} budget_ms={quality.get('budget_ms', '')} "
            f"stage_ms={quality.get('cluster_ms', '')}/{quality.get('route_gen_ms', '')}/{quality.get('set_packing_ms', '')}/{quality.get('rollout_ms', '')} "
            f"refuels={quality.get('feasible_refuels', 0)}/{quality.get('planned_refuels', 0)} "
            f"actual_refuels={actual_refuels} tanker_idle={tanker_idle} "
            f"position_mismatch={position_mismatch} fuel_mismatch={fuel_mismatch} "
            f"tanker_reason={quality.get('tanker_reason', '')} "
            f"failed_rendezvous={quality.get('failed_rendezvous', 0)} "
            f"dijkstra={quality.get('dijkstra_calls', 0)} cache_hits={quality.get('path_cache_hits', 0)} "
            f"timeout_stage={quality.get('timeout_stage', 'none')} "
            f"fast_low_fuel={quality.get('fast_low_fuel', '')} strong_low_fuel={quality.get('strong_low_fuel', '')} "
            f"fast_debt={quality.get('fast_debt', '')} strong_debt={quality.get('strong_debt', '')} "
            f"last_portion_missing={last_portion_missing} repairs={quality.get('last_portion_repairs', 0)} "
            f"risk_repairs={quality.get('risk_repairs', 0)} ghost_compressions={quality.get('ghost_compressions', 0)} "
            f"ghost_reassign_repairs={quality.get('ghost_reassign_repairs', 0)} "
            f"reachable_stock_next_day={quality.get('reachable_stock_next_day', '')} "
            f"retry_count={quality.get('retry_count', 0)} cached_returns={quality.get('cached_plan_returns', 0)} debt_duplicate_updates={quality.get('debt_duplicate_updates', 0)} "
            f"final_summary={quality.get('has_final_plan_summary', 0)} action_hash={quality.get('serialized_action_hash', '')} "
            f"exact_valid={quality.get('exact_valid', '')} exact_collected={quality.get('exact_exact_collected', '')} "
            f"exact_assigned={quality.get('exact_assigned_stock', '')} exact_assigned_visits={quality.get('exact_assigned_visits', '')} "
            f"exact_gap={quality.get('exact_exact_gap', max(0, quality.get('exact_assigned_stock', capped) - quality.get('exact_exact_collected', capped)))} "
            f"exact_effective_est={quality.get('exact_effective_est', quality.get('exact_server_est', ''))} "
            f"brand_max_reachable={quality.get('brand_max_reachable', '')} "
            f"set_packing_mode={quality.get('set_packing_mode', '')} "
            f"set_packing_states={quality.get('set_packing_states', '')} feasible_states={quality.get('set_packing_feasible_states', '')} "
            f"exact_rescored={quality.get('set_packing_exact_rescored', '')} "
            f"set_packing_sim_exact={quality.get('set_packing_simulated_exact', '')} set_packing_gap={quality.get('set_packing_preservation_gap', '')} "
            f"rendezvous_exec={quality.get('rendezvous_executable', 0)}/{quality.get('rendezvous_candidates', 0)} "
            f"rollout_exact_brands={quality.get('rollout_exact_future_brand_days', '')} rollout_exact_portions={quality.get('rollout_exact_future_portions', '')} "
            f"anytime_repairs={quality.get('anytime_repairs', 0)} anytime_exact_delta={quality.get('anytime_exact_delta', 0)} "
            f"top_missing={top_missing_text}"
        )
        if args.rules_check and (invalid or budget_violations):
            for agent_id, pos, action in invalid[:8]:
                print(f"  RULE invalid_move agent={agent_id} pos={pos} action={action}")
            for agent_id, used, budget in budget_violations[:8]:
                print(f"  RULE budget agent={agent_id} used_steps={used} day_steps={budget}")
        if args.spots:
            for i, spot in enumerate(spots):
                stock = spot.get("stocks", spot.get("stock", spot.get("amount", 1)))
                miss = max(0, stock - by_spot[i])
                print(
                    f"  spot_id={i} brand={spot['brand']} pos={spot['pos']} "
                    f"stock={stock} visits={by_spot[i]} missing={miss}"
                )

    result_path = base / "result.json"
    chosen = None
    if result_path.exists():
        result = load_json(result_path)
        standings = result.get("standings", [])
        if args.team_id:
            chosen = next((row for row in standings if row.get("team_id") == args.team_id), None)
        if chosen:
            ratio = int(100 * chosen["udon_total"] / total_cap) if total_cap else 0
            gap = total - chosen["udon_total"]
            actual_gap_to_conservative = conservative_total - chosen["udon_total"]
            print(
                f"actual team={chosen['team_id']} total={chosen['udon_total']} ratio={ratio}% "
                f"types={chosen['udon_types']} daily_types={chosen['daily_types_sum']} "
                f"response_ms={chosen['response_ms_total']} estimated_gap={gap} "
                f"actual_gap_to_conservative={actual_gap_to_conservative}"
            )
        elif standings:
            print("standings:")
            for row in standings:
                ratio = int(100 * row["udon_total"] / total_cap) if total_cap else 0
                print(
                    f"  team={row['team_id']} rank={row['rank']} total={row['udon_total']} "
                    f"ratio={ratio}% types={row['udon_types']} daily_types={row['daily_types_sum']} "
                    f"response_ms={row['response_ms_total']}"
                )
            print("actual team=unknown pass --team-id to compute own actual gap")

    top_missing_total = sorted(
        (
            (missing, spots[i].get("stocks", spots[i].get("stock", spots[i].get("amount", 1))), i)
            for i, missing in total_missing_by_spot.items()
        ),
        reverse=True,
    )
    top_missing_total_text = ",".join(
        f"{spots[i]['pos']}:{missing}/{stock * len(day_steps)}d{repeat_missing_days_by_spot.get(i, 0)}"
        for missing, stock, i in top_missing_total[:6]
        if missing > 0
    )
    profile_counts = {}
    profile_compute = {}
    map_family_counts = {}
    time_profile_counts = {}
    selected_reasons = {}
    strong_rejected_count = 0
    strong_over_budget_days = []
    planned_refuels_total = 0
    feasible_refuels_total = 0
    failed_rendezvous_total = 0
    actual_refuels_total = 0
    tanker_idle_days = 0
    tanker_idle_streak = 0
    max_tanker_idle_streak = 0
    fuel_collapse_days = []
    last_portion_missing_total = 0
    last_portion_repairs_total = 0
    road_wait_steps_total = 0
    end_on_road_total = 0
    assigned_visits_total = 0
    ghost_visits_total = 0
    position_mismatch_total = 0
    fuel_mismatch_total = 0
    route_signature_duplicates_total = 0
    risk_repairs_total = 0
    ghost_reassign_repairs_total = 0
    cached_plan_returns_total = 0
    retry_count_total = 0
    debt_duplicate_updates_total = 0
    final_negative_slack_total = 0
    reachable_stock_next_day_total = 0
    fuel_used_total = 0
    rollout_policy_counts = {}
    selected_rollout_future_total = 0
    selected_rollout_total_total = 0
    brand_constraint_loss_days = []
    set_packing_states_total = 0
    set_packing_feasible_states_total = 0
    set_packing_exact_rescored_total = 0
    set_packing_mode_counts = {}
    set_packing_interrupted_days = []
    rendezvous_candidates_total = 0
    rendezvous_executable_total = 0
    rollout_exact_future_brand_days_total = 0
    rollout_exact_future_portions_total = 0
    anytime_repairs_total = 0
    anytime_exact_delta_total = 0
    improvement_timeline_parts = []
    for day in range(len(day_steps)):
        quality = stderr_quality.get(day, {})
        profile = quality.get("selected_profile") or ("fast_baseline" if quality.get("fast_only") else "unknown")
        profile_counts[profile] = profile_counts.get(profile, 0) + 1
        family = quality.get("map_family") or "unknown"
        time_profile = quality.get("time_profile") or "unknown"
        map_family_counts[family] = map_family_counts.get(family, 0) + 1
        time_profile_counts[time_profile] = time_profile_counts.get(time_profile, 0) + 1
        reason = quality.get("selected_reason")
        if reason:
            selected_reasons[reason] = selected_reasons.get(reason, 0) + 1
        compute = quality.get("compute_ms")
        budget = quality.get("budget_ms")
        if compute is not None:
            profile_compute.setdefault(profile, []).append(compute)
        if profile == "fast_baseline":
            strong_rejected_count += 1
        if compute is not None and budget is not None and compute > budget * 2:
            strong_over_budget_days.append(day)
        planned_refuels_total += quality.get("planned_refuels", 0)
        feasible_refuels_total += quality.get("feasible_refuels", 0)
        failed_rendezvous_total += quality.get("failed_rendezvous", 0)
    for row in day_rows:
        last_portion_missing_total += row.get("last_portion_missing", 0)
        last_portion_repairs_total += row.get("last_portion_repairs", 0)
        road_wait_steps_total += row.get("road_wait_steps", 0)
        end_on_road_total += row.get("end_on_road_routes", 0)
        assigned_visits_total += row.get("assigned_visits", 0)
        ghost_visits_total += row.get("ghost_visits", 0)
        position_mismatch_total += row.get("position_mismatch", 0)
        fuel_mismatch_total += row.get("fuel_mismatch", 0)
        route_signature_duplicates_total += row.get("route_signature_duplicates", 0)
        risk_repairs_total += row.get("risk_repairs", 0)
        ghost_reassign_repairs_total += row.get("ghost_reassign_repairs", 0)
        cached_plan_returns_total += row.get("cached_plan_returns", 0)
        retry_count_total += row.get("retry_count", 0)
        debt_duplicate_updates_total += row.get("debt_duplicate_updates", 0)
        final_negative_slack_total += row.get("negative_slack_final", 0)
        reachable_stock_next_day_total += row.get("reachable_cap_est", 0)
        fuel_used_total += row.get("fuel_used", 0)
        if row.get("best_plan_improvement_timeline"):
            timeline_text = "/".join(
                f"{item.get('stage')}:{item.get('server', '')}:{item.get('negative_slack', '')}"
                for item in row.get("best_plan_improvement_timeline", [])
            )
            improvement_timeline_parts.append(f"d{row.get('day')}[{timeline_text}]")
        policy = row.get("selected_rollout_policy")
        if policy:
            rollout_policy_counts[policy] = rollout_policy_counts.get(policy, 0) + 1
        selected_rollout_future_total += row.get("selected_rollout_future_server", 0)
        selected_rollout_total_total += row.get("selected_rollout_total_server", 0)
        if row.get("brand_max_reachable", 0) and row.get("brands", 0) < row.get("brand_max_reachable", 0):
            brand_constraint_loss_days.append(row.get("day"))
        set_packing_states_total += row.get("set_packing_states", 0)
        set_packing_feasible_states_total += row.get("set_packing_feasible_states", 0)
        set_packing_exact_rescored_total += row.get("set_packing_exact_rescored", 0)
        mode = row.get("set_packing_mode")
        if mode:
            set_packing_mode_counts[mode] = set_packing_mode_counts.get(mode, 0) + 1
        if row.get("set_packing_interrupted", 0):
            set_packing_interrupted_days.append(row.get("day"))
        rendezvous_candidates_total += row.get("rendezvous_candidates", 0)
        rendezvous_executable_total += row.get("rendezvous_executable", 0)
        rollout_exact_future_brand_days_total += row.get("rollout_exact_future_brand_days", 0)
        rollout_exact_future_portions_total += row.get("rollout_exact_future_portions", 0)
        anytime_repairs_total += row.get("anytime_repairs", 0)
        anytime_exact_delta_total += row.get("anytime_exact_delta", 0)
        actual_refuels_total += row.get("actual_refuels", 0)
        if row.get("tanker_idle", 0) > 0:
            tanker_idle_days += 1
            tanker_idle_streak += 1
            max_tanker_idle_streak = max(max_tanker_idle_streak, tanker_idle_streak)
        else:
            tanker_idle_streak = 0
        patrols = row.get("patrols", 0)
        if patrols and (row.get("low_fuel", 0) >= max(1, patrols - 1) or row.get("active", 0) <= max(1, patrols // 2)):
            fuel_collapse_days.append(row.get("day"))
    profile_counts_text = ",".join(f"{key}:{value}" for key, value in sorted(profile_counts.items()))
    map_family_counts_text = ",".join(f"{key}:{value}" for key, value in sorted(map_family_counts.items()))
    time_profile_counts_text = ",".join(f"{key}:{value}" for key, value in sorted(time_profile_counts.items()))
    selected_reasons_text = ",".join(f"{key}:{value}" for key, value in sorted(selected_reasons.items()))
    profile_compute_text = ",".join(
        f"{profile}:avg{int(sum(values) / len(values))}/max{max(values)}"
        for profile, values in sorted(profile_compute.items())
        if values
    )
    rollout_policy_counts_text = ",".join(f"{key}:{value}" for key, value in sorted(rollout_policy_counts.items()))
    set_packing_mode_counts_text = ",".join(f"{key}:{value}" for key, value in sorted(set_packing_mode_counts.items()))

    http_path = base / "http.log"
    bot_ms = []
    state_ms = []
    post_ms = []
    day_jump_count = 0
    http_missing_days = []
    margins = []
    state_margins = []
    submit_margins = []
    negative_margin_days = set()
    summary_deadline_invalid_days = set()
    setup_to_first_state_ms = 0
    requests_total = 0
    requests_per_second_p95 = 0
    overlap_count = 0
    handover_count = 0
    timeout_count = 0
    leader_conflict = False
    rate_invalid = False
    handover_storm = False
    if http_path.exists():
        for line in http_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "leader_lock acquired=0" in line:
                leader_conflict = True
            first_state_match = re.search(r"setup_to_first_state_ms=(\d+)", line)
            if first_state_match:
                setup_to_first_state_ms = int(first_state_match.group(1))
            bot_match = re.search(r"BOT day \d+ -> (\d+)ms", line)
            if bot_match:
                bot_ms.append(int(bot_match.group(1)))
            state_http_match = re.search(r"HTTP .*GET /state -> \d+ (\d+)ms", line)
            if state_http_match:
                state_ms.append(int(state_http_match.group(1)))
            post_match = re.search(r"Submitted day \d+ post_ms=(\d+)", line)
            if post_match:
                post_ms.append(int(post_match.group(1)))
            if "day jump" in line:
                day_jump_count += 1
            miss_match = re.search(r"missing_action_days=\[([^\]]*)\]", line)
            if miss_match:
                http_missing_days = [
                    int(x.strip()) for x in miss_match.group(1).split(",") if x.strip()
                ]
            margin_match = re.search(r"(?:STATE|Submitted) day (\d+) .*submit_deadline_margin_ms=(-?\d+)", line)
            if margin_match:
                day = int(margin_match.group(1))
                margin = int(margin_match.group(2))
                margins.append(margin)
                if "STATE day" in line:
                    state_margins.append(margin)
                if "Submitted day" in line:
                    submit_margins.append(margin)
                if margin < 0:
                    negative_margin_days.add(day)
            deadline_summary_match = re.search(r"deadline_invalid_days=\[([^\]]*)\]", line)
            if deadline_summary_match:
                for raw in deadline_summary_match.group(1).split(","):
                    raw = raw.strip()
                    if raw:
                        summary_deadline_invalid_days.add(int(raw))
            transport_summary_match = re.search(
                r"SUMMARY requests_total=(\d+) requests_per_second_p95=(\d+) overlap_count=(\d+) handover_count=(\d+) timeout_count=(\d+)",
                line,
            )
            if transport_summary_match:
                requests_total = int(transport_summary_match.group(1))
                requests_per_second_p95 = int(transport_summary_match.group(2))
                overlap_count = int(transport_summary_match.group(3))
                handover_count = int(transport_summary_match.group(4))
                timeout_count = int(transport_summary_match.group(5))

    rate_invalid = requests_per_second_p95 > 4 or overlap_count > 0
    handover_storm = handover_count >= 10 or timeout_count >= 10

    avg_bot_ms = int(sum(bot_ms) / len(bot_ms)) if bot_ms else 0
    max_bot_ms = max(bot_ms) if bot_ms else 0
    avg_state_ms = int(sum(state_ms) / len(state_ms)) if state_ms else 0
    max_state_ms = max(state_ms) if state_ms else 0
    avg_post_ms = int(sum(post_ms) / len(post_ms)) if post_ms else 0
    max_post_ms = max(post_ms) if post_ms else 0
    avg_margin = int(sum(margins) / len(margins)) if margins else 0
    min_margin = min(margins) if margins else 0
    min_state_margin = min(state_margins) if state_margins else 0
    min_submit_margin = min(submit_margins) if submit_margins else 0
    action_response_invalid = bool(
        invalid_action_response_days or
        missing_action_response_days or
        assignment_response_valid is False
    )
    transport_invalid = bool(
        missing_action_days or http_missing_days or day_jump_count or
        rate_invalid or leader_conflict or handover_storm or action_response_invalid
    )
    deadline_invalid = bool(negative_margin_days or summary_deadline_invalid_days)
    match_classification = "farm_valid"
    if transport_invalid:
        match_classification = "transport_invalid"
    elif deadline_invalid:
        match_classification = "deadline_invalid"
    elif chosen and total:
        gap_ratio = int(100 * (total - chosen["udon_total"]) / max(1, total))
        if gap_ratio > 25:
            match_classification = "server_gap_unknown"
    farm_benchmark_valid = match_classification == "farm_valid"
    print(
        f"estimated_total={total} conservative_total={conservative_total} "
        f"assigned_visits_total={assigned_visits_total} "
        f"estimated_capture_ratio={(int(100 * total / total_cap) if total_cap else 0)}% "
        f"assignment_coverage={(int(100 * total / total_cap) if total_cap else 0)}% "
        f"stock_execution_efficiency={(int(100 * conservative_total / max(1, total)) if total else 100)}% "
        f"visit_usefulness={(int(100 * total / max(1, assigned_visits_total)) if assigned_visits_total else 100)}% "
        f"end_to_end_visit_yield={(int(100 * conservative_total / max(1, assigned_visits_total)) if assigned_visits_total else 100)}% "
        f"execution_efficiency={(int(100 * conservative_total / max(1, assigned_visits_total)) if assigned_visits_total else 100)}% "
        f"effective_capture_ratio={(int(100 * conservative_total / total_cap) if total_cap else 0)}% "
        f"final_negative_slack_total={final_negative_slack_total} "
        f"fuel_used_per_portion={(fuel_used_total // max(1, total)) if total else 0} "
        f"reachable_stock_next_day_total={reachable_stock_next_day_total} "
        f"road_gap_risk={max(0, total - conservative_total)} "
        f"ghost_stock_total={max(0, total - conservative_total)} "
        f"ghost_visits_total={ghost_visits_total} "
        f"position_mismatch_total={position_mismatch_total} "
        f"fuel_mismatch_total={fuel_mismatch_total} "
        f"reachable_cap_est={reachable_cap_est} "
        f"reachable_capture_ratio={(int(100 * total / reachable_cap_est) if reachable_cap_est else 0)}% "
        f"top_missing_spots_total={top_missing_total_text} "
        f"missing_action_days={missing_action_days} http_missing_days={http_missing_days} "
        f"missing_action_response_days={missing_action_response_days} "
        f"invalid_action_response_days={invalid_action_response_days} "
        f"assignment_response_valid={assignment_response_valid} "
        f"day_jump_count={day_jump_count} avg_bot_ms={avg_bot_ms} max_bot_ms={max_bot_ms} "
        f"setup_to_first_state_ms={setup_to_first_state_ms} "
        f"avg_state_ms={avg_state_ms} max_state_ms={max_state_ms} "
        f"avg_post_ms={avg_post_ms} max_post_ms={max_post_ms} "
        f"negative_margin_days={sorted(negative_margin_days)} avg_margin_ms={avg_margin} "
        f"min_margin_ms={min_margin} min_state_margin_ms={min_state_margin} "
        f"min_submit_margin_ms={min_submit_margin} transport_invalid={int(transport_invalid)} "
        f"rate_invalid={int(rate_invalid)} handover_storm={int(handover_storm)} "
        f"leader_conflict={int(leader_conflict)} requests_total={requests_total} "
        f"requests_per_second_p95={requests_per_second_p95} overlap_count={overlap_count} "
        f"handover_count={handover_count} timeout_count={timeout_count} "
        f"deadline_invalid={int(deadline_invalid)} match_classification={match_classification} "
        f"farm_benchmark_valid={int(farm_benchmark_valid)} "
        f"map_family_counts={map_family_counts_text} time_profile_counts={time_profile_counts_text} "
        f"profile_counts={profile_counts_text} selected_reasons={selected_reasons_text} "
        f"profile_compute={profile_compute_text} "
        f"rollout_policy_counts={rollout_policy_counts_text} "
        f"selected_rollout_future_total={selected_rollout_future_total} "
        f"selected_rollout_total_total={selected_rollout_total_total} "
        f"brand_constraint_loss={brand_constraint_loss_days} "
        f"set_packing_states_total={set_packing_states_total} "
        f"set_packing_feasible_states_total={set_packing_feasible_states_total} "
        f"set_packing_exact_rescored_total={set_packing_exact_rescored_total} "
        f"set_packing_modes={set_packing_mode_counts_text} "
        f"set_packing_interrupted_days={set_packing_interrupted_days} "
        f"rollout_exact_future_brand_days_total={rollout_exact_future_brand_days_total} "
        f"rollout_exact_future_portions_total={rollout_exact_future_portions_total} "
        f"anytime_repairs_total={anytime_repairs_total} anytime_exact_delta_total={anytime_exact_delta_total} "
        f"last_portion_missing={last_portion_missing_total} last_portion_repairs={last_portion_repairs_total} "
        f"risk_repairs={risk_repairs_total} "
        f"ghost_reassign_repairs={ghost_reassign_repairs_total} "
        f"road_wait_steps_total={road_wait_steps_total} end_on_road_total={end_on_road_total} "
        f"route_signature_duplicates_total={route_signature_duplicates_total} "
        f"retry_count={retry_count_total} cached_plan_returns={cached_plan_returns_total} debt_duplicate_updates={debt_duplicate_updates_total} "
        f"best_plan_improvement_timeline={';'.join(improvement_timeline_parts)} "
        f"setup_assignment_reason={setup_assignment_reason} tanker_value_est={'' if tanker_value_est is None else tanker_value_est} "
        f"strong_rejected_count={strong_rejected_count} strong_over_budget_days={strong_over_budget_days} "
        f"planned_refuels={planned_refuels_total} feasible_refuels={feasible_refuels_total} "
        f"actual_refuels={actual_refuels_total} failed_rendezvous={failed_rendezvous_total} "
        f"rendezvous_success_rate={(int(100 * rendezvous_executable_total / max(1, rendezvous_candidates_total)) if rendezvous_candidates_total else 100)} "
        f"tanker_idle_days={tanker_idle_days} tanker_idle_streak={max_tanker_idle_streak} "
        f"fuel_collapse_days={fuel_collapse_days}"
    )
    if args.compare_result and chosen:
        gap = total - chosen["udon_total"]
        gap_ratio = int(100 * gap / max(1, total))
        if gap_ratio > 25:
            print(f"WARNING estimated_vs_actual_gap={gap} gap_ratio={gap_ratio}%")

    if args.benchmark_version:
        benchmarks_dir = Path("benchmarks")
        benchmarks_dir.mkdir(exist_ok=True)
        benchmark_path = benchmarks_dir / f"{args.benchmark_version}.json"
        if benchmark_path.exists():
            benchmark = load_json(benchmark_path)
        else:
            benchmark = {"version": args.benchmark_version, "created_at": datetime.now(timezone.utc).isoformat(), "matches": {}}
        top_row = None
        if result_path.exists():
            result = load_json(result_path)
            standings = result.get("standings", [])
            top_row = max(standings, key=lambda row: row.get("udon_total", 0), default=None)
        benchmark["matches"][args.match_id] = {
            "estimated_total": total,
            "conservative_total": conservative_total,
            "assigned_visits_total": assigned_visits_total,
            "assignment_coverage": int(100 * total / total_cap) if total_cap else 0,
            "stock_execution_efficiency": int(100 * conservative_total / max(1, total)) if total else 100,
            "visit_usefulness": int(100 * total / max(1, assigned_visits_total)) if assigned_visits_total else 100,
            "end_to_end_visit_yield": int(100 * conservative_total / max(1, assigned_visits_total)) if assigned_visits_total else 100,
            "execution_efficiency": int(100 * conservative_total / max(1, assigned_visits_total)) if assigned_visits_total else 100,
            "effective_capture_ratio": int(100 * conservative_total / total_cap) if total_cap else 0,
            "final_negative_slack_total": final_negative_slack_total,
            "fuel_used_per_portion": (fuel_used_total // max(1, total)) if total else 0,
            "reachable_stock_next_day_total": reachable_stock_next_day_total,
            "best_plan_improvement_timeline": improvement_timeline_parts,
            "road_gap_risk": max(0, total - conservative_total),
            "ghost_stock_total": max(0, total - conservative_total),
            "ghost_visits_total": ghost_visits_total,
            "position_mismatch_total": position_mismatch_total,
            "fuel_mismatch_total": fuel_mismatch_total,
            "map_group": map_group,
            "width": width,
            "height": height,
            "stock_cap_total": total_cap,
            "reachable_cap_est": reachable_cap_est,
            "actual_total": chosen.get("udon_total") if chosen else None,
            "actual_types": chosen.get("udon_types") if chosen else None,
            "actual_daily_types": chosen.get("daily_types_sum") if chosen else None,
            "response_ms": chosen.get("response_ms_total") if chosen else None,
            "top_total": top_row.get("udon_total") if top_row else None,
            "top_gap": (top_row.get("udon_total") - chosen.get("udon_total")) if top_row and chosen else None,
            "transport_invalid": transport_invalid,
            "deadline_invalid": deadline_invalid,
            "action_response_invalid": action_response_invalid,
            "missing_action_response_days": missing_action_response_days,
            "invalid_action_response_days": invalid_action_response_days,
            "assignment_response_valid": assignment_response_valid,
            "match_classification": match_classification,
            "farm_benchmark_valid": farm_benchmark_valid,
            "avg_bot_ms": avg_bot_ms,
            "max_bot_ms": max_bot_ms,
            "profile_counts": profile_counts,
            "map_family_counts": map_family_counts,
            "time_profile_counts": time_profile_counts,
            "selected_reasons": selected_reasons,
            "setup_assignment_reason": setup_assignment_reason,
            "setup_ab_3day": setup_ab_3day,
            "tanker_value_est": tanker_value_est,
            "last_portion_missing": last_portion_missing_total,
            "last_portion_repairs": last_portion_repairs_total,
            "risk_repairs": risk_repairs_total,
            "ghost_reassign_repairs": ghost_reassign_repairs_total,
            "road_wait_steps_total": road_wait_steps_total,
            "end_on_road_total": end_on_road_total,
            "route_signature_duplicates_total": route_signature_duplicates_total,
            "set_packing_exact_rescored_total": set_packing_exact_rescored_total,
            "retry_count": retry_count_total,
            "cached_plan_returns": cached_plan_returns_total,
            "debt_duplicate_updates": debt_duplicate_updates_total,
            "profile_compute": {
                profile: {
                    "avg_ms": int(sum(values) / len(values)),
                    "max_ms": max(values),
                }
                for profile, values in profile_compute.items()
                if values
            },
            "strong_rejected_count": strong_rejected_count,
            "strong_over_budget_days": strong_over_budget_days,
            "planned_refuels": planned_refuels_total,
            "feasible_refuels": feasible_refuels_total,
            "failed_rendezvous": failed_rendezvous_total,
            "actual_refuels": actual_refuels_total,
            "tanker_idle_days": tanker_idle_days,
            "tanker_idle_streak": max_tanker_idle_streak,
            "fuel_collapse_days": fuel_collapse_days,
            "setup_to_first_state_ms": setup_to_first_state_ms,
            "avg_state_ms": avg_state_ms,
            "max_state_ms": max_state_ms,
            "avg_post_ms": avg_post_ms,
            "max_post_ms": max_post_ms,
            "min_state_margin_ms": min_state_margin,
            "min_submit_margin_ms": min_submit_margin,
            "top_missing_spots_total": [
                {
                    "spot_id": i,
                    "pos": spots[i]["pos"],
                    "brand": spots[i]["brand"],
                    "stock_per_day": stock,
                    "missing_total": missing,
                    "repeat_missing_days": repeat_missing_days_by_spot.get(i, 0),
                }
                for missing, stock, i in top_missing_total[:8]
                if missing > 0
            ],
            "days": day_rows,
        }
        benchmark_path.write_text(json.dumps(benchmark, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"benchmark_written={benchmark_path}")


if __name__ == "__main__":
    main()
