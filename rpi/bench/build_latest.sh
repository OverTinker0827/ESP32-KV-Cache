#!/bin/bash
# Build the newest Redis and Valkey from their official source tags (after the running benchmark ends).
while ! grep -q "## done" ~/kvpi/results/run_all_p3.log; do sleep 10; done
set -e
cd ~/kvpi/src
echo "== redis 8.12.0 $(date +%T)"
[ -d redis-8.12.0 ] || git clone -q --depth 1 --branch 8.12.0 https://github.com/redis/redis.git redis-8.12.0
make -C redis-8.12.0 -j4 BUILD_TLS=no >/tmp/build_redis.log 2>&1 && ./redis-8.12.0/src/redis-server --version
echo "== valkey 9.2.1 $(date +%T)"
[ -d valkey-9.2.1 ] || git clone -q --depth 1 --branch 9.2.1 https://github.com/valkey-io/valkey.git valkey-9.2.1
make -C valkey-9.2.1 -j4 BUILD_TLS=no >/tmp/build_valkey.log 2>&1 && ./valkey-9.2.1/src/valkey-server --version
echo "== builds done $(date +%T)"
