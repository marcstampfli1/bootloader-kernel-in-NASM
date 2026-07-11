#!/usr/bin/env bash
# Cross-build the Avian JVM (interpreter) for MakaOS.
#
# Produces a statically-linked MakaOS x86-64 `avian` executable plus its
# classpath.jar (Avian's bundled minimal class library).  Build with AVIANTEST=1
# to install them onto the disk image and run /avian/Hello.class at boot; see
# docs/JVM_PORT_PLAN.md.
#
# Requires:
#   - a host JDK (javac/java) for Avian's classpath compile + generators
#     (JAVA_HOME auto-detected from `javac` if unset);
#   - a prior MakaOS build so build/sysroot has the fresh libc.a + C++-safe
#     headers (run `bash build.sh` first).
#
# Design notes (why the flags):
#   - process=interpret bootimage=false  : no JIT bootimage cross-generation.
#   - classpath=avian                    : Avian's own tiny class library (no OpenJDK).
#   - java-version=8                     : Avian parses the old `javac 1.x` version
#                                          string; a modern JDK breaks it, so pin 8
#                                          (-> `-source 1.8`, still supported).
#   - cc/cxx = makaos toolchain + --sysroot ; build-cc/build-cxx = host (Avian's
#     type/bootimage generators must run on the host).
#   - rdynamic=-Wl,--export-dynamic      : the makaos gcc rejects bare -rdynamic.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$(pwd)"
AVIAN="$REPO/build/third_party/avian"
SYSROOT="$REPO/build/sysroot"
TOOLS="$REPO/toolchain/bin"
OUT="$REPO/build/avian-app"
BUILT="$AVIAN/build/linux-x86_64-interpret"

: "${JAVA_HOME:=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
export JAVA_HOME
[ -f "$JAVA_HOME/bin/javac" ] || { echo "FATAL: no host JDK (set JAVA_HOME)"; exit 1; }
[ -f "$SYSROOT/usr/lib/libc.a" ] || { echo "FATAL: run 'bash build.sh' first (no build/sysroot)"; exit 1; }
[ -x "$TOOLS/x86_64-pc-makaos-g++" ] || { echo "FATAL: makaos toolchain missing"; exit 1; }

# 1. Fetch Avian.
if [ ! -d "$AVIAN" ]; then
    echo "[port-avian] cloning Avian"
    mkdir -p "$REPO/build/third_party"
    git clone --depth 1 https://github.com/ReadyTalk/avian "$AVIAN"
fi

# 2. Portability patches (idempotent) -- Avian quirks, not MakaOS bugs.
#    (a) posix.cpp relies on a non-POSIX transitive limits.h for PATH_MAX.
#    (b) java-lang.cpp includes the unused, removed-in-modern-libc sys/sysctl.h.
grep -q '__FreeBSD__ || defined __makaos__' "$AVIAN/src/system/posix.cpp" || \
    sed -i 's/#if defined __FreeBSD__$/#if defined __FreeBSD__ || defined __makaos__/' \
        "$AVIAN/src/system/posix.cpp"
grep -q '!defined __ANDROID__ && !defined __makaos__' "$AVIAN/classpath/java-lang.cpp" || \
    perl -0pi -e 's/#ifndef __ANDROID__\n#include "sys\/sysctl.h"/#if !defined __ANDROID__ && !defined __makaos__\n#include "sys\/sysctl.h"/' \
        "$AVIAN/classpath/java-lang.cpp"

# 3. Cross-build the VM executable + classpath.jar.
MCC="$TOOLS/x86_64-pc-makaos-gcc --sysroot=$SYSROOT"
MCXX="$TOOLS/x86_64-pc-makaos-g++ --sysroot=$SYSROOT"
echo "[port-avian] cross-building (JAVA_HOME=$JAVA_HOME)"
make -C "$AVIAN" -j"$(nproc)" \
    platform=linux arch=x86_64 process=interpret bootimage=false classpath=avian \
    java-version=8 \
    cc="$MCC" cxx="$MCXX" build-cc=gcc build-cxx=g++ rdynamic=-Wl,--export-dynamic \
    "$BUILT/avian" "$BUILT/classpath.jar"

# 4. Stage artifacts for build.sh's AVIANTEST installer.
mkdir -p "$OUT"
cp "$BUILT/avian"         "$OUT/avian"
cp "$BUILT/classpath.jar" "$OUT/classpath.jar"

# 5. A trivial Hello.class targeting Java 8 bytecode (Avian is old; it will not
#    read modern classfiles).
cat > "$OUT/Hello.java" <<'JAVA'
public class Hello {
    public static void main(String[] args) {
        System.out.println("Hello from Avian JVM on MakaOS");
    }
}
JAVA
"$JAVA_HOME/bin/javac" -source 8 -target 8 -d "$OUT" "$OUT/Hello.java" 2>/dev/null

echo "[port-avian] done: $OUT/{avian,classpath.jar,Hello.class}"
echo "[port-avian] run first-light with:  AVIANTEST=1 CONSOLE_SERIAL=1 bash build.sh"
