#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EDK2=${UNIXNU_EDK2:-$ROOT/package/Mu_Basecore}
export WORKSPACE=$ROOT
export PACKAGES_PATH=$ROOT/drivers/DarwinBoot:$ROOT/package:$EDK2
export EDK_TOOLS_PATH=$EDK2/BaseTools
export CONF_PATH=$ROOT/Conf
if [ ! -f "$EDK2/MdePkg/MdePkg.dec" ]; then
  echo "error: set UNIXNU_EDK2 to a Mu_Basecore checkout (currently $EDK2)" >&2
  exit 1
fi
if [ ! -x "$EDK_TOOLS_PATH/Source/C/bin/GenFw" ]; then
  make -C "$EDK_TOOLS_PATH/Source/C" -j"${JOBS:-4}"
fi
mkdir -p "$CONF_PATH"
for name in target tools_def build_rule; do
  template="$EDK_TOOLS_PATH/Conf/$name.template"
  config="$CONF_PATH/$name.txt"
  if [ ! -f "$template" ]; then
    echo "error: EDK II configuration template is missing: $template" >&2
    exit 1
  fi
  if [ ! -f "$config" ] || [ "$template" -nt "$config" ]; then
    cp "$template" "$config"
  fi
done
if [ -z "${CLANG_BIN:-}" ]; then
  mkdir -p "$ROOT/.cache/toolchain"
  for name in clang lld-link llvm-lib llvm-rc; do
    tool=$(command -v "$name") || { echo "error: $name not found on PATH" >&2; exit 1; }
    ln -sf "$tool" "$ROOT/.cache/toolchain/$name"
  done
  CLANG_BIN=$ROOT/.cache/toolchain/
fi
export CLANG_BIN
export PYTHON_COMMAND=python3
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
cd "$ROOT"
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b "${TARGET:-DEBUG}" -t CLANGPDB -n "${JOBS:-4}"
