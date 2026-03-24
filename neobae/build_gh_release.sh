#!/bin/bash
RDIR="$(realpath "$(pwd)")"
BDIR="${RDIR}/bin"
ODIR="${RDIR}/out"

export USE_FLUIDSYNTH=1
SILENT=1

if  [ "${1}" == "testing" ]; then
	shift;
	SILENT=0
	function signit() {
  		echo "Skipping signing for testing build..."
		mv "${1}" "${2}"
	}
else
	function signit() {
  	# Custom for zefie's Jenkins build system
  	osslsigncode sign \
	    -certs /opt/signkey/signcert.pem \
	    -key /opt/signkey/signkey.pem \
	    -n "zefie's NeoBAE" \
	    -i "https://www.soundmusicsys.com" \
	    -t "http://timestamp.digicert.com" \
	    -in "${1}" "${2}"
	}
fi

if [ -n "$1" ]; then
	if [[ "$1" =~ ^[0-9]+$ ]]; then
		SKIPTO=$1
		echo "Skipping to build ${1}"
		if [ -n "${2}" ]; then
			SILENT=0
		fi
	else
		SILENT=0
	fi
fi


function runcmd() {
	if [ ${SILENT} -eq 1 ]; then
		"${@}" 2>/dev/null > /dev/null;
	else
		"${@}"
	fi
}

function install_file() {
	if [ -f "${1}" ]; then
		mv "${1}" "${2}"
	else
		echo "Could not find file ${1}"
		exit 1
	fi
}

