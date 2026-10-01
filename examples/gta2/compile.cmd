@echo off
rem Native Windows version of compile.sh: compiles one gta2_re translation unit
rem with VC6, using the gta2_lib flags.
rem
rem   compile.cmd <src.cpp> <out.obj>
rem
rem GTA2_RE must point at a gta2_re checkout with the
rem 3rdParty\gta2_re_compile_tools submodule. EXTRA_CFLAGS adds per-file flags.
rem Precompiled headers are used unless NO_PCH or PERMUTER_NO_PCH is set (see
rem compile.sh for why).
rem
rem Use it as: cpp_permuter -c "examples\gta2\compile.cmd {src} {obj}" ...
setlocal
if "%GTA2_RE%"=="" (
    echo set GTA2_RE to your gta2_re checkout
    exit /b 2
)
set "TOOLS=%GTA2_RE%\3rdParty\gta2_re_compile_tools"
set "PATH=%TOOLS%\VC98\Bin;%TOOLS%\Common\MSDev98\Bin;%PATH%"
set "INCLUDE=%TOOLS%\VC98\ATL\Include;%TOOLS%\VC98\Include;%TOOLS%\VC98\MFC\Include"
set "LIB=%TOOLS%\VC98\Lib"
set "SRC=%~f1"
set "OBJ=%~f2"
set "PCH=%~dpn2.pch"
set "LOG=%~dpn2.log"
rem the source's own directory first, so a candidate in cpp_permuter's mirror
rem sees the mirror's headers ("." keeps the trailing backslash off the quote)
set "SRCDIR=%~dp1."
set FLAGS=/DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /D_CRT_NON_CONFORMING_SWPRINTFS /DIMGUI_DLL /W3 /EHsc /GX /ML /O2 /DNDEBUG %EXTRA_CFLAGS%

if defined NO_PCH goto plain
if defined PERMUTER_NO_PCH goto plain
cl.exe /nologo /TP /c /I"%SRCDIR%" /I"%GTA2_RE%" /I"%TOOLS%" %FLAGS% /Zm500 /YX /Fp"%PCH%" /Fo"%OBJ%" "%SRC%" > "%LOG%" 2>&1
if not errorlevel 1 (
    type "%LOG%"
    exit /b 0
)
rem only fall back when the precompiled header itself was the problem
findstr /R "C1060 C1076 C1083 C1852 C1853 C1859 C2859" "%LOG%" >nul
if errorlevel 1 (
    type "%LOG%"
    exit /b 2
)

:plain
cl.exe /nologo /TP /c /I"%SRCDIR%" /I"%GTA2_RE%" /I"%TOOLS%" %FLAGS% /Zm1000 /Fo"%OBJ%" "%SRC%"
