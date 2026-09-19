import io
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from types import SimpleNamespace
from unittest.mock import Mock, patch

from exoanchor_mcp.client import ExoAnchorError
from exoanchor_mcp.server import McpServer, ToolRuntime, main


class RuntimeLifecycleTests(unittest.TestCase):
    def test_close_without_jobs_does_not_create_resources(self):
        runtime = ToolRuntime(SimpleNamespace())
        runtime.close()
        runtime.close()
        with self.assertRaisesRegex(ExoAnchorError, "closed"):
            _ = runtime.jobs
        with self.assertRaisesRegex(ExoAnchorError, "closed"):
            _ = runtime.ops_jobs
        self.assertIsNone(runtime._jobs)
        self.assertIsNone(runtime._ops_jobs)

    def test_concurrent_first_access_uses_one_manager(self):
        runtime = ToolRuntime(SimpleNamespace(config=SimpleNamespace(state_dir="/unused")))
        barrier = threading.Barrier(8)
        manager = Mock()

        def get_manager():
            barrier.wait(2)
            return runtime.jobs

        with patch("exoanchor_mcp.server.JobManager", return_value=manager) as factory:
            with ThreadPoolExecutor(max_workers=8) as executor:
                values = list(executor.map(lambda _: get_manager(), range(8)))
            factory.assert_called_once()
            self.assertTrue(all(value is manager for value in values))
        runtime.close()
        manager.close.assert_called_once()

    def test_stdio_eof_closes_workers_and_scheduler(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = ToolRuntime(SimpleNamespace(config=SimpleNamespace(state_dir=directory)))
            jobs = runtime.jobs
            ops = runtime.ops_jobs
            accepted, _ = jobs.start({}, lambda: {"ok": True})
            server = McpServer(runtime)
            try:
                with patch("exoanchor_mcp.server.build_server", return_value=server), \
                     patch("sys.stdin", io.StringIO("")):
                    self.assertEqual(main([]), 0)
                self.assertFalse(ops._scheduler.is_alive())
                self.assertEqual(jobs.status(accepted["job_id"])["state"], "succeeded")
                self.assertTrue(jobs._closed)
                self.assertTrue(ops._closed)
            finally:
                runtime.close()

    def test_exceptions_in_serve_or_probe_still_close_runtime(self):
        for args in ([], ["--probe"]):
            with self.subTest(args=args):
                server = Mock()
                server.serve.side_effect = BrokenPipeError("client disconnected")
                server.runtime.call.side_effect = RuntimeError("probe failed")
                with patch("exoanchor_mcp.server.build_server", return_value=server):
                    with self.assertRaises((BrokenPipeError, RuntimeError)):
                        main(args)
                server.runtime.close.assert_called_once()
