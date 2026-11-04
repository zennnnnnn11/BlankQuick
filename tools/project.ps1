[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('configure', 'build', 'check', 'clang-tidy', 'clazy', 'cppcheck', 'qmllint', 'format', 'format-check', 'run', 'deploy', 'sync-kernel', 'generate', 'check-generated', 'test')]
    [string] $Action = 'build',
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Debug',
    [string] $QtRoot = $env:QT_ROOT,
    [string] $ClazyExecutable = $env:CLAZY_EXECUTABLE
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$projectRoot = Split-Path -Parent $PSScriptRoot
$preset = $Configuration.ToLowerInvariant()
$buildRoot = Join-Path $projectRoot "out/build/$preset"
$env:QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES = '1'
$env:VSLANG = '1033'

function Invoke-Tool {
    param([string] $Executable, [string[]] $Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE."
    }
}

if ($Action -in 'sync-kernel', 'generate', 'check-generated') {
    $arguments = switch ($Action) {
        'sync-kernel' { @('sync', '--latest') }
        'generate' { @('generate') }
        'check-generated' { @('check', '--offline') }
    }
    Invoke-Tool 'python' (@((Join-Path $PSScriptRoot 'sync_mihomo.py')) + $arguments)
    return
}

function Initialize-Environment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'MSVC Build Tools are required.' }
    Import-Module (Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    if (-not $script:QtRoot) {
        $script:QtRoot = if ($env:Qt6_DIR) {
            [IO.Path]::GetFullPath((Join-Path $env:Qt6_DIR '../../..'))
        } else {
            'C:/Qt/6.12.0/msvc2022_64'
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $script:QtRoot 'lib/cmake/Qt6/Qt6Config.cmake'))) {
        throw 'Set QT_ROOT or pass -QtRoot pointing to a Qt 6.12 MSVC x64 kit.'
    }
    $env:QT_ROOT = $script:QtRoot
    $env:PATH = "$(Join-Path $script:QtRoot 'bin');$env:PATH"
    if ($ClazyExecutable) { $env:CLAZY_EXECUTABLE = $ClazyExecutable }
    if ($Action -in 'configure', 'build') {
        # Convert MSVC OEM diagnostics to Ninja's UTF-8 prefix without lossy decoding.
        $probeRoot = Join-Path $projectRoot 'out/toolchain'
        New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
        $probeHeader = (Join-Path $probeRoot 'include_probe.hpp').Replace('\', '/')
        $probeSource = Join-Path $probeRoot 'include_probe.cpp'
        [IO.File]::WriteAllText($probeHeader, '')
        [IO.File]::WriteAllText($probeSource, '#include "' + $probeHeader + '"' + "`n")
        $probeStart = [Diagnostics.ProcessStartInfo]::new()
        $probeStart.FileName = (Get-Command cl.exe -ErrorAction Stop).Source
        foreach ($argument in @('/nologo', '/showIncludes', '/EP', '/TP', $probeSource)) { $probeStart.ArgumentList.Add($argument) }
        $probeStart.UseShellExecute = $false
        $probeStart.CreateNoWindow = $true
        $probeStart.RedirectStandardOutput = $true
        $probeStart.RedirectStandardError = $true
        $probeProcess = [Diagnostics.Process]::Start($probeStart)
        $probeOut = [IO.MemoryStream]::new()
        $probeErr = [IO.MemoryStream]::new()
        try {
            $outTask = $probeProcess.StandardOutput.BaseStream.CopyToAsync($probeOut)
            $errTask = $probeProcess.StandardError.BaseStream.CopyToAsync($probeErr)
            [Threading.Tasks.Task]::WhenAll([Threading.Tasks.Task[]]@($outTask, $errTask)).GetAwaiter().GetResult()
            $probeProcess.WaitForExit()
            if ($probeProcess.ExitCode -ne 0) { throw 'MSVC include dependency probe failed.' }
            $rawOutput = [Text.Encoding]::Latin1.GetString($probeOut.ToArray()) + [Text.Encoding]::Latin1.GetString($probeErr.ToArray())
            $prefixMatches = [regex]::Matches($rawOutput, '(?m)^(.+?:[ \t]+)(?=[A-Za-z]:[/\\])[^\r\n]*include_probe\.hpp')
            if ($prefixMatches.Count -ne 1) { throw 'Cannot determine MSVC include dependency prefix.' }
            $rawPrefix = $prefixMatches[0].Groups[1].Value.TrimEnd() + ' '
            $oemCodePage = [int](Get-ItemPropertyValue -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage' -Name OEMCP)
            $decodedPrefix = [Text.Encoding]::GetEncoding($oemCodePage).GetString([Text.Encoding]::Latin1.GetBytes($rawPrefix))
            [IO.File]::WriteAllText((Join-Path $probeRoot 'cl-includes-prefix.txt'), $decodedPrefix, [Text.UTF8Encoding]::new($false))
        } finally {
            $probeOut.Dispose()
            $probeErr.Dispose()
            $probeProcess.Dispose()
        }
    }
}

function Get-SourceFiles {
    @(Get-ChildItem -LiteralPath @((Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests/cpp')) -Recurse -File |
        Where-Object Extension -in '.cpp', '.h', '.hpp', '.inc' | Sort-Object FullName)
}

function Invoke-Configure {
    Invoke-Tool 'cmake' @('--preset', $preset, '--warn-uninitialized', '-Werror=dev', '-Werror=deprecated',
        "--graphviz=$buildRoot/architecture-targets.dot")
    Invoke-Tool 'python' @((Join-Path $PSScriptRoot 'check_architecture.py'), '--record-build', $buildRoot)
}

function Get-QmlFiles {
    @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'qml') -Recurse -Filter '*.qml' -File | Sort-Object FullName)
}

function Split-CompilerCommand {
    param([string] $CommandLine)
    if (-not ('BlankQuickTools.CommandLine' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BlankQuickTools {
    public static class CommandLine {
        [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CommandLineToArgvW(string command, out int count);
        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr pointer);
        public static string[] Split(string command) {
            int count;
            var pointer = CommandLineToArgvW(command, out count);
            if (pointer == IntPtr.Zero) { throw new System.ComponentModel.Win32Exception(); }
            try {
                var result = new string[count];
                for (var index = 0; index < count; ++index) {
                    result[index] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(pointer, index * IntPtr.Size));
                }
                return result;
            } finally { LocalFree(pointer); }
        }
    }
}
'@
    }
    return [BlankQuickTools.CommandLine]::Split($CommandLine)
}

function Write-AnalysisDatabases {
    $databasePath = Join-Path $buildRoot 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $databasePath)) { throw 'Run tools/project.ps1 build first.' }
    $commands = @(Get-Content -LiteralPath $databasePath -Raw | ConvertFrom-Json)
    $sourceRoot = (Join-Path $projectRoot 'src').Replace('\', '/') + '/'
    $testRoot = (Join-Path $projectRoot 'tests').Replace('\', '/') + '/'
    $metaHeader = Get-Content -LiteralPath (Join-Path $QtRoot 'include/QtCore/qtmetamacros.h') -Raw
    $mocRevision = [regex]::Match($metaHeader, '(?m)^#define Q_MOC_OUTPUT_REVISION\s+(\d+)').Groups[1].Value
    if (-not $mocRevision) { throw 'Cannot determine the actual SDK MOC revision for Cppcheck.' }
    $nativeEntries = [Collections.Generic.List[object]]::new()
    $cppcheckEntries = [Collections.Generic.List[object]]::new()
    $toolsetVersion = [version]$env:VCToolsVersion.TrimEnd('\', '/')
    $compatibilityVersion = '{0}.{1}' -f ($toolsetVersion.Major + 5), $toolsetVersion.Minor
    foreach ($entry in $commands) {
        $source = $entry.file.Replace('\', '/')
        $isNativeSource = $source.StartsWith($sourceRoot, [StringComparison]::OrdinalIgnoreCase)
        $isTestSource = $source.StartsWith($testRoot, [StringComparison]::OrdinalIgnoreCase)
        if (-not $isNativeSource -and -not $isTestSource) { continue }
        $flags = [Collections.Generic.List[string]]::new()
        $flags.AddRange([string[]]@('--target=x86_64-pc-windows-msvc', '-std=c++20', "-fms-compatibility-version=$compatibilityVersion", '-D_MT', '-D_DLL'))
        if ($Configuration -eq 'Debug') { $flags.Add('-D_DEBUG') }
        $tokens = @(Split-CompilerCommand $entry.command)
        for ($index = 0; $index -lt $tokens.Count; ++$index) {
            $token = $tokens[$index]
            if ($token -cmatch '^[-/]D(.+)$') {
                $flags.Add('-D' + $Matches[1])
            } elseif ($token -cmatch '^[-/](?:external:I|I)(.*)$') {
                $includePath = $Matches[1]
                if (-not $includePath) { $includePath = $tokens[++$index] }
                if (-not [IO.Path]::IsPathRooted($includePath)) { $includePath = Join-Path $entry.directory $includePath }
                $flags.Add($(if ($token -cmatch '^[-/]external:I') { '-isystem' } else { '-I' }))
                $flags.Add($includePath.Replace('\', '/'))
            }
        }
        foreach ($includePath in ($env:INCLUDE -split ';' | Where-Object { $_ })) {
            $flags.Add('-isystem')
            $flags.Add($includePath.Replace('\', '/'))
        }
        # Project headers remain visible to diagnostics; SDK and Qt headers are external.
        $flags.Add('-I' + $sourceRoot.TrimEnd('/'))
        if ($isNativeSource) {
            $nativeEntries.Add(@{ directory = $entry.directory; file = $source; arguments = @('clang++') + $flags.ToArray() + @('-c', $source) })
        }
        $cppcheckFlags = [Collections.Generic.List[string]]::new()
        # Qt's system headers are modeled by qt.cfg; generated call sites require
        # the SDK's real revision so Cppcheck can traverse QTest slot invocations.
        $cppcheckFlags.Add('-DQ_MOC_OUTPUT_REVISION=' + $mocRevision)
        for ($index = 0; $index -lt $flags.Count; ++$index) {
            if ($flags[$index] -in '-isystem', '-I') {
                $isExternal = $flags[$index] -eq '-isystem'
                $includePath = $flags[++$index]
                $isProjectInclude = $includePath.StartsWith($projectRoot.Replace('\', '/'), [StringComparison]::OrdinalIgnoreCase)
                $isGeneratedQtInclude = $includePath -match '[/\\][^/\\]+_autogen[/\\]include(?:_[^/\\]+)?$'
                if ($isProjectInclude -and (-not $isExternal -or $isGeneratedQtInclude)) {
                    $cppcheckFlags.Add('-I' + $includePath)
                }
            } elseif ($flags[$index].StartsWith('-D') -or $flags[$index].StartsWith('-I')) {
                $cppcheckFlags.Add($flags[$index])
            }
        }
        $cppcheckEntries.Add(@{ directory = $entry.directory; file = $source; arguments = @('cl.exe') + $cppcheckFlags.ToArray() + @('/c', $source) })
    }
    if ($nativeEntries.Count -eq 0) { throw 'No project translation units found in compile_commands.json.' }
    foreach ($variant in @('native', 'cppcheck')) {
        $directory = Join-Path $buildRoot "analysis/$variant"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $entries = switch ($variant) {
            'native' { $nativeEntries.ToArray() }
            'cppcheck' { $cppcheckEntries.ToArray() }
        }
        ConvertTo-Json -InputObject @($entries) -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'compile_commands.json') -Encoding utf8NoBOM
    }
    return $nativeEntries.ToArray()
}

function Invoke-Format {
    param([bool] $Check)
    $cppFiles = @(Get-SourceFiles | ForEach-Object FullName)
    $formatArguments = if ($Check) { @('--dry-run', '--Werror', '--fail-on-incomplete-format') } else { @('-i', '--fail-on-incomplete-format') }
    Invoke-Tool 'clang-format' ($formatArguments + $cppFiles)
    foreach ($file in (Get-QmlFiles)) {
        if ($Check) {
            $formatted = & (Join-Path $QtRoot 'bin/qmlformat.exe') $file.FullName
            if ($LASTEXITCODE -ne 0) { throw "qmlformat failed: $($file.FullName)" }
            $expected = ($formatted -join "`n").TrimEnd("`r", "`n") + "`n"
            $actual = [IO.File]::ReadAllText($file.FullName).Replace("`r`n", "`n")
            if ($actual -cne $expected) { throw "QML format mismatch: $($file.FullName). Run tools/project.ps1 format." }
        } else {
            Invoke-Tool (Join-Path $QtRoot 'bin/qmlformat.exe') @('--inplace', $file.FullName)
        }
    }
}

function Invoke-Analysis {
    param([string] $Tool)
    $entries = @(Write-AnalysisDatabases)
    $nativeDatabase = Join-Path $buildRoot 'analysis/native'
    $sourceFiles = @($entries | ForEach-Object file)
    switch ($Tool) {
        'clang-tidy' {
            $nativePattern = '/src/(platform|services|features/(runtime|system_proxy|telemetry))/'
            foreach ($group in @(
                @{ Config = "$projectRoot/.clang-tidy"; Files = @($sourceFiles | Where-Object { $_ -notmatch $nativePattern }) },
                @{ Config = "$projectRoot/cmake/Native.clang-tidy"; Files = @($sourceFiles | Where-Object { $_ -match $nativePattern }) }
            )) {
                if ($group.Files.Count -eq 0) { continue }
                Invoke-Tool 'clang-tidy' (@('--verify-config', "--config-file=$($group.Config)") + $group.Files + @("-p=$nativeDatabase"))
                Invoke-Tool 'clang-tidy' (@("--config-file=$($group.Config)", '--warnings-as-errors=*', "-p=$nativeDatabase") + $group.Files)
            }
        }
        'clazy' {
            # Derive every available check, including manual checks, from the installed Clazy.
            $executable = $ClazyExecutable
            if (-not $executable) {
                $bundledExecutable = Join-Path $projectRoot 'out/tools/clazy/clang64/bin/clazy-standalone.exe'
                $executable = if (Test-Path -LiteralPath $bundledExecutable) { $bundledExecutable } else {
                    $pathCommand = Get-Command clazy-standalone.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
                    if ($pathCommand) { $pathCommand.Source }
                }
            }
            if (-not $executable) { throw 'Windows native Clazy is required. Run tools/setup-clazy.ps1 first, or set CLAZY_EXECUTABLE.' }
            $executable = (Get-Command $executable -CommandType Application -ErrorAction Stop).Source
            $metadata = & $executable --supported-checks-json
            if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate native Clazy checks.' }
            $version = (& $executable --version) -join "`n"
            if ($LASTEXITCODE -ne 0) { throw 'Cannot determine Clazy version.' }
            $exclusions = @(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'clazy-exclusions.json') -Raw | ConvertFrom-Json)
            $excludedChecks = @($exclusions | Where-Object { $version -match ('clazy version ' + [regex]::Escape($_.version) + '(\s|$)') } | ForEach-Object check)
            foreach ($excluded in $excludedChecks) { Write-Host "Clazy tool compatibility exception: $excluded (see tools/clazy-exclusions.json)." }
            $checks = (($metadata -join "`n" | ConvertFrom-Json).checks.name | Where-Object { $_ -notin $excludedChecks } | Sort-Object -Unique) -join ','
            $headerFilter = '^' + [regex]::Escape($projectRoot.Replace('\', '/')) + '/(src|tests|include)/'
            $externalRoots = @($QtRoot, (Join-Path $projectRoot 'third_party')) + @($env:INCLUDE -split ';' | Where-Object { $_ })
            $ignoreDirs = '^(' + (($externalRoots | ForEach-Object { [regex]::Escape($_.Replace('\', '/').TrimEnd('/')) }) -join '|') + ')(/|$)'
            Write-Host "Windows native Clazy: $executable"
            $clazyArguments = @("--checks=$checks", "--header-filter=$headerFilter", "--ignore-dirs=$ignoreDirs", '--extra-arg=-Werror', "-p=$nativeDatabase")
            $llvmVersionMatch = [regex]::Match($version, 'LLVM version ([0-9]+)')
            if ($llvmVersionMatch.Success) {
                $resourceDirectory = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $executable) "../lib/clang/$($llvmVersionMatch.Groups[1].Value)"))
                if (Test-Path -LiteralPath (Join-Path $resourceDirectory 'include')) {
                    $clazyArguments += "--extra-arg=-resource-dir=$resourceDirectory"
                }
            }
            # The MSYS2 PE reserves only 1 MiB, which overflows while Clang parses
            # nlohmann/json with the MSVC STL. Raise only the runner's stack;
            # keep the verified tool/package untouched and every check enabled.
            $runnerRoot = Join-Path $buildRoot 'analysis/clazy-runner'
            New-Item -ItemType Directory -Path $runnerRoot -Force | Out-Null
            $runnerExecutable = Join-Path $runnerRoot 'clazy-standalone.exe'
            Copy-Item -LiteralPath $executable -Destination $runnerExecutable -Force
            Invoke-Tool 'editbin.exe' @('/NOLOGO', '/STACK:16777216', $runnerExecutable)
            $env:PATH = (Split-Path -Parent $executable) + ';' + $env:PATH
            Write-Host 'Clazy runner uses a 16 MiB stack reserve; original executable is unchanged.'
            Invoke-Tool $runnerExecutable ($clazyArguments + $sourceFiles)
        }
        'cppcheck' {
            # Cppcheck's Qt/Windows/standard-library models replace vendor implementation headers.
            $cppcheckProject = Join-Path $buildRoot 'analysis/cppcheck/strict.cppcheck'
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'cppcheck.cppcheck.in') -Destination $cppcheckProject
            # Include callers in tests so public library APIs are checked with
            # their real uses, rather than falsely reported as unused functions.
            # Qt 6.12 moc names parameters intentionally left anonymous by Q_OBJECT.
            # Retain those generated callers; exclude only that spelling diagnostic
            # in generated .moc files, never diagnostics in authored source.
            Invoke-Tool 'cppcheck' @("--project=$cppcheckProject", '--inline-suppr', '--std=c++20', '--platform=win64', '--library=qt', '--library=windows', '--enable=all', '--inconclusive', '--force', '--safety', '--check-level=exhaustive', '--error-exitcode=2', '--suppress=missingIncludeSystem', '--suppress=funcArgNamesDifferentUnnamed:*_autogen/include/*.moc', '--exitcode-suppress=checkersReport', "--checkers-report=$buildRoot/analysis/cppcheck-checkers.txt", '--template=gcc', "--file-filter=$projectRoot/*")
        }
    }
}

function Invoke-QmlLint {
    $files = @(Get-QmlFiles | ForEach-Object FullName)
    Invoke-Tool (Join-Path $QtRoot 'bin/qmllint.exe') (@('--max-warnings=0', '-I', (Join-Path $QtRoot 'qml'), '-I', $buildRoot) + $files)
}

Push-Location -LiteralPath $projectRoot
try {
    Initialize-Environment
    switch ($Action) {
        'configure' { Invoke-Configure }
        'build' {
            Invoke-Configure
            Invoke-Tool 'cmake' @('--build', '--preset', $preset)
        }
        'check' {
            Invoke-Tool 'python' @((Join-Path $PSScriptRoot 'sync_mihomo.py'), 'check', '--offline')
            Invoke-Tool 'python' @((Join-Path $PSScriptRoot 'check_architecture.py'), '--build', $buildRoot)
            Invoke-Tool 'ctest' @('--test-dir', $buildRoot, '--output-on-failure')
            Invoke-Format $true
            foreach ($tool in @('clang-tidy', 'clazy', 'cppcheck')) { Invoke-Analysis $tool }
            Invoke-QmlLint
            Write-Host 'All quality gates passed.'
        }
        'format' { Invoke-Format $false }
        'format-check' { Invoke-Format $true }
        'qmllint' { Invoke-QmlLint }
        'test' { Invoke-Tool 'ctest' @('--test-dir', $buildRoot, '--output-on-failure') }
        'run' { Invoke-Tool (Join-Path $buildRoot 'blank_quick.exe') @() }
        'deploy' { Invoke-Tool 'cmake' @('--install', $buildRoot, '--config', $Configuration) }
        default { Invoke-Analysis $Action }
    }
} finally {
    Pop-Location
}
