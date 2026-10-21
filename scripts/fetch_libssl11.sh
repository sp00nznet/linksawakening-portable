#!/bin/bash
# Download libssl1.1 (.deb) so PkgTool.Core's .NET Core 3.0 runtime can do
# the crypto in `pkg_build`. Tries a few mirrors. No sudo needed to fetch;
# the install (dpkg -i) is a separate sudo step.
set -e
cd "$HOME"   # home persists across WSL restarts; /tmp does not
DEB="libssl1.1_1.1.1w-0+deb11u1_amd64.deb"
URLS=(
  "http://ftp.us.debian.org/debian/pool/main/o/openssl/$DEB"
  "http://archive.debian.org/debian/pool/main/o/openssl/$DEB"
  "http://security.debian.org/debian-security/pool/updates/main/o/openssl/libssl1.1_1.1.1w-0+deb11u5_amd64.deb"
)
for u in "${URLS[@]}"; do
  echo "trying: $u"
  if wget -q --timeout=40 -O "$HOME/libssl11.deb" "$u"; then
    sz=$(stat -c%s "$HOME/libssl11.deb" 2>/dev/null || echo 0)
    if [ "$sz" -gt 100000 ]; then
      echo "OK — downloaded $sz bytes to $HOME/libssl11.deb"
      exit 0
    fi
  fi
  echo "  ...failed, next mirror"
done
echo "ERROR: all mirrors failed"
exit 1
