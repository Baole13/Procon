import unittest
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from run_cpp_http import BackoffPolicy, LeaderLock


class TransportHardeningTest(unittest.TestCase):
    def test_handover_backoff_increases_and_caps(self):
        policy = BackoffPolicy()
        delays = [
            policy.delay_for_http(503, '{"error":"E_HANDOVER"}')[0]
            for _ in range(8)
        ]
        self.assertGreaterEqual(delays[1], 0.4)
        self.assertGreaterEqual(delays[2], 0.8)
        self.assertLessEqual(max(delays), 6.0)

    def test_retry_after_is_respected_for_429(self):
        policy = BackoffPolicy()
        delay, reason = policy.delay_for_http(429, "", retry_after="3")
        self.assertEqual(reason, "rate_limited")
        self.assertGreaterEqual(delay, 3.0)

    def test_leader_lock_allows_only_one_owner(self):
        match_id = f"m-unit-{int(time.time() * 1000)}"
        log_path = Path("matches") / match_id / "leader.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        first = LeaderLock(match_id, "token", log_path)
        second = LeaderLock(match_id, "token", log_path)
        try:
            self.assertTrue(first.acquire())
            self.assertFalse(second.acquire())
            first.release()
            self.assertTrue(second.acquire())
        finally:
            first.release()
            second.release()


if __name__ == "__main__":
    unittest.main()
