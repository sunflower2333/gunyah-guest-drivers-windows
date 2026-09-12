using System.IO;
using ModeHelper = VioGpuDisplayModeTestV2;
public static class DisplayModeHelperTests {
    static int checks, failures;
    static bool Has(string value, string part) { return value != null && value.Contains(part); }
    static void Check(bool value, string label) {
        checks++;
        if (!value) { failures++; Console.WriteLine("FAIL " + label); }
    }
    static byte[] Mode(int hz, int bpp) {
        byte[] mode = new byte[220];
        Put(mode, 168, bpp); Put(mode, 172, 3040); Put(mode, 176, 1904); Put(mode, 184, hz);
        return mode;
    }
    static void Put(byte[] mode, int offset, int value) {
        Array.Copy(BitConverter.GetBytes(value), 0, mode, offset, 4);
    }
    sealed class Backend : ModeHelper.IBackend {
        public byte[] Current = Mode(60, 32), Target = Mode(165, 32);
        public int ApplyResult, RestoreResult, TestResult, SaveResult;
        public bool ApplyChanges = true, RestoreChanges = true, ThrowApply, ThrowRestore;
        public bool UnknownAfterApply, HoldChanges, HoldThrows;
        public int ChangeCalls, ModeCalls, HoldCalls, ReadsAfterApply, SaveCalls;
        public byte[] RestoreRequest;
        public byte[] Read(int index) {
            if (index >= 0) return index == 0 ? (byte[])Target.Clone() : null;
            if (ModeCalls == 1) {
                ReadsAfterApply++;
                if (UnknownAfterApply) return null;
            }
            return (byte[])Current.Clone();
        }
        public int Change(byte[] mode, uint flags) {
            ChangeCalls++;
            if (flags == 2) return TestResult;
            if (flags == 1) { SaveCalls++; return SaveResult; }
            ModeCalls++;
            if (ModeCalls == 1) {
                if (ApplyChanges) Current = (byte[])mode.Clone();
                if (ThrowApply) throw new Exception("apply transport exception");
                return ApplyResult;
            }
            RestoreRequest = (byte[])mode.Clone();
            if (RestoreChanges) Current = (byte[])mode.Clone();
            if (ThrowRestore) throw new Exception("restore transport exception");
            return RestoreResult;
        }
        public void Hold(int milliseconds) {
            HoldCalls++;
            if (milliseconds != 5000) throw new Exception("unexpected hold bound");
            if (HoldChanges) Current = Mode(120, 32);
            if (HoldThrows) throw new Exception("hold interrupted");
        }
    }
    sealed class Execution {
        public ModeHelper.Outcome Result;
        public string Log;
        public string Error;
    }
    static Execution Execute(Backend backend, bool apply, bool keep) {
        var execution = new Execution();
        TextWriter output = Console.Out;
        using (var capture = new StringWriter()) {
            Console.SetOut(capture);
            try {
                execution.Result = ModeHelper.Execute(backend, 3040, 1904, 165, 5, apply, keep);
                try { execution.Result.ThrowIfFailed(); }
                catch (Exception error) { execution.Error = error.Message; }
            } finally { Console.SetOut(output); }
            execution.Log = capture.ToString();
        }
        return execution;
    }
    static void Failed(Execution execution, string label) {
        Check(!execution.Result.Passed && execution.Error != null, label + " cannot pass");
        Check(execution.Log.Contains("OVERALL=FAIL") && !execution.Log.Contains("OVERALL=PASS"), label + " reports failure");
    }
    public static int Main() {
        var backend = new Backend { ApplyResult = -1 };
        var execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 2 && execution.Result.RestoreOutcome == "RESTORED",
            "failed apply with changed mode restores exactly once");
        Check(backend.ReadsAfterApply >= 2, "failed apply queries actual current mode before restoration");
        Check(execution.Result.Apply.Result == -1 && execution.Result.Restore != null && execution.Result.Restore.Result == 0 &&
            execution.Result.Failure != null && execution.Result.Failure.Contains("phase=APPLY flags=0 result=-1"),
            "successful restore retains original failed apply");
        Check(backend.HoldCalls == 0 && backend.SaveCalls == 0, "failed apply never holds or saves");
        Failed(execution, "failed changed apply");

