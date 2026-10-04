// Removes the Omnuv bundles that came before Connect's own version.
//
//     onv-replace-old.exe <old bundle upgrade code>
//
// **Why this exists** (4 October 2026). Until 0.2.2 the bundle took
// upstream's version, 6.1.0, and Burn refuses to install a bundle over a
// higher version of itself (exit 1638, measured on the install rig). So
// Connect's bundle has its own upgrade code, and this, chained after the
// MSI, uninstalls any bundle still registered under the old one.
//
// **After the MSI, not before**, on purpose. The new MSI's major upgrade has
// already removed the old MSI by then, as an upgrade: its tunnel service is
// stopped and removed by its own ServiceControl, and its uninstall-only
// cleanup (deleting HKCU\Software\Omnuv, where the client keeps its saved
// address and settings) does not run, because UPGRADINGPRODUCTCODE is set.
// What is left of the old bundle is its registration and its cached copy,
// and its own quiet uninstall removes those and finds nothing else to do.
// The sign-in token is in Credential Manager, which no uninstall touches.
//
// Compiled by the .NET Framework's csc.exe, present on every Windows 10 and
// later, so it is C# 5 and needs no runtime the bundle does not have.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using Microsoft.Win32;

static class ReplaceOld
{
    const string Uninstall = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";

    static string Bare(string code)
    {
        return code.Trim().Trim('{', '}').ToUpperInvariant();
    }

    static int Main(string[] args)
    {
        if (args.Length != 1) { Console.Error.WriteLine("usage: onv-replace-old <old bundle upgrade code>"); return 2; }
        string old = Bare(args[0]);
        var found = new List<KeyValuePair<string, string>>();
        // Burn registers a per-machine bundle in whichever view its engine
        // runs in, so both are read.
        foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            using (RegistryKey hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
            using (RegistryKey root = hklm.OpenSubKey(Uninstall))
            {
                if (root == null) continue;
                foreach (string name in root.GetSubKeyNames())
                {
                    using (RegistryKey k = root.OpenSubKey(name))
                    {
                        if (k == null) continue;
                        string[] codes = k.GetValue("BundleUpgradeCode") as string[];
                        if (codes == null) continue;
                        bool ours = false;
                        foreach (string c in codes) if (Bare(c) == old) ours = true;
                        if (!ours) continue;
                        string quiet = k.GetValue("QuietUninstallString") as string;
                        Console.WriteLine("OLDBUNDLE=" + name + " " + k.GetValue("DisplayVersion"));
                        if (string.IsNullOrEmpty(quiet)) { Console.Error.WriteLine("no quiet uninstall for " + name); return 1; }
                        found.Add(new KeyValuePair<string, string>(name, quiet));
                    }
                }
            }
        }
        foreach (var b in found)
        {
            // Burn writes `"<cached bundle>" /uninstall /quiet`.
            string cmd = b.Value.Trim();
            string exe, rest;
            if (cmd.StartsWith("\""))
            {
                int end = cmd.IndexOf('"', 1);
                if (end < 0) { Console.Error.WriteLine("unparsable uninstall for " + b.Key + ": " + cmd); return 1; }
                exe = cmd.Substring(1, end - 1);
                rest = cmd.Substring(end + 1).Trim();
            }
            else
            {
                int sp = cmd.IndexOf(' ');
                exe = sp < 0 ? cmd : cmd.Substring(0, sp);
                rest = sp < 0 ? "" : cmd.Substring(sp + 1).Trim();
            }
            var p = Process.Start(new ProcessStartInfo(exe, rest + " /norestart") { UseShellExecute = false });
            p.WaitForExit();
            Console.WriteLine("REMOVED=" + b.Key + " exit=" + p.ExitCode);
            // 3010: done, a restart would finish it. Anything else is a
            // failure, and a vital one: two entries in installed programs is
            // what this exists to prevent.
            if (p.ExitCode != 0 && p.ExitCode != 3010) return p.ExitCode;
        }
        if (found.Count == 0) Console.WriteLine("OLDBUNDLE=none");
        return 0;
    }
}
