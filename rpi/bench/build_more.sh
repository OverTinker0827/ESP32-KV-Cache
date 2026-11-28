#!/bin/bash
# After build_latest.sh: Garnet v2.2.14 (official linux-arm64 release + Microsoft's .NET runtime installer)
# and KeyDB v6.3.4 (Snapchat/KeyDB source).
while ! grep -q "builds done" ~/kvpi/results/build_latest.log 2>/dev/null; do sleep 10; done
cd ~/kvpi/src
echo "== garnet $(date +%T)"
mkdir -p garnet && curl -sSL -o garnet/linux-arm64-based.tar.xz https://github.com/microsoft/garnet/releases/download/v2.2.14/linux-arm64-based.tar.xz \
  && tar xJf garnet/linux-arm64-based.tar.xz -C garnet && ls garnet | head
ls garnet/*/ 2>/dev/null | head -5
find garnet -maxdepth 3 -name "*.runtimeconfig.json" | head -2 | xargs -r cat
echo "== keydb $(date +%T)"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq uuid-dev libcurl4-openssl-dev libbz2-dev libzstd-dev liblz4-dev libsnappy-dev >/dev/null 2>&1
[ -d keydb ] || git clone -q --depth 1 --branch v6.3.4 https://github.com/Snapchat/KeyDB.git keydb
make -C keydb -j4 BUILD_TLS=no ENABLE_FLASH=no >/tmp/build_keydb.log 2>&1; echo keydb rc=$?; tail -5 /tmp/build_keydb.log
./keydb/src/keydb-server --version
echo "== more done $(date +%T)"
