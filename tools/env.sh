# Source from Git Bash: the portable devkitPro (tools/fetch_devkitpro.py) and MSYS2 make.
SLIPPI_WII="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="$SLIPPI_WII/.toolchain/opt/devkitpro"
export DEVKITPPC="$DEVKITPRO/devkitPPC"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$SLIPPI_WII/.toolchain/msys/usr/bin:$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$DEVKITARM/bin:$PATH"
