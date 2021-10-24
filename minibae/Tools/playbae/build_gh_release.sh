#!/bin/bash
BDIR="$(realpath $(pwd))"
ODIR="${BDIR}/ghrel_out"


function install_file() {
	if [ -f "${1}" ]; then
		mv "${1}" "${2}"
	else
		exit 1
	fi
}

rm -f "${ODIR}/"*
rmdir "${ODIR}" 2>/dev/null
mkdir "${ODIR}"

echo "Building Linux 32bit..."
make -f Makefile clean pack 2>/dev/null > /dev/null
install_file "${BDIR}/playbae_static.gz" "${ODIR}/playbae_linux32_static.gz"
make -f Makefile clean 2>/dev/null > /dev/null

echo "Building MingW32..."
make -f Makefile.mingw clean pack 2>/dev/null > /dev/null
install_file "${BDIR}/playbae.exe.gz" "${ODIR}/playbae.exe.gz"
make -f Makefile.mingw clean 2>/dev/null > /dev/null

echo "Building MingW32 Libraries..."
cd ../..
make -f Makefile.mingw clean all 2>/dev/null > /dev/null
zip -u "${ODIR}/libMiniBAE_win_DLL.zip" *.dll 2>/dev/null > /dev/null
make -f Makefile.mingw clean 2>/dev/null > /dev/null

cd "${BDIR}"
ls -l "${ODIR}"

