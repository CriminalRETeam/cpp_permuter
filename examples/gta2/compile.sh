#!/bin/sh
# Compiles one gta2_re translation unit with VC6 under wine, using the same
# flags as gta2_lib in gta2_re's CMakeLists.txt / build.py --single_cpp.
#
#   compile.sh <src.cpp> <out.obj>
#
# GTA2_RE must point at a gta2_re checkout with the 3rdParty/gta2_re_compile_tools
# submodule. EXTRA_CFLAGS adds per-file flags (cmake/vc6.cmake), e.g. "/Gz" for
# Network_20324.cpp.
#
# The source's own directory stands in for Source/ on the include path, so a
# candidate compiled in cpp_permuter's mirror of Source/ sees the mirror's
# (possibly edited) headers everywhere.
#
# Use it as: cpp_permuter -c 'examples/gta2/compile.sh {src} {obj}' ...
set -e
SRC=$1
OBJ=$2
: "${GTA2_RE:?set GTA2_RE to your gta2_re checkout}"
TOOLS="$GTA2_RE/3rdParty/gta2_re_compile_tools"

winpath() { printf 'Z:%s' "$(realpath -m "$1")" | tr / '\\'; }

export WINEDEBUG=-all
export WINEPATH="$(winpath "$TOOLS/VC98/Bin");$(winpath "$TOOLS/Common/MSDev98/Bin")"
export INCLUDE="$(winpath "$TOOLS/VC98/ATL/Include");$(winpath "$TOOLS/VC98/Include");$(winpath "$TOOLS/VC98/MFC/Include")"
export LIB="$(winpath "$TOOLS/VC98/Lib")"

FLAGS="/DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /D_CRT_NON_CONFORMING_SWPRINTFS /DIMGUI_DLL \
    /W3 /EHsc /GX /ML /O2 /DNDEBUG $EXTRA_CFLAGS"
INCS="/I$(winpath "$(dirname "$SRC")") /I$(winpath "$GTA2_RE") /I$(winpath "$TOOLS")"

# Precompiled headers (/YX, one .pch next to each object, so per permuter
# worker) halve the compile time. VC6 rebuilds the .pch when a header it holds
# changes, and the code is the same as without it (checked on every function of
# PedGroup.cpp). /Zm500 instead of /Zm1000: with /YX, /Zm1000 runs out of heap
# space under wine. NO_PCH=1 turns it off; if VC6 can't use the .pch the file is
# compiled again without it.
#
# cl.exe exits 0 on warnings and 2 on errors; its output goes to stdout.
if [ -z "${NO_PCH:-}" ]; then
    LOG=$(wine cl.exe /nologo /TP /c $INCS $FLAGS /Zm500 /YX /Fp"$(winpath "${OBJ%.*}.pch")" \
        /Fo"$(winpath "$OBJ")" "$(winpath "$SRC")") && { printf '%s\n' "$LOG"; exit 0; }
    if ! printf '%s' "$LOG" | grep -q -E "C1060|C1076|C1083|C1852|C1853|C1859|C2859"; then
        printf '%s\n' "$LOG"
        exit 2
    fi
fi
wine cl.exe /nologo /TP /c $INCS $FLAGS /Zm1000 /Fo"$(winpath "$OBJ")" "$(winpath "$SRC")"
