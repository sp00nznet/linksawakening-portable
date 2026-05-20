#!/bin/bash
echo "--- libssl on system ---"
find /usr/lib /lib -name 'libssl*' 2>/dev/null
echo "--- libcrypto ---"
find /usr/lib /lib -name 'libcrypto*' 2>/dev/null
echo "--- dpkg ssl packages ---"
dpkg -l 2>/dev/null | grep -iE 'libssl|openssl' | awk '{print $1, $2, $3}'
echo "--- PkgTool linked .NET version ---"
"$HOME/PS4Toolchain/bin/linux/PkgTool.Core" --version 2>&1 | head -3
