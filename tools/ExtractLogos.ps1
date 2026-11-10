# Copies each tool's logo, at the highest resolution installed on this PC, into
# assets\logos. The logos belong to their companies: they are used only inside
# this private app, are not committed (see .gitignore), and are re-created on
# another machine by running this script. The build copies them beside the
# executable.
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\assets\logos' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class ShellImage {
    [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItemImageFactory { void GetImage(Size size, int flags, out IntPtr bitmap); }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr bc, [MarshalAs(UnmanagedType.LPStruct)] Guid riid, [MarshalAs(UnmanagedType.Interface)] out IShellItemImageFactory factory);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr handle);
    // The largest icon Windows has for a file (an executable's own icon), with transparency.
    public static Bitmap Icon(string path, int size) {
        IShellItemImageFactory factory;
        SHCreateItemFromParsingName(path, IntPtr.Zero, typeof(IShellItemImageFactory).GUID, out factory);
        IntPtr handle;
        factory.GetImage(new Size(size, size), 0x1 /* SIIGBF_BIGGERSIZEOK */ | 0x4 /* SIIGBF_ICONONLY */, out handle);
        try {
            using (var source = Image.FromHbitmap(handle)) {
                var bits = source.LockBits(new Rectangle(0, 0, source.Width, source.Height), System.Drawing.Imaging.ImageLockMode.ReadOnly, source.PixelFormat);
                var copy = new Bitmap(source.Width, source.Height, bits.Stride, System.Drawing.Imaging.PixelFormat.Format32bppArgb, bits.Scan0);
                var result = new Bitmap(copy);
                source.UnlockBits(bits);
                return result;
            }
        } finally { DeleteObject(handle); }
    }
}
'@

function Copy-Logo([string]$Source, [string]$Name) {
    if (Test-Path -LiteralPath $Source) {
        Copy-Item -LiteralPath $Source -Destination (Join-Path $OutputDirectory $Name) -Force
        Write-Output "$Name  <-  $Source"
    } else {
        Write-Warning "Not found: $Source"
    }
}

# ChatGPT / Codex desktop app (MSIX). The "unplated" knot is white; "lightunplated" is black.
$codex = Get-AppxPackage OpenAI.Codex | Sort-Object Version -Descending | Select-Object -First 1
if ($codex) {
    $a = Join-Path $codex.InstallLocation 'assets'
    Copy-Logo (Join-Path $a 'Square44x44Logo.targetsize-256_altform-unplated.png') 'chatgpt-white.png'
    Copy-Logo (Join-Path $a 'Square44x44Logo.targetsize-256_altform-lightunplated.png') 'chatgpt-black.png'
} else { Write-Warning 'The ChatGPT (Codex) desktop app is not installed.' }

# Claude desktop app (MSIX): the orange tile, largest at 310x310 scale-400 (1240 px).
$claude = Get-AppxPackage Claude | Sort-Object Version -Descending | Select-Object -First 1
if ($claude) {
    $a = Join-Path $claude.InstallLocation 'Assets'
    Copy-Logo (Join-Path $a 'Square310x310Logo.scale-400.png') 'claude-large.png'
    Copy-Logo (Join-Path $a 'Square44x44Logo.targetsize-256.png') 'claude.png'
} else { Write-Warning 'The Claude desktop app is not installed.' }

# Antigravity, the agy command line, Claude Code and the Gemini app: their executables' own icons.
foreach ($pair in @(
    @{ Path = Join-Path $env:LOCALAPPDATA 'Programs\Antigravity\Antigravity.exe'; Name = 'antigravity.png' },
    @{ Path = Join-Path $env:LOCALAPPDATA 'agy\bin\agy.exe'; Name = 'agy.png' },
    @{ Path = Join-Path $env:USERPROFILE '.local\bin\claude.exe'; Name = 'claude-cli.png' },
    @{ Path = Join-Path $env:LOCALAPPDATA 'Google\Gemini\Gemini.exe'; Name = 'gemini.png' })) {
    if (Test-Path -LiteralPath $pair.Path) {
        $bitmap = [ShellImage]::Icon($pair.Path, 256)
        $bitmap.Save((Join-Path $OutputDirectory $pair.Name), [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Output "$($pair.Name)  <-  $($pair.Path) ($($bitmap.Width) px)"
        $bitmap.Dispose()
    } else { Write-Warning "Not found: $($pair.Path)" }
}
