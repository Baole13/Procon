#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
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


class MatchClient:
    def __init__(self, base_url, match_id, token, timeout, log_path):
        self.api = f"{base_url.rstrip('/')}/api/v1/matches/{match_id}"
        self.timeout = timeout
        self.log_path = log_path
        self.headers = {
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        }
        self.last_elapsed_ms = 0

    def call(self, method, path, body=None, timeout=None):
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
                write_log(self.log_path, f"HTTP {method} {path} -> {response.status} {elapsed}ms")
                if not payload:
                    return None
                return json.loads(payload.decode("utf-8"))
        except urllib.error.HTTPError as exc:
            elapsed = int((time.perf_counter() - started) * 1000)
            self.last_elapsed_ms = elapsed
            body_text = exc.read().decode("utf-8", errors="replace")
            write_log(self.log_path, f"HTTP {method} {path} -> {exc.code} {elapsed}ms {body_text}")
            raise
        except Exception as exc:
            elapsed = int((time.perf_counter() - started) * 1000)
            self.last_elapsed_ms = elapsed
            write_log(self.log_path, f"HTTP {method} {path} -> ERR {elapsed}ms {exc}")
            raise


def read_bot_line(bot, context, log_path):
    started = time.perf_counter()
    line = bot.stdout.readline()
    elapsed = int((time.perf_counter() - started) * 1000)
    if not line:
        raise RuntimeError(f"C++ bot returned no output for {context}")
    write_log(log_path, f"BOT {context} -> {elapsed}ms {line.strip()[:300]}")
    return line


def call_with_retry(client, method, path, body, retries=6, timeout=None, retry_base=0.25):
    for attempt in range(retries):
        try:
            return client.call(method, path, body, timeout=timeout)
        except urllib.error.HTTPError as exc:
            if exc.code != 429 or attempt == retries - 1:
                raise
            time.sleep(retry_base + 0.15 * attempt)
        except Exception:
            if attempt == retries - 1:
                raise
            time.sleep(retry_base + 0.05 * attempt)


def main():
    parser = argparse.ArgumentParser(description="HTTP transport wrapper for the C++ sandbox bot.")
    parser.add_argument("--url", default="https://procon.ptit.edu.vn")
    parser.add_argument("--match", required=True)
    parser.add_argument("--token", default=None)
    parser.add_argument("--bot", default="bot.exe")
    parser.add_argument("--poll-ms", type=int, default=70)
    parser.add_argument("--short-poll-ms", type=int, default=15)
    parser.add_argument("--fast-poll", action="store_true", help="Use shorter polling for short-day hard matches.")
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--action-timeout", type=float, default=1.0)
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

    client = MatchClient(args.url, args.match, token, args.timeout, http_log)
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
        while setup is None:
            try:
                setup = client.call("GET", "/setup")
            except Exception:
                time.sleep(args.poll_ms / 1000)

        day_seconds = setup.get("daySeconds", [])
        min_day_seconds = min(day_seconds) if day_seconds else None
        short_day = min_day_seconds is not None and min_day_seconds <= 1
        tight_day = min_day_seconds is not None and min_day_seconds <= 2
        poll_ms = args.poll_ms
        if tight_day:
            poll_ms = min(poll_ms, args.short_poll_ms)
        elif args.fast_poll or (min_day_seconds is not None and min_day_seconds <= 15):
            poll_ms = min(poll_ms, 70)
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

        save_json(match_dir / "setup.json", setup)
        append_replay(replay, setup)
        bot.stdin.write(json.dumps(setup, separators=(",", ":")) + "\n")
        bot.stdin.flush()
        assignment_raw = read_bot_line(bot, "assignment", http_log)
        (match_dir / "assignment.raw.json").write_text(assignment_raw, encoding="utf-8")
        call_with_retry(client, "POST", "/assignment", json.loads(assignment_raw))
        assignment_submitted_at = time.perf_counter()
        write_log(http_log, f"Assignment submitted for match {args.match}")

        current_day = -1
        submitted_days = set()
        missed_days = []
        deadline_invalid_days = set()
        post_samples_ms = []
        first_state_seen = False
        state_errors = 0
        while bot.poll() is None:
            handled_new_day = False
            try:
                state = client.call("GET", "/state", timeout=state_timeout)
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
                    call_with_retry(
                        client,
                        "POST",
                        "/actions",
                        json.loads(actions_raw),
                        retries=2 if tight_day else 6,
                        timeout=action_timeout,
                        retry_base=0.05 if short_day else 0.25,
                    )
                    post_ms = int((time.perf_counter() - post_started) * 1000)
                    post_samples_ms.append(post_ms)
                    if len(post_samples_ms) > 20:
                        post_samples_ms.pop(0)
                    current_day = day
                    submitted_days.add(day)
                    if ends_at:
                        margin_after_post_ms, _mode_after_post = deadline_meta(ends_at, time.time())
                        if margin_after_post_ms is not None and margin_after_post_ms < 0:
                            deadline_invalid_days.add(day)
                        write_log(http_log, f"Submitted day {day} post_ms={post_ms} submit_deadline_margin_ms={margin_after_post_ms}")
                    else:
                        write_log(http_log, f"Submitted day {day} post_ms={post_ms}")
            except Exception:
                state_errors += 1
                if state_errors >= 4:
                    try:
                        result = client.call("GET", "/result")
                        if result is not None:
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
                time.sleep(poll_ms / 1000)

            if not (tight_day and handled_new_day):
                time.sleep(poll_ms / 1000)
    finally:
        if bot.poll() is None:
            bot.kill()
        stderr_file.close()
        if stderr_path.exists():
            stderr = stderr_path.read_text(encoding="utf-8")
            if stderr:
                Path(f"run_{args.match}.err.log").write_text(stderr, encoding="utf-8")


if __name__ == "__main__":
    main()
