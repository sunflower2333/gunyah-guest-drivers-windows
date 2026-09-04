#!/usr/bin/env python3
import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
SCRIPT = SCRIPT_DIR / "run-viogpu-58186-host-teardown.sh"
CAPTURE_HASH = "d45cc3a8377f72544a1f97488b5118ee7a33b4bf469801053e44faff581ef635"
TURNIP_HASH = "f8f9e26ca5e5512b8c7d000d3515277e702babc05b84dfca5a650212fe299513"


def complete_trace(ctx_id, slice_index):
    before = 1 << slice_index
    return [
        f"VA slice {slice_index}: [0x1000, +8GB)",
        f"virgl context table add: ctx_id={ctx_id} result=ok",
        f"rutabaga context destroy begin: ctx_id={ctx_id}",
        f"virglrenderer context drop begin: ctx_id={ctx_id} capset_id=6",
        f"virgl context table remove begin: ctx_id={ctx_id} found=1",
        f"virgl context backend destroy begin: ctx_id={ctx_id}",
        f"ctx_id={ctx_id} backend destroy begin VA slice={slice_index}",
        f"ctx_id={ctx_id} released VA slice {slice_index} "
        f"used=0x{before:016x}->0x0000000000000000",
        f"ctx_id={ctx_id} backend destroy complete",
        f"virgl context backend destroy complete: ctx_id={ctx_id}",
        f"virgl context table remove complete: ctx_id={ctx_id} found=1",
        f"virglrenderer context drop complete: ctx_id={ctx_id}",
        f"rutabaga context destroy complete: ctx_id={ctx_id}",
    ]


class HostTeardownOrchestratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="viogpu-host-teardown-")
        self.root = Path(self.temp.name)
        self.baseline = self.root / "baseline.txt"
        self.capture = self.root / "capture.txt"
        self.ssh_log = self.root / "ssh.log"
        self.baseline.write_text(
            textwrap.dedent(
                """\
                PID=16091
                CROSVM_SHA256=bbd5eacd4d430e68c2fe497be681d1367f590a07d86eb7b60179d8a002ef53dc
                VIRGL_SHA256=77a99067ec8fde12a8261865821f9f6da0f024c5f176663c83dd2214fdc145af
                BASELINE_LINE=100
                BASELINE_INODE=200
                BASELINE_COMPLETE=1
                """
            ),
            encoding="ascii",
        )
        trace = complete_trace(10, 0) + complete_trace(11, 1)
        capture_lines = [
            "PID=16091",
            "BASELINE_LINE=100",
            "BASELINE_INODE=200",
            "CAPTURE_LINE=126",
            "CAPTURE_INODE=200",
            "VA_SLICE_CLAIM=2",
            "CONTEXT_TABLE_ADD=2",
            "===== FILTERED_CONTEXT_TEARDOWN =====",
        ]
        capture_lines.extend(f"{index}:{line}" for index, line in enumerate(trace, 1))
        capture_lines.append("CAPTURE_COMPLETE=1")
        self.capture.write_text("\n".join(capture_lines) + "\n", encoding="ascii")
        self.adb = self._write_executable(
            "adb",
            """#!/bin/sh
case "$*" in
    *" get-state") echo device ;;
    *" push "*) echo pushed ;;
    *" shell chmod 0755 "*) exit 0 ;;
    *" shell sha256sum "*) echo "$FAKE_CAPTURE_HASH  remote-helper" ;;
    *" --baseline") cat "$FAKE_BASELINE" ;;
    *" --capture "*) cat "$FAKE_CAPTURE" ;;
    *) echo "unexpected adb arguments: $*" >&2; exit 90 ;;
esac
""",
        )
        self.ssh = self._write_executable(
            "ssh",
            """#!/bin/sh
printf '%s\n' "$*" >> "$FAKE_SSH_LOG"
case "$*" in
    *" certutil.exe "*) echo "$FAKE_TURNIP_HASH" ;;
    *) echo '{"Success":true}'; exit "${FAKE_WORKLOAD_EXIT:-0}" ;;
esac
""",
        )

    def tearDown(self):
        self.temp.cleanup()

    def _write_executable(self, name, content):
        path = self.root / name
        path.write_text(content, encoding="ascii")
        path.chmod(0o755)
        return path

    def run_orchestrator(self, evidence_name, workload_exit="0"):
        environment = os.environ.copy()
        environment.update(
            {
                "ADB_BIN": str(self.adb),
                "SSH_BIN": str(self.ssh),
                "FAKE_BASELINE": str(self.baseline),
                "FAKE_CAPTURE": str(self.capture),
                "FAKE_CAPTURE_HASH": CAPTURE_HASH,
                "FAKE_TURNIP_HASH": TURNIP_HASH,
                "FAKE_SSH_LOG": str(self.ssh_log),
                "FAKE_WORKLOAD_EXIT": workload_exit,
            }
        )
        evidence = self.root / evidence_name
        result = subprocess.run(
            [str(SCRIPT), str(evidence), "C:/evidence/host-test"],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )
        return result, evidence

    def test_complete_workload_and_host_teardown_pass(self):
        result, evidence = self.run_orchestrator("pass")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CREATED_CONTEXTS=2", result.stdout)
        self.assertIn("HOST_TEARDOWN_COMPLETE=1", result.stdout)
        self.assertIn('"passed": true', (evidence / "host-classification.json").read_text())
        ssh_arguments = self.ssh_log.read_text(encoding="ascii")
        self.assertNotIn(" -p ", ssh_arguments)
        self.assertNotIn(" -i ", ssh_arguments)
        self.assertIn("USER@192.168.60.237", ssh_arguments)
        self.assertIn("powershell.exe -NoProfile -NonInteractive", ssh_arguments)

    def test_workload_failure_still_captures_and_classifies(self):
        result, evidence = self.run_orchestrator("workload-failure", "7")
        self.assertEqual(result.returncode, 8, result.stderr)
        self.assertEqual((evidence / "windows-workload.exit.txt").read_text(), "7\n")
        self.assertEqual((evidence / "host-capture.exit.txt").read_text(), "0\n")
        self.assertEqual((evidence / "host-classification.exit.txt").read_text(), "0\n")


if __name__ == "__main__":
    unittest.main()
