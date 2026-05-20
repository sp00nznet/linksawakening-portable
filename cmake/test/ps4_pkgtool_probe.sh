#!/bin/bash
# Try to coax PkgTool.Core's bundled .NET runtime into using OpenSSL 3.x.
export OO_PS4_TOOLCHAIN="$HOME/PS4Toolchain"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
PKGTOOL="$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core"

echo "=== .NET runtime info embedded in PkgTool ==="
strings "$PKGTOOL" 2>/dev/null | grep -iE 'Microsoft.NETCore.App|netcoreapp|net[0-9]\.[0-9]' | sort -u | head -8

echo
echo "=== attempt 1: CLR_OPENSSL_VERSION_OVERRIDE=3 ==="
CLR_OPENSSL_VERSION_OVERRIDE=3 "$PKGTOOL" sfo_new /tmp/_t.sfo 2>&1 | head -4
echo "exit=$?"

echo
echo "=== attempt 2: CLR_OPENSSL_VERSION_OVERRIDE=1.1 ==="
CLR_OPENSSL_VERSION_OVERRIDE=1.1 "$PKGTOOL" sfo_new /tmp/_t2.sfo 2>&1 | head -4
echo "exit=$?"

echo
echo "=== which attempt produced an sfo ==="
ls -la /tmp/_t.sfo /tmp/_t2.sfo 2>/dev/null
