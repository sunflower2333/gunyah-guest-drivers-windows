param(
    [ValidateRange(640,8192)][int]$Width = 3040,
    [ValidateRange(480,8192)][int]$Height = 1904,
    [ValidateRange(20,360)][int]$Hz = 165,
    [ValidateRange(5,30)][int]$HoldSeconds = 5,
    [switch]$Apply,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
if ((Get-Process -Id $PID).SessionId -eq 0) { throw 'Must run in the interactive console session' }
if ($Keep -and -not $Apply) { throw '-Keep requires -Apply' }
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class VioGpuDisplayModeTestV2 {
    // This seam lets tests execute the complete production flow without user32.
    public interface IBackend {
        byte[] Read(int index);
        int Change(byte[] mode, uint flags);
        void Hold(int milliseconds);
    }
    sealed class NativeBackend : IBackend {
        [DllImport("user32.dll", CharSet=CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string device, int mode, IntPtr settings);
        [DllImport("user32.dll", CharSet=CharSet.Unicode)]
        static extern int ChangeDisplaySettingsEx(string device, IntPtr settings, IntPtr hwnd, uint flags, IntPtr param);
        public byte[] Read(int index) {
            IntPtr p = Marshal.AllocHGlobal(220);
            try {
                Marshal.Copy(new byte[220], 0, p, 220); Marshal.WriteInt16(p, 68, 220);
                if (!EnumDisplaySettings(null, index, p)) return null;
                var result = new byte[220]; Marshal.Copy(p, result, 0, 220); return result;
            } finally { Marshal.FreeHGlobal(p); }
        }
        public int Change(byte[] mode, uint flags) {
            IntPtr p = Marshal.AllocHGlobal(220);
            try {
                Marshal.Copy(mode, 0, p, 220);
                return ChangeDisplaySettingsEx(null, p, IntPtr.Zero, flags, IntPtr.Zero);
            } finally { Marshal.FreeHGlobal(p); }
        }
        public void Hold(int milliseconds) { System.Threading.Thread.Sleep(milliseconds); }
    }
    public sealed class Attempt {
        public int? Result;
        public byte[] After;
        public string Failure;
    }
    public sealed class Outcome {
        public string Failure;
        public string RestoreFailure;
        public string RestoreOutcome = "NOT_REQUESTED";
        public bool Validated;
        public Attempt Apply;
        public Attempt Restore;
        public bool Passed { get { return Validated && Failure == null && RestoreFailure == null; } }
        public void ThrowIfFailed() {
            if (!Passed) throw new Exception("ORIGINAL_FAILURE=" + (Failure ?? "NONE") +
                "; RESTORE_OUTCOME=" + RestoreOutcome + "; RESTORE_FAILURE=" + (RestoreFailure ?? "NONE"));
        }
    }
    static int Value(byte[] mode, int offset) { return BitConverter.ToInt32(mode, offset); }
    static bool Matches(byte[] mode, int w, int h, int hz) {
        return mode != null && Value(mode,172) == w && Value(mode,176) == h &&
            Value(mode,184) == hz && Value(mode,168) == 32;
    }
    static bool SameMode(byte[] left, byte[] right) {
        if (left == null || right == null) return false;
        // Compare reported display state, not driver/version/padding bytes.
        foreach (int offset in new int[] {76,80,84,88,168,172,176,180,184})
            if (Value(left, offset) != Value(right, offset)) return false;
        return true;
    }
    static void Report(string label, byte[] mode) {
        if (mode == null) Console.WriteLine(label + "=NONE");
        else Console.WriteLine("{0}={1}x{2}@{3} BPP={4}", label, Value(mode,172),
            Value(mode,176), Value(mode,184), Value(mode,168));
    }
    static string Append(string first, string next) { return first == null ? next : first + "; " + next; }
    static Attempt ChangeAndObserve(IBackend backend, byte[] mode, uint flags, string phase) {
        var attempt = new Attempt();
        Console.WriteLine("BEGIN={0} FLAGS={1} UTC={2:O}", phase, flags, DateTime.UtcNow);
        var timer = Stopwatch.StartNew();
        try {
            attempt.Result = backend.Change(mode, flags);
            if (attempt.Result != 0) attempt.Failure = "ChangeDisplaySettingsEx phase=" + phase +
                " flags=" + flags + " result=" + attempt.Result;
        } catch (Exception error) { attempt.Failure = phase + " call: " + error.Message; }
        timer.Stop();
        Console.WriteLine("END={0} RESULT={1} ELAPSED_MS={2}", phase,
            attempt.Result.HasValue ? attempt.Result.Value.ToString() : "EXCEPTION", timer.ElapsedMilliseconds);
        // The system can change state even when its return value indicates failure.
        try {
            attempt.After = backend.Read(-1);
            if (attempt.After == null) throw new Exception("No active display");
        } catch (Exception error) {
            attempt.Failure = Append(attempt.Failure, phase + " observation: " + error.Message);
        }
        Report("AFTER_" + phase, attempt.After);
        return attempt;
    }
    static void RequireSuccess(Attempt attempt) {
        if (attempt.Failure != null) throw new Exception(attempt.Failure);
    }
    public static Outcome Execute(IBackend backend, int w, int h, int hz, int seconds, bool apply, bool keep) {
        var outcome = new Outcome();
        byte[] before = null;
        bool applyAttempted = false;
        try {
            if (keep && !apply) throw new Exception("-Keep requires -Apply");
            if (seconds < 5 || seconds > 30) throw new Exception("Hold must be between 5 and 30 seconds");
            before = backend.Read(-1);
            if (before == null) throw new Exception("No active display");
            before = (byte[])before.Clone();
            Report("BEFORE", before);
            byte[] target = null;
            for (int i = 0; i < 256; i++) {
                byte[] mode = backend.Read(i); if (mode == null) break;
                if (Matches(mode, w, h, hz)) {
                    target = mode; Console.WriteLine("TARGET_INDEX=" + i); break;
                }
            }
            if (target == null) throw new Exception("Requested mode is not enumerated; refusing invented timing");
            RequireSuccess(ChangeAndObserve(backend, target, 2, "TEST"));
            if (apply) {
                applyAttempted = true;
                outcome.Apply = ChangeAndObserve(backend, target, 0, "APPLY");
                RequireSuccess(outcome.Apply);
                if (!Matches(outcome.Apply.After, w, h, hz)) throw new Exception("Selected mode does not match request");
                backend.Hold(seconds * 1000);
                if (!Matches(backend.Read(-1), w, h, hz)) throw new Exception("Mode changed during hold");
                if (keep) {
                    Attempt save = ChangeAndObserve(backend, target, 1, "SAVE");
                    RequireSuccess(save);
                    if (!Matches(save.After, w, h, hz)) throw new Exception("Saved mode does not match request");
                }
            } else { Console.WriteLine("TEST_ONLY=1"); }
            outcome.Validated = true;
        } catch (Exception error) { outcome.Failure = error.Message; }
        finally {
            if (applyAttempted) {
                byte[] current = null;
                try {
                    current = backend.Read(-1);
                    if (current == null) throw new Exception("No active display");
                } catch (Exception error) {
                    outcome.Failure = Append(outcome.Failure, "RESTORE_DECISION observation: " + error.Message);
                }
                Report("RESTORE_DECISION", current);
                if (outcome.Validated && !Matches(current, w, h, hz))
                    outcome.Failure = Append(outcome.Failure, "Mode changed before final observation");
                bool keepValidated = keep && outcome.Validated && outcome.Failure == null && Matches(current, w, h, hz);
                if (keepValidated) { outcome.RestoreOutcome = "KEPT_VALIDATED_MODE"; }
                else if (SameMode(before, current)) { outcome.RestoreOutcome = "UNCHANGED"; }
                else {
                    // One restoration attempt, also when current state is unknown.
                    // Never retry or claim a wall-clock bound on the synchronous OS call.
                    outcome.Restore = ChangeAndObserve(backend, before, 0, "RESTORE");
                    outcome.RestoreFailure = outcome.Restore.Failure;
                    if (!SameMode(before, outcome.Restore.After))
                        outcome.RestoreFailure = Append(outcome.RestoreFailure, "Original display mode restore did not stick");
                    outcome.RestoreOutcome = outcome.RestoreFailure == null ? "RESTORED" : "FAILED";
                }
            }
        }
        Console.WriteLine("ORIGINAL_FAILURE=" + (outcome.Failure ?? "NONE"));
        Console.WriteLine("RESTORE_OUTCOME=" + outcome.RestoreOutcome);
        Console.WriteLine("RESTORE_FAILURE=" + (outcome.RestoreFailure ?? "NONE"));
        Console.WriteLine("OVERALL=" + (outcome.Passed ? "PASS" : "FAIL"));
        return outcome;
    }
    public static void Run(int w, int h, int hz, int seconds, bool apply, bool keep) {
        Execute(new NativeBackend(), w, h, hz, seconds, apply, keep).ThrowIfFailed();
    }
}
'@
[VioGpuDisplayModeTestV2]::Run($Width,$Height,$Hz,$HoldSeconds,[bool]$Apply,[bool]$Keep)
