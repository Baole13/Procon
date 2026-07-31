#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import random
import subprocess
import sys
import time
import urllib.error
import urllib.request
from enum import Enum
from datetime import datetime, timezone
from pathlib import Path


def load_token(explicit):
    if explicit:
        return explicit
    if os.environ.get("HEX_TOKEN"):
        return os.environ["HEX_TOKEN"]
    env_path = Path(".env")
    if env_path.exists():
        for line in env_path.read_text(encoding="utf-8").splitlines():
            if line.strip().startswith("HEX_TOKEN="):
                return line.split("=", 1)[1].strip()
    return None


def write_log(path, message):
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {message}"
    with path.open("a", encoding="utf-8") as f:
        f.write(line + "\n")
    print(message, flush=True)


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


def append_replay(path, value):
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")


def accepted_response(payload):
    """Return (accepted, reason, implicit) for assignment/action POST responses."""
    if payload is None:
        return False, "empty_response", False
    if not isinstance(payload, dict):
        return False, "non_object_response", False
    for key in ("valid", "accepted", "ok", "success"):
        if key in payload:
            return bool(payload.get(key)), f"{key}={payload.get(key)}", False
    status = str(payload.get("status", "")).lower()
    if status in ("ok", "accepted", "success", "valid"):
        return True, f"status={status}", False
    if status in ("error", "invalid", "rejected", "failed", "failure"):
        return False, f"status={status}", False
    if "error" in payload:
        return False, f"error={payload.get('error')}", False
    if "reason" in payload and str(payload.get("reason", "")).strip():
        return False, f"reason={payload.get('reason')}", False
    response_type = str(payload.get("type", "")).lower()
    if "result" in response_type or "accepted" in response_type or "action" in response_type or "assignment" in response_type:
        return True, f"implicit_type={response_type}", True
    return False, "missing_accept_flag", False


def safe_actions_for_state(state, setup):
    agents = state.get("agents", [])
    day = state.get("day", 0)
    day_steps = setup.get("daySteps", [])
    budget = day_steps[day] if isinstance(day, int) and 0 <= day < len(day_steps) else state.get("daySteps", 0)
    try:
        budget = int(budget)
    except Exception:
        budget = 0
    wait = -budget if budget > 0 else 0
    return [[wait] if wait else [] for _ in agents]


def parse_deadline_seconds(value):
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value) / 1000.0 if value > 1_000_000_000_000 else float(value)
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return None
        try:
            numeric = float(text)
            return numeric / 1000.0 if numeric > 1_000_000_000_000 else numeric
        except ValueError:
            pass
        try:
            if text.endswith("Z"):
                text = text[:-1] + "+00:00"
            return datetime.fromisoformat(text).timestamp()
        except ValueError:
            return None
    return None


def deadline_meta(ends_at, received_at):
    deadline_seconds = parse_deadline_seconds(ends_at)
    if deadline_seconds is None:
        return None, 0
    margin_ms = int((deadline_seconds - received_at) * 1000)
    if margin_ms < 700:
        return margin_ms, 2
    if margin_ms < 1500:
        return margin_ms, 1
    return margin_ms, 0


def bot_environment():
    env = os.environ.copy()
    candidates = [
        Path(r"C:\msys64\ucrt64\bin"),
        Path(r"C:\msys64\mingw64\bin"),
        Path(r"C:\msys64\clang64\bin"),
        Path(r"C:\mingw64\bin"),
        Path(r"C:\MinGW\bin"),
    ]
    extra = [str(path) for path in candidates if path.exists()]
    if extra:
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    return env


class HttpPhase(Enum):
    WAIT_SETUP = "WAIT_SETUP"
    WAIT_START = "WAIT_START"
    ACTIVE_DAY = "ACTIVE_DAY"
    AFTER_VALID_ACTION = "AFTER_VALID_ACTION"
    FINISHED = "FINISHED"


