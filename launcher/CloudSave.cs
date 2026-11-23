using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using UFC3Launcher.Tabs;

namespace UFC3Launcher;

/// <summary>
/// Cloud save folder (ufc3_cloud_save_dir in ufc3.toml, e.g. %OneDrive%\SaveGames\UFC3ReX-Enhanced): a copy of the
/// saves kept in a folder a cloud client syncs (OneDrive), so two PCs (desktop + handheld) share one career.
///
/// The game keeps playing from its own userdata\ folder; only the launcher touches the cloud folder, and never while
/// the game runs:
///   - before Play: a warning if the game is running on another device (its in-use file is fresh), then bring this
///     device up to date (download when only the cloud copy changed, upload when only this device's did, ask when
///     both did), copy DLC packages either way when one side lacks them, and match the career length;
///   - while the game runs: the in-use file (device, heartbeat every minute);
///   - after the game exits: upload what the game saved.
///
/// What is synced: every profile's title folder (userdata\&lt;profile&gt;\5451087D\: the UFC2012_SYSTEM_DATA save =
/// settings, unlocks, careers, created fighters, and its header) and the achievements. Not synced: the highlight
/// replays (UFC2012_HLR_QUE_nnn, 12 MB each), the local safety copies (ufc3-save-backup, ufc3-career-backup), the
/// cache. DLC (userdata\0000000000000000\5451087D\) is copied, never deleted.
///
/// Safety: the cloud's ufc3-cloud-sync.json lists every synced file with its SHA-256. A download copies exactly those
/// files into a staging folder and checks every hash before anything local is replaced, so a half-synced cloud folder
/// (or a OneDrive conflict copy) is never used. Whatever a sync replaces is copied aside first (userdata\
/// ufc3-cloud-backup\ for this device, &lt;cloud&gt;\backups\ for the cloud copy). A save the game was writing when it
/// stopped (save_guard's .saving marker) is not uploaded.
/// </summary>
public static class CloudSave
{
    public const string TomlKey = "ufc3_cloud_save_dir";
    const string Title = "5451087D";
    const string DlcProfile = "0000000000000000";
    const string ManifestName = "ufc3-cloud-sync.json";
    const string LockName = "ufc3-in-use.json";
    const string StateName = "ufc3-cloud-state.json";
    const string Tmp = ".ufc3tmp";
    static readonly TimeSpan LockFresh = TimeSpan.FromMinutes(3);
    const string Caption = "Cloud saves";

    public record FileEntry(string Path, long Size, string Sha256);
    public record Manifest(int Version, int Seq, string Device, DateTime Time, string Fingerprint, int CareerFights, List<FileEntry> Files);
    record State(string CloudDir, int Seq, string Fingerprint);
    record InUse(string Device, DateTime Started, DateTime Heartbeat);

    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    /// <summary>This PC's name in the cloud files. UFC3_DEVICE_NAME overrides it (tests: two "devices" on one PC).</summary>
    static string Device => Environment.GetEnvironmentVariable("UFC3_DEVICE_NAME") is { Length: > 0 } d ? d : Environment.MachineName;

    /// <summary>Every question and notice goes through here (the --cloud-sync command answers them itself).</summary>
    public static Func<string, MessageBoxButtons, MessageBoxIcon, DialogResult> Prompt =
        (text, buttons, icon) => DarkBox.Show(text, Caption, buttons, icon);
    static System.Threading.Timer? heartbeat;

    // ---- settings ----

    /// <summary>The folder as written in ufc3.toml (may hold %OneDrive%), or "".</summary>
    public static string Configured
    {
        get { try { return new Toml().GetStr(TomlKey, "").Trim(); } catch { return ""; } }
    }

    public static bool Enabled => Configured.Length > 0;

    /// <summary>%VARS% expanded; null when one is left unexpanded (OneDrive not set up on this PC).</summary>
    public static string? Resolve(string configured)
    {
        if (configured.Length == 0) return null;
        var p = Environment.ExpandEnvironmentVariables(configured);
        if (p.Contains('%')) return null;
        try { return Path.GetFullPath(p); } catch { return null; }
    }