rm -f "${ODIR}/"*
rmdir "${ODIR}" 2>/dev/null
mkdir "${ODIR}"

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 2 ]; then
	export USE_SDL3=1
	export BITS=32
	echo "Building playbae SDL3 x32..."
	runcmd make clean
	runcmd make -f Makefile.mingw "-j$(nproc)" all
    signit "${BDIR}/playbae.exe" "${BDIR}/playbae_signed.exe"
    mv "${BDIR}/playbae_signed.exe" "${BDIR}/playbae.exe"
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9 "${ODIR}/playbae_win_sdl3_x32.zip" -- playbae.exe libfluidsynth*.dll SDL*.dll liblzma*.dll libmp3lame*.dll
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9 "${ODIR}/libNeoBAE_win_sdl3_x32.zip" -- *.dll *.lib *.a
	runcmd zip -d "${ODIR}/playbae_win_sdl3_x32.zip" -- SDL2_ttf.dll SDL3_ttf.dll
	runcmd zip -d "${ODIR}/libNeoBAE_win_sdl3_x32.zip" -- SDL2_ttf.dll SDL3_ttf.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 3 ]; then
	export USE_SDL3=1
	export BITS=64
	echo "Building playbae SDL3 x64..."
	runcmd make clean
	runcmd make -f Makefile.mingw "-j$(nproc)" all
    signit "${BDIR}/playbae.exe" "${BDIR}/playbae_signed.exe"
    mv "${BDIR}/playbae_signed.exe" "${BDIR}/playbae.exe"
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9 "${ODIR}/playbae_win_sdl3_x64.zip" -- playbae.exe libfluidsynth*.dll SDL*.dll liblzma*.dll libmp3lame*.dll
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9 "${ODIR}/libNeoBAE_win_sdl3_x64.zip" -- *.dll *.lib *.a
	runcmd zip -d "${ODIR}/playbae_win_sdl3_x64.zip" -- SDL2_ttf.dll SDL3_ttf.dll
	runcmd zip -d "${ODIR}/libNeoBAE_win_sdl3_x64.zip" -- SDL2_ttf.dll SDL3_ttf.dll	
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 4 ]; then
	export USE_SDL3=1
	export BITS=32
	echo "Building zefidi SDL3 GUI x32..."
	runcmd make clean
	runcmd make -f Makefile.gui-mingw "-j$(nproc)" all
    signit "${BDIR}/zefidi.exe" "${BDIR}/zefidi_signed.exe"
    mv "${BDIR}/zefidi_signed.exe" "${BDIR}/zefidi.exe"
	signit "${BDIR}/RegisterFiletypes.exe" "${BDIR}/RegisterFiletypes_signed.exe"
	mv "${BDIR}/RegisterFiletypes_signed.exe" "${BDIR}/RegisterFiletypes.exe"
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9 "${ODIR}/zefidi_win_sdl3_x32.zip" -- zefidi.exe RegisterFiletypes.exe libfluidsynth*.dll SDL*.dll liblzma*.dll libmp3lame*.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.gui-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 5 ]; then
	export USE_SDL3=1
	export BITS=64
	echo "Building zefidi SDL3 GUI x64..."
	runcmd make clean
	runcmd make -f Makefile.gui-mingw "-j$(nproc)" all
    signit "${BDIR}/zefidi.exe" "${BDIR}/zefidi_signed.exe"
    mv "${BDIR}/zefidi_signed.exe" "${BDIR}/zefidi.exe"
	signit "${BDIR}/RegisterFiletypes.exe" "${BDIR}/RegisterFiletypes_signed.exe"
	mv "${BDIR}/RegisterFiletypes_signed.exe" "${BDIR}/RegisterFiletypes.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9 "${ODIR}/zefidi_win_sdl3_x64.zip" -- zefidi.exe RegisterFiletypes.exe libfluidsynth*.dll SDL*.dll liblzma*.dll libmp3lame*.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.gui-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 6 ]; then
	export BITS=32
	echo "Building RMFInfo (x32)..."
	runcmd make clean
	runcmd make -f Makefile.rmfinfo-mingw "-j$(nproc)" all
	signit "${BDIR}/rmfinfo.exe" "${BDIR}/rmfinfo_signed.exe"
    mv "${BDIR}/rmfinfo_signed.exe" "${BDIR}/rmfinfo.exe"
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x32.zip" -- rmfinfo.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.rmfinfo-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 7 ]; then
	export BITS=64
	echo "Building RMFInfo (x64)..."
	runcmd make clean
	runcmd make -f Makefile.rmfinfo-mingw "-j$(nproc)" all
	signit "${BDIR}/rmfinfo.exe" "${BDIR}/rmfinfo_signed.exe"
    mv "${BDIR}/rmfinfo_signed.exe" "${BDIR}/rmfinfo.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x64.zip" -- rmfinfo.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.rmfinfo-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 8 ]; then
	export BITS=32
	echo "Building RMF2MID (x32)..."
	runcmd make clean
	runcmd make -f Makefile.rmf2mid-mingw "-j$(nproc)" all
	signit "${BDIR}/rmf2mid.exe" "${BDIR}/rmf2mid_signed.exe"
    mv "${BDIR}/rmf2mid_signed.exe" "${BDIR}/rmf2mid.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x32.zip" -- rmf2mid.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.rmf2mid-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 9 ]; then
	export BITS=64
	echo "Building RMF2MID (x64)..."
	runcmd make clean
	runcmd make -f Makefile.rmf2mid-mingw "-j$(nproc)" all
	signit "${BDIR}/rmf2mid.exe" "${BDIR}/rmf2mid_signed.exe"
    mv "${BDIR}/rmf2mid_signed.exe" "${BDIR}/rmf2mid.exe"
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x64.zip" -- rmf2mid.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.rmf2mid-mingw clean
fi