        backend = new Backend { ApplyResult = -1, ApplyChanges = false };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 1 && execution.Result.Restore == null && execution.Result.RestoreOutcome == "UNCHANGED",
            "failed apply with unchanged mode avoids restoration call");
        Check(backend.ReadsAfterApply >= 2, "failed unchanged apply still observes state");
        Failed(execution, "failed unchanged apply");

        backend = new Backend { ApplyResult = -1, RestoreResult = -2, RestoreChanges = false };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 2 && execution.Result.RestoreOutcome == "FAILED", "failed restoration does not retry");
        Check(execution.Result.Failure != null && execution.Result.Failure.Contains("phase=APPLY flags=0 result=-1") &&
            execution.Result.RestoreFailure != null && execution.Result.RestoreFailure.Contains("phase=RESTORE flags=0 result=-2") &&
            execution.Error != null && execution.Error.Contains("phase=APPLY flags=0 result=-1") &&
            execution.Error.Contains("phase=RESTORE flags=0 result=-2"), "restore failure preserves original apply failure and both outcomes");
        Failed(execution, "double failure");

        backend = new Backend();
        execution = Execute(backend, true, false);
        Check(execution.Result.Passed && execution.Error == null && execution.Result.RestoreOutcome == "RESTORED",
            "successful roundtrip passes after verified restoration");
        Check(backend.ModeCalls == 2 && backend.HoldCalls == 1 && backend.SaveCalls == 0,
            "successful roundtrip has one apply one hold one restore");

        backend = new Backend { RestoreResult = -2 };
        execution = Execute(backend, true, false);
        Check(execution.Result.RestoreOutcome == "FAILED" && Has(execution.Result.RestoreFailure, "result=-2"),
            "failed restore return is retained even if original state becomes visible");
        Failed(execution, "successful apply failed restore");

        backend = new Backend { RestoreChanges = false };
        execution = Execute(backend, true, false);
        Check(execution.Result.RestoreOutcome == "FAILED" && Has(execution.Result.RestoreFailure, "did not stick"),
            "restore success return without state change is failure");
        Failed(execution, "restore did not stick");

        backend = new Backend { ThrowApply = true };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 2 && Has(execution.Result.Failure, "apply transport exception"),
            "exception after changed apply restores and retains call exception");
        Failed(execution, "apply exception");

        backend = new Backend { ApplyResult = -1, ThrowRestore = true };
        execution = Execute(backend, true, false);
        Check(execution.Result.Failure != null && execution.Result.Failure.Contains("result=-1") &&
            Has(execution.Result.RestoreFailure, "restore transport exception") && backend.ModeCalls == 2,
            "restoration exception retains original failure without retry");
        Failed(execution, "restore exception");

        backend = new Backend { ApplyResult = -1, UnknownAfterApply = true };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 2 && execution.Result.RestoreOutcome == "RESTORED",
            "unknown current state gets one conservative restore attempt");
        Check(execution.Result.Failure != null && execution.Result.Failure.Contains("result=-1") &&
            execution.Result.Failure.Contains("observation"), "observation failure never hides failed apply");
        Failed(execution, "unknown state");

        backend = new Backend { Current = Mode(60, 16) };
        Put(backend.Current, 76, 50); Put(backend.Current, 84, 1);
        execution = Execute(backend, true, false);
        Check(execution.Result.Passed && backend.RestoreRequest != null &&
            BitConverter.ToInt32(backend.RestoreRequest, 168) == 16 &&
            BitConverter.ToInt32(backend.RestoreRequest, 76) == 50 &&
            BitConverter.ToInt32(backend.RestoreRequest, 84) == 1, "restore preserves original depth position and orientation");

        backend = new Backend();
        execution = Execute(backend, true, true);
        Check(execution.Result.Passed && execution.Result.RestoreOutcome == "KEPT_VALIDATED_MODE" &&
            backend.ModeCalls == 1 && backend.SaveCalls == 1, "keep retains only successfully validated saved mode");

        backend = new Backend { ApplyResult = -1 };
        execution = Execute(backend, true, true);
        Check(backend.ModeCalls == 2 && backend.SaveCalls == 0 && execution.Result.RestoreOutcome == "RESTORED",
            "keep never suppresses restoration of failed apply");
        Failed(execution, "failed keep apply");

        backend = new Backend { SaveResult = -1 };
        execution = Execute(backend, true, true);
        Check(backend.ModeCalls == 2 && execution.Result.Failure != null && execution.Result.Failure.Contains("phase=SAVE"),
            "failed save restores original active mode");
        Failed(execution, "failed save");

        backend = new Backend { HoldChanges = true };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 2 && execution.Result.Failure != null && execution.Result.Failure.Contains("during hold"),
            "mode drift during hold restores original mode");
        Failed(execution, "hold drift");

        backend = new Backend { HoldThrows = true };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 2 && execution.Result.Failure != null && execution.Result.Failure.Contains("hold interrupted"),
            "hold exception restores original mode");
        Failed(execution, "hold exception");

        backend = new Backend();
        execution = Execute(backend, false, false);
        Check(execution.Result.Passed && backend.ChangeCalls == 1 && backend.ModeCalls == 0 &&
            backend.HoldCalls == 0 && backend.SaveCalls == 0, "test only never applies or restores");

        backend = new Backend { TestResult = -1 };
        execution = Execute(backend, true, false);
        Check(backend.ModeCalls == 0 && execution.Result.Restore == null, "failed CDS_TEST never applies or restores");
        Failed(execution, "failed preflight");

        Console.WriteLine("{0} production display mode helper: {1} checks, {2} failures; no user32 calls",
            failures == 0 ? "PASS" : "FAIL", checks, failures);
        return failures == 0 ? 0 : 1;
    }
}