class LeaderLock:
    def __init__(self, match_id, token, log_path):
        token_hash = hashlib.sha256(token.encode("utf-8")).hexdigest()[:12]
        self.path = Path("matches") / f".leader-{match_id}-{token_hash}.lock"
        self.log_path = log_path
        self.fd = None
        self.owns_lock = False

    def acquire(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.fd = os.open(str(self.path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(self.fd, f"pid={os.getpid()} started={time.time()}\n".encode("ascii"))
            self.owns_lock = True
            write_log(self.log_path, f"leader_lock acquired=1 path={self.path}")
            return True
        except FileExistsError:
            owner = ""
            try:
                owner = self.path.read_text(encoding="utf-8", errors="replace").strip()
            except Exception:
                pass
            write_log(self.log_path, f"leader_lock acquired=0 path={self.path} owner={owner}")
            return False

    def release(self):
        owned = self.owns_lock
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None
        self.owns_lock = False
        if owned:
            try:
                self.path.unlink()
            except FileNotFoundError:
                pass


class RequestLimiter:
    def __init__(self, min_interval_ms, log_path):
        self.min_interval = max(0, min_interval_ms) / 1000.0
        self.log_path = log_path
        self.last_request_started = 0.0

    def wait(self, phase, method, path):
        now = time.perf_counter()
        wait_seconds = max(0.0, self.last_request_started + self.min_interval - now)
        wait_ms = int(wait_seconds * 1000)
        if wait_ms > 0:
            write_log(self.log_path, f"rate_limit phase={phase.value} method={method} path={path} rate_wait_ms={wait_ms}")
            time.sleep(wait_seconds)
        self.last_request_started = time.perf_counter()


class BackoffPolicy:
    def __init__(self):
        self.handover_attempts = 0
        self.timeout_attempts = 0
        self.not_ready_attempts = 0

    def reset(self):
        self.handover_attempts = 0
        self.timeout_attempts = 0
        self.not_ready_attempts = 0

    @staticmethod
    def jitter(seconds):
        return seconds * random.uniform(0.82, 1.18)

    def delay_for_http(self, code, body_text, retry_after=None):
        if code in (200, 201):
            self.reset()
            return 0.0, "success"
        if code == 401 or code == 403:
            return None, "auth_stop"
        if code == 400:
            return None, "bad_payload_stop"
        if code == 429:
            if retry_after:
                try:
                    return max(1.0, float(retry_after)), "rate_limited"
                except ValueError:
                    pass
            return 1.0, "rate_limited"
        if code == 425:
            self.not_ready_attempts += 1
            return self.jitter(min(0.5, 0.25 + 0.05 * self.not_ready_attempts)), "not_ready"
        if code == 503 and "E_HANDOVER" in body_text:
            bases = [0.25, 0.5, 1.0, 2.0, 4.0, 5.0]
            delay = bases[min(self.handover_attempts, len(bases) - 1)]
            self.handover_attempts += 1
            return self.jitter(delay), "handover"
        return None, "http_stop"

    def delay_for_timeout(self):
        bases = [0.25, 0.5, 1.0, 2.0]
        delay = bases[min(self.timeout_attempts, len(bases) - 1)]
        self.timeout_attempts += 1
        return self.jitter(delay), "timeout"


class MatchClient:
    def __init__(self, base_url, match_id, token, timeout, log_path, limiter):
        self.api = f"{base_url.rstrip('/')}/api/v1/matches/{match_id}"
        self.timeout = timeout
        self.log_path = log_path
        self.limiter = limiter
        self.headers = {
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        }
        self.last_elapsed_ms = 0
        self.last_status = None
        self.last_body = ""
        self.last_retry_after = None
        self.request_id = 0
        self.inflight = False
        self.overlap_count = 0
        self.requests_total = 0
        self.handover_count = 0
        self.timeout_count = 0
        self.request_start_times = []

    def call(self, method, path, body=None, timeout=None, phase=HttpPhase.ACTIVE_DAY):
        if self.inflight:
            self.overlap_count += 1
            write_log(self.log_path, f"ERROR overlap_request phase={phase.value} method={method} path={path} overlap_count={self.overlap_count}")
        self.limiter.wait(phase, method, path)
        self.inflight = True
        self.request_id += 1
        rid = self.request_id
        self.requests_total += 1
        self.request_start_times.append(time.time())
        self.last_status = None
        self.last_body = ""
        self.last_retry_after = None
        data = None if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
        req = urllib.request.Request(
            self.api + path,
            data=data,
            method=method,
            headers=self.headers,
        )
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout if timeout is None else timeout) as response:
                payload = response.read()
                elapsed = int((time.perf_counter() - started) * 1000)
                self.last_elapsed_ms = elapsed
                self.last_status = response.status
                write_log(self.log_path, f"HTTP phase={phase.value} request_id={rid} inflight=1 {method} {path} -> {response.status} {elapsed}ms")
                if not payload:
                    return None
                return json.loads(payload.decode("utf-8"))
        except urllib.error.HTTPError as exc:
            elapsed = int((time.perf_counter() - started) * 1000)
            self.last_elapsed_ms = elapsed
            self.last_status = exc.code
            self.last_retry_after = exc.headers.get("Retry-After") if exc.headers else None
            body_text = exc.read().decode("utf-8", errors="replace")
            self.last_body = body_text
            if exc.code == 503 and "E_HANDOVER" in body_text:
                self.handover_count += 1
            write_log(self.log_path, f"HTTP phase={phase.value} request_id={rid} inflight=1 {method} {path} -> {exc.code} {elapsed}ms {body_text}")
            raise
        except Exception as exc:
            elapsed = int((time.perf_counter() - started) * 1000)
            self.last_elapsed_ms = elapsed
            self.timeout_count += 1
            write_log(self.log_path, f"HTTP phase={phase.value} request_id={rid} inflight=1 {method} {path} -> ERR {elapsed}ms {exc}")
            raise
        finally:
            self.inflight = False

    def request_rate_p95(self):
        if not self.request_start_times:
            return 0
        buckets = {}
        for stamp in self.request_start_times:
            second = int(stamp)
            buckets[second] = buckets.get(second, 0) + 1
        counts = sorted(buckets.values())
        if not counts:
            return 0
        return counts[min(len(counts) - 1, int(0.95 * len(counts)))]


def read_bot_line(bot, context, log_path):
    started = time.perf_counter()
    line = bot.stdout.readline()
    elapsed = int((time.perf_counter() - started) * 1000)
    if not line:
        raise RuntimeError(f"C++ bot returned no output for {context}")
    write_log(log_path, f"BOT {context} -> {elapsed}ms {line.strip()[:300]}")
    return line


def call_with_retry(client, method, path, body, retries=6, timeout=None, phase=HttpPhase.ACTIVE_DAY, backoff=None):
    if backoff is None:
        backoff = BackoffPolicy()
    for attempt in range(retries):
        try:
            result = client.call(method, path, body, timeout=timeout, phase=phase)
            backoff.reset()
            return result
        except urllib.error.HTTPError as exc:
            delay, reason = backoff.delay_for_http(exc.code, client.last_body, client.last_retry_after)
            if delay is None or attempt == retries - 1:
                write_log(client.log_path, f"retry_stop phase={phase.value} method={method} path={path} code={exc.code} reason={reason} attempt={attempt}")
                raise
            delay_ms = int(delay * 1000)
            write_log(client.log_path, f"backoff phase={phase.value} method={method} path={path} code={exc.code} reason={reason} attempt={attempt} backoff_ms={delay_ms}")
            time.sleep(delay)
        except Exception:
            if attempt == retries - 1:
                raise
            delay, reason = backoff.delay_for_timeout()
            delay_ms = int(delay * 1000)
            write_log(client.log_path, f"backoff phase={phase.value} method={method} path={path} code=ERR reason={reason} attempt={attempt} backoff_ms={delay_ms}")
            time.sleep(delay)


def main():
    parser = argparse.ArgumentParser(description="HTTP transport wrapper for the C++ sandbox bot.")
    parser.add_argument("--url", default="https://procon.ptit.edu.vn")
    parser.add_argument("--match", required=True)
    parser.add_argument("--token", default=None)
    parser.add_argument("--bot", default="bot.exe")
    parser.add_argument("--poll-ms", type=int, default=250)
    parser.add_argument("--short-poll-ms", type=int, default=250)
    parser.add_argument("--fast-poll", action="store_true", help="Use shorter polling for short-day hard matches.")
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--action-timeout", type=float, default=1.0)
    parser.add_argument("--min-request-interval-ms", type=int, default=250)
    parser.add_argument("--observer-if-locked", action="store_true", help="Do not submit when another leader owns this match/token.")
    args = parser.parse_args()

    token = load_token(args.token)
    if not token:
        raise SystemExit("Missing token. Pass --token or set HEX_TOKEN in .env/environment.")

    bot_path = Path(args.bot)
    if not bot_path.exists():
        raise SystemExit("Bot executable not found. Run .\\build_cpp.ps1 first.")

    match_dir = Path("matches") / args.match
    match_dir.mkdir(parents=True, exist_ok=True)
    http_log = match_dir / "http.log"
    replay = match_dir / "replay.jsonl"
    http_log.write_text("", encoding="utf-8")
    replay.write_text("", encoding="utf-8")

    leader_lock = LeaderLock(args.match, token, http_log)
    is_leader = leader_lock.acquire()
    if not is_leader and not args.observer_if_locked:
        raise SystemExit("Another run_cpp_http.py process is already leader for this match/token.")
    if not is_leader and args.observer_if_locked:
        write_log(http_log, "leader_lock observer_exit=1 reason=leader_already_running")
        return

    limiter = RequestLimiter(args.min_request_interval_ms, http_log)
    client = MatchClient(args.url, args.match, token, args.timeout, http_log, limiter)
    transport_phase = HttpPhase.WAIT_SETUP
    setup_backoff = BackoffPolicy()
    state_backoff = BackoffPolicy()
    action_backoff = BackoffPolicy()
    stderr_path = match_dir / "bot.stderr.log"
    stderr_file = stderr_path.open("w", encoding="utf-8")
    bot = subprocess.Popen(
        [str(bot_path.resolve())],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=stderr_file,
        env=bot_environment(),
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    try:
        setup = None
        setup_path = match_dir / "setup.json"
        if setup_path.exists():
            try:
                setup = json.loads(setup_path.read_text(encoding="utf-8"))
                write_log(http_log, f"setup_cache_hit=1 path={setup_path}")
            except Exception as exc:
                write_log(http_log, f"setup_cache_hit=0 cache_error={exc}")
                setup = None
        while setup is None:
            try:
                setup = call_with_retry(
                    client,
                    "GET",
                    "/setup",
                    None,
                    retries=1,
                    timeout=args.timeout,
                    phase=transport_phase,
                    backoff=setup_backoff,
                )
                if setup is not None:
                    setup_backoff.reset()
                    write_log(http_log, "setup_cache_hit=0 setup_loaded=1")
                    break
            except urllib.error.HTTPError as exc:
                delay, reason = setup_backoff.delay_for_http(exc.code, client.last_body, client.last_retry_after)
                if delay is None:
                    raise
                write_log(http_log, f"backoff phase={transport_phase.value} method=GET path=/setup code={exc.code} reason={reason} backoff_ms={int(delay * 1000)}")
                time.sleep(delay)
            except Exception:
                delay, reason = setup_backoff.delay_for_timeout()
                write_log(http_log, f"backoff phase={transport_phase.value} method=GET path=/setup code=ERR reason={reason} backoff_ms={int(delay * 1000)}")
                time.sleep(delay)

        day_seconds = setup.get("daySeconds", [])
        min_day_seconds = min(day_seconds) if day_seconds else None
        short_day = min_day_seconds is not None and min_day_seconds <= 1
        tight_day = min_day_seconds is not None and min_day_seconds <= 2
        poll_ms = args.poll_ms
        if tight_day:
            poll_ms = max(args.min_request_interval_ms, min(poll_ms, args.short_poll_ms))
        elif args.fast_poll or (min_day_seconds is not None and min_day_seconds <= 15):
            poll_ms = max(args.min_request_interval_ms, min(poll_ms, 250))
        else:
            poll_ms = max(args.min_request_interval_ms, poll_ms)
        action_timeout = (
            min(args.timeout, args.action_timeout, max(0.45, min_day_seconds * 0.35))
            if tight_day
            else args.timeout
        )
        state_timeout = min(args.timeout, 0.30) if tight_day else args.timeout
        write_log(
            http_log,
            f"Transport poll_ms={poll_ms} min_day_seconds={min_day_seconds} "
            f"short_day={int(short_day)} tight_day={int(tight_day)} "
            f"state_timeout={state_timeout} action_timeout={action_timeout}",
        )

        save_json(setup_path, setup)
        append_replay(replay, setup)
        bot.stdin.write(json.dumps(setup, separators=(",", ":")) + "\n")
        bot.stdin.flush()
        assignment_raw = read_bot_line(bot, "assignment", http_log)
        (match_dir / "assignment.raw.json").write_text(assignment_raw, encoding="utf-8")
        transport_phase = HttpPhase.WAIT_START
        assignment_marker = match_dir / "assignment.submitted"
        if assignment_marker.exists():
            write_log(http_log, f"Assignment already submitted for match {args.match}; skip duplicate POST")
        elif is_leader:
            assignment_response = call_with_retry(
                client,
                "POST",
                "/assignment",
                json.loads(assignment_raw),
                phase=transport_phase,
                backoff=action_backoff,
            )
            save_json(match_dir / "assignment-response.json", assignment_response)
            accepted, reason, implicit = accepted_response(assignment_response)
            write_log(http_log, f"ASSIGNMENT_RESULT accepted={int(accepted)} implicit_accept={int(implicit)} reason={reason} payload={json.dumps(assignment_response, ensure_ascii=False, separators=(',', ':'))}")
            if not accepted:
                raise RuntimeError(f"Assignment rejected: {reason}")
            assignment_marker.write_text(str(time.time()), encoding="ascii")
        assignment_submitted_at = time.perf_counter()
        write_log(http_log, f"Assignment submitted for match {args.match}")

        current_day = -1
        submitted_days = set()
        missed_days = []
        deadline_invalid_days = set()
        post_samples_ms = []
        first_state_seen = False
        state_errors = 0
        last_result_probe_at = 0.0
        while bot.poll() is None:
            handled_new_day = False
            try:
                transport_phase = HttpPhase.ACTIVE_DAY
                state = client.call("GET", "/state", timeout=state_timeout, phase=transport_phase)
                state_latency_ms = client.last_elapsed_ms
                state_errors = 0
                if state is not None and state.get("day") != current_day:
                    handled_new_day = True
                    day = state.get("day")
                    if not first_state_seen:
                        setup_to_first_state_ms = int((time.perf_counter() - assignment_submitted_at) * 1000)
                        write_log(http_log, f"STATE first_day={day} setup_to_first_state_ms={setup_to_first_state_ms}")
                        first_state_seen = True
                    if current_day >= 0 and day != current_day + 1:
                        jumped = list(range(current_day + 1, day))
                        missed_days.extend(jumped)
                        write_log(http_log, f"ERROR day jump {current_day} -> {day} missed_days={jumped}")
                    save_json(match_dir / f"day-{day}-state.json", state)
                    append_replay(replay, state)
                    received_at = time.time()
                    ends_at = state.get("endsAt")
                    margin_ms, deadline_mode = deadline_meta(ends_at, received_at)
                    current_day_seconds = None
                    if isinstance(day, int) and day_seconds and 0 <= day < len(day_seconds):
                        current_day_seconds = day_seconds[day]
                    elif min_day_seconds is not None:
                        current_day_seconds = min_day_seconds
                    ultra_fast = short_day or (margin_ms is not None and margin_ms < 250)
                    if post_samples_ms:
                        ordered_post = sorted(post_samples_ms)
                        p95_index = min(len(ordered_post) - 1, int(0.95 * len(ordered_post)))
                        rolling_p95_post_ms = ordered_post[p95_index]
                    else:
                        rolling_p95_post_ms = 200 if tight_day else 100
                    network_reserve_ms = max(150, min(500, rolling_p95_post_ms + 100))
                    hard_budget_ms = None
                    if margin_ms is not None:
                        hard_budget_ms = max(10, margin_ms - network_reserve_ms)
                    if ends_at:
                        if margin_ms is not None and margin_ms < 0:
                            deadline_invalid_days.add(day)
                            write_log(http_log, f"ERROR deadline_invalid day={day} margin_ms={margin_ms} endsAt_raw={ends_at}")
                        write_log(
                            http_log,
                            f"STATE day {day} submit_deadline_margin_ms={margin_ms} "
                            f"deadline_mode={deadline_mode} ultra_fast={int(ultra_fast)} "
                            f"hard_budget_ms={hard_budget_ms} network_reserve_ms={network_reserve_ms} "
                            f"day_seconds={current_day_seconds} state_latency_ms={state_latency_ms} "
                            f"endsAt_raw={ends_at} "
                            f"local_epoch={received_at:.3f} local_utc={datetime.fromtimestamp(received_at, timezone.utc).isoformat()}"
                        )
                    bot_state = dict(state)
                    if current_day_seconds is not None:
                        bot_state["_daySeconds"] = int(current_day_seconds)
                    bot_state["_ultraFastMode"] = 1 if ultra_fast else 0
                    if margin_ms is not None:
                        bot_state["_deadlineMarginMs"] = margin_ms
                        bot_state["_deadlineMode"] = deadline_mode
                    bot.stdin.write(json.dumps(bot_state, separators=(",", ":")) + "\n")
                    bot.stdin.flush()
                    actions_raw = read_bot_line(bot, f"day {day}", http_log)
                    (match_dir / f"day-{day}-actions.raw.json").write_text(actions_raw, encoding="utf-8")
                    post_started = time.perf_counter()
                    action_response = None
                    action_accepted = not is_leader
                    action_reason = "observer"
                    if is_leader:
                        action_response = call_with_retry(
                            client,
                            "POST",
                            "/actions",
                            json.loads(actions_raw),
                            retries=2 if tight_day else 6,
                            timeout=action_timeout,
                            phase=HttpPhase.ACTIVE_DAY,
                            backoff=action_backoff,
                        )
                        save_json(match_dir / f"day-{day}-action-response.json", action_response)
                        action_accepted, action_reason, implicit_accept = accepted_response(action_response)
                        write_log(http_log, f"ACTION_RESULT day={day} accepted={int(action_accepted)} implicit_accept={int(implicit_accept)} reason={action_reason} payload={json.dumps(action_response, ensure_ascii=False, separators=(',', ':'))}")
                        if not action_accepted:
                            write_log(http_log, f"ERROR action_rejected day={day} reason={action_reason}")
                            safe_actions = safe_actions_for_state(state, setup)
                            save_json(match_dir / f"day-{day}-safe-actions.raw.json", safe_actions)
                            safe_response = call_with_retry(
                                client,
                                "POST",
                                "/actions",
                                safe_actions,
                                retries=1,
                                timeout=action_timeout,
                                phase=HttpPhase.ACTIVE_DAY,
                                backoff=action_backoff,
                            )
                            save_json(match_dir / f"day-{day}-safe-action-response.json", safe_response)
                            safe_accepted, safe_reason, safe_implicit = accepted_response(safe_response)
                            write_log(http_log, f"SAFE_ACTION_RESULT day={day} accepted={int(safe_accepted)} implicit_accept={int(safe_implicit)} reason={safe_reason} payload={json.dumps(safe_response, ensure_ascii=False, separators=(',', ':'))}")
                            action_accepted = safe_accepted
                            action_reason = safe_reason
                            if not safe_accepted:
                                raise RuntimeError(f"Actions rejected and safe fallback rejected for day {day}: {safe_reason}")
                    post_ms = int((time.perf_counter() - post_started) * 1000)
                    post_samples_ms.append(post_ms)
                    if len(post_samples_ms) > 20:
                        post_samples_ms.pop(0)
                    current_day = day
                    if action_accepted:
                        submitted_days.add(day)
                    if ends_at:
                        margin_after_post_ms, _mode_after_post = deadline_meta(ends_at, time.time())
                        if margin_after_post_ms is not None and margin_after_post_ms < 0:
                            deadline_invalid_days.add(day)
                        write_log(http_log, f"Submitted day {day} accepted={int(action_accepted)} reason={action_reason} post_ms={post_ms} submit_deadline_margin_ms={margin_after_post_ms}")
                    else:
                        write_log(http_log, f"Submitted day {day} accepted={int(action_accepted)} reason={action_reason} post_ms={post_ms}")
                    transport_phase = HttpPhase.AFTER_VALID_ACTION
            except Exception as exc:
                state_errors += 1
                error_delay = poll_ms / 1000.0
                if isinstance(exc, urllib.error.HTTPError):
                    delay, reason = state_backoff.delay_for_http(exc.code, client.last_body, client.last_retry_after)
                    if delay is None:
                        raise
                    error_delay = delay
                    write_log(http_log, f"backoff phase={transport_phase.value} method=GET path=/state code={exc.code} reason={reason} backoff_ms={int(error_delay * 1000)}")
                    if exc.code == 425 and current_day < 0:
                        state_errors = 0
                        time.sleep(error_delay)
                        handled_new_day = True
                        continue
                else:
                    delay, reason = state_backoff.delay_for_timeout()
                    error_delay = delay
                    write_log(http_log, f"backoff phase={transport_phase.value} method=GET path=/state code=ERR reason={reason} backoff_ms={int(error_delay * 1000)}")
                now = time.perf_counter()
                expected_days_for_probe = len(setup.get("daySteps", []))
                after_expected_days = expected_days_for_probe > 0 and current_day >= expected_days_for_probe - 1
                if after_expected_days and state_errors >= 4 and now - last_result_probe_at >= 1.0:
                    last_result_probe_at = now
                    try:
                        result = client.call("GET", "/result", phase=HttpPhase.FINISHED)
                        if result is not None:
                            transport_phase = HttpPhase.FINISHED
                            save_json(match_dir / "result.json", result)
                            expected_days = len(setup.get("daySteps", []))
                            absent = [d for d in range(expected_days) if d not in submitted_days]
                            if absent:
                                write_log(http_log, f"SUMMARY missing_action_days={absent}")
                            if missed_days:
                                write_log(http_log, f"SUMMARY missed_days={missed_days}")
                            if deadline_invalid_days:
                                write_log(http_log, f"SUMMARY deadline_invalid_days={sorted(deadline_invalid_days)}")
                            bot.stdin.write(json.dumps(result, separators=(",", ":")) + "\n")
                            bot.stdin.flush()
                            print("Match finished:")
                            print(json.dumps(result, ensure_ascii=False, indent=2))
                            break
                    except Exception:
                        pass
                time.sleep(error_delay)
                handled_new_day = True

            if not (tight_day and handled_new_day):
                time.sleep(poll_ms / 1000)
    finally:
        write_log(
            http_log,
            f"SUMMARY requests_total={client.requests_total} "
            f"requests_per_second_p95={client.request_rate_p95()} "
            f"overlap_count={client.overlap_count} "
            f"handover_count={client.handover_count} "
            f"timeout_count={client.timeout_count}",
        )
        if bot.poll() is None:
            bot.kill()
        stderr_file.close()
        if stderr_path.exists():
            stderr = stderr_path.read_text(encoding="utf-8")
            if stderr:
                Path(f"run_{args.match}.err.log").write_text(stderr, encoding="utf-8")
        leader_lock.release()


if __name__ == "__main__":
    main()