if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 9 ]; then
	export BITS=32
	echo "Building MID2RMF (x32)..."
	runcmd make clean
	runcmd make -f Makefile.mid2rmf-mingw "-j$(nproc)" all
	signit "${BDIR}/mid2rmf.exe" "${BDIR}/mid2rmf_signed.exe"
    mv "${BDIR}/mid2rmf_signed.exe" "${BDIR}/mid2rmf.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x32.zip" -- mid2rmf.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mid2rmf-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 10 ]; then
	export BITS=64
	echo "Building MID2RMF (x64)..."
	runcmd make clean
	runcmd make -f Makefile.mid2rmf-mingw "-j$(nproc)" all
	signit "${BDIR}/mid2rmf.exe" "${BDIR}/mid2rmf_signed.exe"
    mv "${BDIR}/mid2rmf_signed.exe" "${BDIR}/mid2rmf.exe"		
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x64.zip" -- mid2rmf.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mid2rmf-mingw clean
fi


if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 11 ]; then
	export BITS=32
	echo "Building MID2RMI (x32)..."
	runcmd make clean
	runcmd make -f Makefile.mid2rmi-mingw "-j$(nproc)" all
	signit "${BDIR}/mid2rmi.exe" "${BDIR}/mid2rmi_signed.exe"
    mv "${BDIR}/mid2rmi_signed.exe" "${BDIR}/mid2rmi.exe"		
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x32.zip" -- mid2rmi.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mid2rmi-mingw clean
fi


if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 12 ]; then
	export BITS=64
	echo "Building MID2RMI (x64)..."
	runcmd make clean
	runcmd make -f Makefile.mid2rmi-mingw "-j$(nproc)" all
	signit "${BDIR}/mid2rmi.exe" "${BDIR}/mid2rmi_signed.exe"
    mv "${BDIR}/mid2rmi_signed.exe" "${BDIR}/mid2rmi.exe"		
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x64.zip" -- mid2rmi.exe
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mid2rmi-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 13 ]; then
	export BITS=32
	echo "Building MOD2RMF (x32)..."
	runcmd make clean
	runcmd make -f Makefile.mod2rmf-mingw "-j$(nproc)" all
	signit "${BDIR}/mod2rmf.exe" "${BDIR}/mod2rmf_signed.exe"
    mv "${BDIR}/mod2rmf_signed.exe" "${BDIR}/mod2rmf.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x32.zip" -- mod2rmf.exe liblzma*.dll libmp3lame*.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mod2rmf-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 14 ]; then
	export BITS=64
	echo "Building MOD2RMF (x64)..."
	runcmd make clean
	runcmd make -f Makefile.mod2rmf-mingw "-j$(nproc)" all
	signit "${BDIR}/mod2rmf.exe" "${BDIR}/mod2rmf_signed.exe"
    mv "${BDIR}/mod2rmf_signed.exe" "${BDIR}/mod2rmf.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x64.zip" -- mod2rmf.exe liblzma*.dll libmp3lame*.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.mod2rmf-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 15 ]; then
	export BITS=32
	echo "Building RMF-InstDump (x32)..."
	runcmd make clean
	runcmd make -f Makefile.instdump-mingw "-j$(nproc)" all
	signit "${BDIR}/rmf-instdump.exe" "${BDIR}/rmf-instdump_signed.exe"
    mv "${BDIR}/rmf-instdump_signed.exe" "${BDIR}/rmf-instdump.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x32.zip" -- rmf-instdump.exe liblzma*.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.instdump-mingw clean
fi

if [ -z "${SKIPTO}" ] || [ "${SKIPTO}" -le 16 ]; then
	export BITS=64
	echo "Building RMF-InstDump (x64)..."
	runcmd make clean
	runcmd make -f Makefile.instdump-mingw "-j$(nproc)" all
	signit "${BDIR}/rmf-instdump.exe" "${BDIR}/rmf-instdump_signed.exe"
    mv "${BDIR}/rmf-instdump_signed.exe" "${BDIR}/rmf-instdump.exe"	
	runcmd cd "${BDIR}" || exit 1 && runcmd zip -9u "${ODIR}/clitools_win_x64.zip" -- rmf-instdump.exe liblzma*.dll
	runcmd cd "${RDIR}" || exit 1
	runcmd make -f Makefile.instdump-mingw clean
fi


cd "${RDIR}" || exit 1
ls -l "${ODIR}"