    /// <summary>A picked folder, written with %OneDrive% (or %USERPROFILE%) in front when it lives there, so the same
    /// setting works on a PC where that folder has another path.</summary>
    public static string Portable(string folder)
    {
        folder = Path.GetFullPath(folder).TrimEnd('\\');
        foreach (var v in new[] { "OneDrive", "OneDriveConsumer", "OneDriveCommercial", "USERPROFILE" })
        {
            var root = Environment.GetEnvironmentVariable(v)?.TrimEnd('\\');
            if (string.IsNullOrEmpty(root)) continue;
            if (folder.Equals(root, StringComparison.OrdinalIgnoreCase)) return "%" + v + "%";
            if (folder.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase)) return "%" + v + "%" + folder[root.Length..];
        }
        return folder;
    }

    // ---- before / after a run ----

    /// <summary>Before the game starts. False: don't start it (the player said no). Shows its own dialogs; heavy
    /// copies run off the UI thread with the window kept responsive.</summary>
    public static bool BeforePlay(Action<string>? status = null)
    {
        if (!Enabled) return true;
        var cloud = Resolve(Configured);
        if (cloud == null)
            return Ask($"The cloud save folder can't be found on this PC:\n{Configured}\n\nPlay without syncing your saves?");
        try
        {
            Directory.CreateDirectory(cloud);
            var other = ReadJson<InUse>(Path.Combine(cloud, LockName));
            if (other != null && !SameDevice(other.Device) && DateTime.UtcNow - other.Heartbeat < LockFresh)
            {
                var min = (int)(DateTime.UtcNow - other.Heartbeat).TotalMinutes;
                var ago = min < 1 ? "less than a minute" : min == 1 ? "1 minute" : $"{min} minutes";
                if (!Ask($"UFC 3 is running on {other.Device} (last seen {ago} ago).\n\n" +
                         "Playing here now can overwrite the progress made there. Close the game on that device first.\n\nPlay anyway?"))
                    return false;
            }
            status?.Invoke("Syncing saves with the cloud folder...");
            if (!SyncSaves(cloud)) return false;
            Pumped(() => SyncDlc(cloud));
            MatchCareerLength(cloud);
            StartHeartbeat(cloud);
            status?.Invoke("");
            return true;
        }
        catch (Exception ex)
        {
            Log("ERROR before play: " + ex.Message);
            status?.Invoke("");
            return Ask("Your saves could not be synced with the cloud folder:\n" + ex.Message + "\n\nPlay without syncing?");
        }
    }

    /// <summary>After the run (any thread): waits for every ufc3.exe to exit (the emulated fallback is a second
    /// process), then uploads what the game saved. No dialogs; logged to launcher-cloud.log.</summary>
    public static void AfterPlay()
    {
        if (!Enabled) return;
        var cloud = Resolve(Configured);
        if (cloud == null) return;
        try
        {
            foreach (var p in Process.GetProcessesByName("ufc3")) { try { p.WaitForExit(); } catch { } }
            StopHeartbeat();
            if (SaveInProgress())
            {
                Log("not uploaded: a save was being written when the game stopped (it is restored at the next start)");
                return;
            }
            var local = List(Paths.UserData);
            var fp = Fingerprint(local);
            var st = ReadState(cloud);
            var m = ReadJson<Manifest>(Path.Combine(cloud, ManifestName));
            bool careerChanged = m != null && m.CareerFights != CareerFights();
            if (st != null && fp == st.Fingerprint && !careerChanged) { Log("after play: nothing new to upload"); return; }
            // Only over the copy this run started from: a run that didn't sync first (st null) never overwrites the cloud.
            if (m == null || (st != null && m.Seq == st.Seq)) { Upload(cloud, local, fp, m, backupCloud: false); return; }
            Log(st == null
                ? "not uploaded: this run didn't start from the cloud copy (not synced first); you'll be asked at the next start"
                : $"not uploaded: the cloud copy changed meanwhile (from {m.Device}); you'll be asked at the next start");
        }
        catch (Exception ex) { Log("ERROR after play: " + ex.Message); }
        finally
        {
            StopHeartbeat();
            ReleaseLock(cloud);
        }
    }

    /// <summary>For a start the launcher doesn't wait for (emulated renderer): without a cloud folder the launcher
    /// quits as before; with one it hides and stays until the game exits, to upload the saves.</summary>
    public static void WaitThenExit(Control from)
    {
        if (!Enabled) { Application.Exit(); return; }
        var form = from.FindForm();
        form?.Hide();
        new Thread(() =>
        {
            AfterPlay();
            if (form != null && form.IsHandleCreated) form.BeginInvoke(Application.Exit); else Application.Exit();
        }) { IsBackground = false, Name = "cloud-wait" }.Start();
    }

    /// <summary>--cloud-sync before|after [--answer yes|no|cancel]: one step without the game, for tests. Questions are
    /// printed and get the given answer (default yes); prints launcher-cloud.log's new lines. Exit 0 = go ahead.</summary>
    public static int Cli(string[] args)
    {
        var ans = args.SkipWhile(a => a != "--answer").Skip(1).FirstOrDefault() ?? "yes";
        var res = ans == "no" ? DialogResult.No : ans == "cancel" ? DialogResult.Cancel : DialogResult.Yes;
        Prompt = (text, buttons, _) =>
        {
            Console.WriteLine("[question] " + text.Replace("\n", " / ") + $" -> {(buttons == MessageBoxButtons.OK ? "OK" : res.ToString())}");
            return buttons == MessageBoxButtons.OK ? DialogResult.OK : res;
        };
        var log = Path.Combine(Paths.Root, "launcher-cloud.log");
        long from = File.Exists(log) ? new FileInfo(log).Length : 0;
        bool ok = true;
        if (args.Length > 1 && args[1] == "after") { AfterPlay(); }
        else { ok = BeforePlay(s => { if (s.Length > 0) Console.WriteLine("[status] " + s); }); StopHeartbeat(); }
        if (File.Exists(log)) { using var f = File.OpenRead(log); f.Seek(from, SeekOrigin.Begin); Console.Write(new StreamReader(f).ReadToEnd()); }
        Console.WriteLine(ok ? "result: play" : "result: don't play");
        return ok ? 0 : 1;
    }

    // ---- saves ----

    static bool SyncSaves(string cloud)
    {
        var mPath = Path.Combine(cloud, ManifestName);
        var m = ReadJson<Manifest>(mPath);
        List<FileEntry> local = null!;
        Pumped(() => local = List(Paths.UserData));
        var fp = Fingerprint(local);
        var st = ReadState(cloud);

        if (m == null)
        {
            if (local.Count == 0) { Log("no saves yet on this device or in the cloud"); return true; }
            Pumped(() => Upload(cloud, local, fp, null, backupCloud: false));   // first device: seed the cloud
            return true;
        }
        if (m.Fingerprint == fp) { WriteState(new State(cloud, m.Seq, fp)); return true; }

        bool cloudChanged = st == null || st.Seq != m.Seq;
        bool localChanged = st == null || st.Fingerprint != fp;
        if (local.Count == 0) localChanged = false;   // a new device: just take the cloud's
        if (!cloudChanged) { Pumped(() => Upload(cloud, local, fp, m, backupCloud: false)); return true; }
        if (!localChanged) return Download(cloud, m);

        var mine = local.Count == 0 ? "no saves" : $"last changed {LocalTime(Paths.UserData, local):g}";
        var r = Prompt(
            "Your saves on this device and in the cloud folder are different.\n\n" +
            $"Cloud folder: saved on {m.Device}, {m.Time.ToLocalTime():g}\nThis device ({Device}): {mine}\n\n" +
            "Yes: use the cloud saves (this device's saves are kept in userdata\\ufc3-cloud-backup).\n" +
            "No: keep this device's saves and upload them (the cloud ones are kept in the cloud folder's backups).\n" +
            "Cancel: don't play now.",
            MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
        if (r == DialogResult.Yes) return Download(cloud, m);
        if (r == DialogResult.No) { Pumped(() => Upload(cloud, local, fp, m, backupCloud: true)); return true; }
        return false;
    }

    /// <summary>Cloud -> this device. False only when the player chose not to play.</summary>
    static bool Download(string cloud, Manifest m)
    {
        string? problem = null;
        Pumped(() =>
        {
            var src = Path.Combine(cloud, "userdata");
            var stage = Path.Combine(Paths.UserData, "ufc3-cloud-incoming");
            if (Directory.Exists(stage)) Directory.Delete(stage, true);
            foreach (var f in m.Files)
            {
                var from = Path.Combine(src, f.Path);
                if (!File.Exists(from)) { problem = "missing " + f.Path; break; }
                var to = Path.Combine(stage, f.Path);
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                File.Copy(from, to, true);
                if (new FileInfo(to).Length != f.Size || Sha(to) != f.Sha256) { problem = "changed " + f.Path; break; }
            }
            if (problem != null) { try { Directory.Delete(stage, true); } catch { } return; }

            var local = List(Paths.UserData);
            if (local.Count > 0) CopySet(Paths.UserData, local, Path.Combine(Paths.UserData, "ufc3-cloud-backup", Stamp() + "-before-download"));
            Prune(Path.Combine(Paths.UserData, "ufc3-cloud-backup"), 10);
            var keep = new HashSet<string>(m.Files.Select(f => f.Path), StringComparer.OrdinalIgnoreCase);
            foreach (var f in local.Where(f => !keep.Contains(f.Path))) File.Delete(Path.Combine(Paths.UserData, f.Path));
            foreach (var f in m.Files)
            {
                var to = Path.Combine(Paths.UserData, f.Path);
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                File.Move(Path.Combine(stage, f.Path), to, true);
            }
            Directory.Delete(stage, true);
            WriteState(new State(cloud, m.Seq, m.Fingerprint));
            Log($"downloaded {m.Files.Count} files (saved on {m.Device}, {m.Time:u}, seq {m.Seq})");
        });
        if (problem == null) return true;
        Log("download skipped: " + problem);
        return Ask("The cloud folder isn't fully synced yet (" + problem + "), so its saves were not used.\n\n" +
                   "Wait for OneDrive to finish, then press Play again. Play now with this device's saves?");
    }

    /// <summary>This device -> cloud. Files are written beside their place and moved in; the list goes last, so a
    /// reader never takes a half-written copy.</summary>
    static void Upload(string cloud, List<FileEntry> local, string fp, Manifest? old, bool backupCloud)
    {
        var dst = Path.Combine(cloud, "userdata");
        if (backupCloud && old != null)
        {
            var have = old.Files.Where(f => File.Exists(Path.Combine(dst, f.Path))).ToList();
            CopySet(dst, have, Path.Combine(cloud, "backups", $"{Stamp()}-from-{Safe(old.Device)}"));
            Prune(Path.Combine(cloud, "backups"), 10);
        }
        foreach (var f in local)
        {
            var to = Path.Combine(dst, f.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            File.Copy(Path.Combine(Paths.UserData, f.Path), to + Tmp, true);
            File.Move(to + Tmp, to, true);
        }
        if (old != null)
        {
            var now = new HashSet<string>(local.Select(f => f.Path), StringComparer.OrdinalIgnoreCase);
            foreach (var f in old.Files.Where(f => !now.Contains(f.Path)))
                try { File.Delete(Path.Combine(dst, f.Path)); } catch { }
        }
        var m = new Manifest(1, (old?.Seq ?? 0) + 1, Device, DateTime.UtcNow, fp, CareerFights(), local);
        WriteJson(Path.Combine(cloud, ManifestName), m);
        WriteState(new State(cloud, m.Seq, fp));
        Log($"uploaded {local.Count} files (seq {m.Seq})");
    }

    /// <summary>The synced files under a userdata folder (this device's, or the cloud's copy).</summary>
    static List<FileEntry> List(string root)
    {
        var res = new List<FileEntry>();
        if (!Directory.Exists(root)) return res;
        var ach = Path.Combine(root, "achievements", Title + ".toml");
        if (File.Exists(ach)) res.Add(Entry(root, ach));
        foreach (var prof in Directory.EnumerateDirectories(root))
        {
            var name = Path.GetFileName(prof);
            if (name.Length != 16 || !name.All(Uri.IsHexDigit) || name == DlcProfile) continue;
            var title = Path.Combine(prof, Title);
            if (!Directory.Exists(title)) continue;
            foreach (var f in Directory.EnumerateFiles(title, "*", SearchOption.AllDirectories))
            {
                var rel = Path.GetRelativePath(root, f);
                if (Synced(rel)) res.Add(Entry(root, f));
            }
        }
        res.Sort((a, b) => string.Compare(a.Path, b.Path, StringComparison.OrdinalIgnoreCase));
        return res;
    }

    static bool Synced(string rel)
    {
        if (rel.EndsWith(Tmp, StringComparison.OrdinalIgnoreCase)) return false;
        foreach (var part in rel.Split('\\', '/'))
        {
            if (part.StartsWith("UFC2012_HLR_QUE", StringComparison.OrdinalIgnoreCase)) return false;   // replays
            if (part.Equals("ufc3-save-backup", StringComparison.OrdinalIgnoreCase)) return false;
            if (part.Equals("ufc3-career-backup", StringComparison.OrdinalIgnoreCase)) return false;
        }
        return true;
    }

    static FileEntry Entry(string root, string file) =>
        new(Path.GetRelativePath(root, file), new FileInfo(file).Length, Sha(file));

    static string Fingerprint(List<FileEntry> files)
    {
        var sb = new StringBuilder();
        foreach (var f in files) sb.Append(f.Path.ToUpperInvariant()).Append('\t').Append(f.Size).Append('\t').Append(f.Sha256).Append('\n');
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString()))).ToLowerInvariant();
    }

    /// <summary>save_guard's marker: the game was stopped while writing a save.</summary>
    static bool SaveInProgress()
    {
        if (!Directory.Exists(Paths.UserData)) return false;
        foreach (var prof in Directory.EnumerateDirectories(Paths.UserData))
        {
            var guard = Path.Combine(prof, Title, "ufc3-save-backup");
            if (Directory.Exists(guard) && Directory.EnumerateFiles(guard, "*.saving", SearchOption.AllDirectories).Any()) return true;
        }
        return false;
    }

    // ---- DLC ----

    /// <summary>DLC packages are read-only: copy a file to whichever side lacks it, never delete.</summary>
    static void SyncDlc(string cloud)
    {
        var mine = Path.Combine(Paths.UserData, DlcProfile, Title);
        var theirs = Path.Combine(cloud, "dlc", DlcProfile, Title);
        int down = CopyMissing(theirs, mine), up = CopyMissing(mine, theirs);
        if (down + up > 0) Log($"DLC: {down} files from the cloud, {up} to the cloud");
    }

    static int CopyMissing(string from, string to)
    {
        if (!Directory.Exists(from)) return 0;
        int n = 0;
        foreach (var f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            if (f.EndsWith(Tmp, StringComparison.OrdinalIgnoreCase)) continue;
            var dest = Path.Combine(to, Path.GetRelativePath(from, f));
            if (File.Exists(dest) && new FileInfo(dest).Length == new FileInfo(f).Length) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(f, dest + Tmp, true);
            File.Move(dest + Tmp, dest, true);
            n++;
        }
        return n;
    }

    // ---- career length ----

    static int CareerFights() { try { return new Toml().GetInt("ufc3_career_fights", 48); } catch { return 48; } }

    /// <summary>A career longer than this device's setting would be retired at its next check: raise the setting to
    /// the one the saves were played with (never lowered here).</summary>
    static void MatchCareerLength(string cloud)
    {
        var m = ReadJson<Manifest>(Path.Combine(cloud, ManifestName));
        int mine = CareerFights();
        if (m == null || m.CareerFights <= mine) return;
        var t = new Toml(); t.Set("ufc3_career_fights", m.CareerFights); t.Save();
        Log($"career length {mine} -> {m.CareerFights} fights to match the synced saves");
        Prompt($"Career length set to {m.CareerFights} fights to match your synced saves (it was {mine} on this device).\n\n" +
                     "A shorter setting would end a longer career at its next check.", MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    // ---- in-use file ----

    static void StartHeartbeat(string cloud)
    {
        StopHeartbeat();
        var started = DateTime.UtcNow;
        var path = Path.Combine(cloud, LockName);
        void Beat() { try { WriteJson(path, new InUse(Device, started, DateTime.UtcNow)); } catch { } }
        Beat();
        heartbeat = new System.Threading.Timer(_ => Beat(), null, TimeSpan.FromMinutes(1), TimeSpan.FromMinutes(1));
    }

    static void StopHeartbeat() { heartbeat?.Dispose(); heartbeat = null; }

    static void ReleaseLock(string cloud)
    {
        try
        {
            var path = Path.Combine(cloud, LockName);
            var l = ReadJson<InUse>(path);
            if (l != null && SameDevice(l.Device)) File.Delete(path);
        }
        catch { }
    }

    static bool SameDevice(string d) => d.Equals(Device, StringComparison.OrdinalIgnoreCase);

    // ---- helpers ----

    /// <summary>Runs <paramref name="work"/> off the UI thread while keeping the window responsive (on the UI
    /// thread), or inline elsewhere.</summary>
    static void Pumped(Action work)
    {
        if (SynchronizationContext.Current is not WindowsFormsSynchronizationContext) { work(); return; }
        var t = Task.Run(work);
        while (!t.IsCompleted) { Application.DoEvents(); t.Wait(30); }
        t.GetAwaiter().GetResult();
    }

    static bool Ask(string text) => Prompt(text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes;

    static void CopySet(string root, List<FileEntry> files, string dest)
    {
        foreach (var f in files)
        {
            var to = Path.Combine(dest, f.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            File.Copy(Path.Combine(root, f.Path), to, true);
        }
    }

    static void Prune(string dir, int keep)
    {
        if (!Directory.Exists(dir)) return;
        foreach (var d in Directory.GetDirectories(dir).OrderByDescending(d => d, StringComparer.Ordinal).Skip(keep))
            try { Directory.Delete(d, true); } catch { }
    }

    static DateTime LocalTime(string root, List<FileEntry> files) =>
        files.Select(f => File.GetLastWriteTime(Path.Combine(root, f.Path))).DefaultIfEmpty(DateTime.MinValue).Max();

    static string Sha(string file)
    {
        using var s = File.OpenRead(file);
        return Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
    }

    static string Stamp() => DateTime.Now.ToString("yyyyMMdd-HHmmss");
    static string Safe(string s) => string.Concat(s.Select(c => char.IsLetterOrDigit(c) || c == '-' ? c : '_'));

    static State? ReadState(string cloud)
    {
        var s = ReadJson<State>(Path.Combine(Paths.UserData, StateName));
        return s != null && string.Equals(s.CloudDir, cloud, StringComparison.OrdinalIgnoreCase) ? s : null;
    }

    static void WriteState(State s)
    {
        Directory.CreateDirectory(Paths.UserData);
        WriteJson(Path.Combine(Paths.UserData, StateName), s);
    }

    static T? ReadJson<T>(string path) where T : class
    {
        try { return File.Exists(path) ? JsonSerializer.Deserialize<T>(File.ReadAllText(path)) : null; }
        catch { return null; }
    }

    static void WriteJson<T>(string path, T value)
    {
        File.WriteAllText(path + Tmp, JsonSerializer.Serialize(value, Json));
        File.Move(path + Tmp, path, true);
    }

    static void Log(string line)
    {
        try { File.AppendAllText(Path.Combine(Paths.Root, "launcher-cloud.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}"); }
        catch { }
    }
}
