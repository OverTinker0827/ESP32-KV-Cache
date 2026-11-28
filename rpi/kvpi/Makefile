# kvpi: Pi 5 KV cache. VARIANT flags select the measured optimization steps.
CC ?= gcc
OPT ?= -O2
ARCH ?= -march=armv8.2-a+crc
DEFS ?=
SRC ?=
CFLAGS = -I$(or $(SRC),.) $(OPT) $(ARCH) -g -Wall -Wextra -Wno-unused-parameter -pthread $(DEFS)
TAG ?= v0
B = build/$(TAG)

all: $(B)/bench_engine $(B)/test_engine $(B)/kvpi-server-uring

$(B):
	mkdir -p $(B)
$(B)/bench_engine: $(SRC)bench_engine.c $(SRC)engine.c $(SRC)engine.h | $(B)
	$(CC) $(CFLAGS) $(SRC)bench_engine.c $(SRC)engine.c -o $@
$(B)/test_engine: $(SRC)test_engine.c $(SRC)engine.c $(SRC)engine.h | $(B)
	$(CC) $(CFLAGS) $(SRC)test_engine.c $(SRC)engine.c -o $@
$(B)/kvpi-server: server.c engine.c engine.h | $(B)
	$(CC) $(CFLAGS) server.c engine.c -o $@
clean:
	rm -rf build
$(B)/kvpi-server-mt: server_mt.c store.h proto.h engine.c engine.h | $(B)
	$(CC) $(CFLAGS) server_mt.c engine.c -o $@
$(B)/kvpi-server-uring: $(SRC)server_uring.c $(SRC)store.h $(SRC)proto.h $(SRC)engine.c $(SRC)engine.h | $(B)
	$(CC) $(CFLAGS) $(SRC)server_uring.c $(SRC)engine.c $(if $(SRC),,objects.c) -luring -lm -o $@

# v3 steps, each one build on top of the previous (measured separately in results.md):
#  v3a: -O3 -mcpu=cortex-a76   v3b: + CRC32C hash   v3c: + fast parser/dispatch/formatting   v3d: + batched prefetch
v3:
	$(MAKE) SRC=frozen_v4/ TAG=v3a OPT=-O3 ARCH=-mcpu=cortex-a76 build/v3a/kvpi-server-uring build/v3a/bench_engine
	$(MAKE) SRC=frozen_v4/ TAG=v3b OPT=-O3 ARCH=-mcpu=cortex-a76 DEFS="-DKV_HASH_CRC=1" build/v3b/kvpi-server-uring build/v3b/bench_engine
	$(MAKE) SRC=frozen_v4/ TAG=v3c OPT=-O3 ARCH=-mcpu=cortex-a76 DEFS="-DKV_HASH_CRC=1 -DKV_FAST=1" build/v3c/kvpi-server-uring
	$(MAKE) SRC=frozen_v4/ TAG=v3d OPT=-O3 ARCH=-mcpu=cortex-a76 DEFS="-DKV_HASH_CRC=1 -DKV_FAST=1 -DKV_BATCH=1" build/v3d/kvpi-server-uring build/v3d/test_engine
# v4: v3d + network-path options (runtime: -M 1 multishot recv, -b <us> busy-poll, -q <cpu> SQPOLL)
v4:
	$(MAKE) SRC=frozen_v4/ TAG=v4 OPT=-O3 ARCH=-mcpu=cortex-a76 DEFS="-DKV_HASH_CRC=1 -DKV_FAST=1 -DKV_BATCH=1" build/v4/kvpi-server-uring
# v5: features (expiry, maxmemory + LRU eviction, string/list/hash/set/zset commands) on v4
v5:
	$(MAKE) TAG=v5 OPT=-O3 ARCH=-mcpu=cortex-a76 DEFS="-DKV_HASH_CRC=1 -DKV_FAST=1 -DKV_BATCH=1" build/v5/kvpi-server-uring build/v5/test_engine build/v5/bench_engine build/v5/test_features
$(B)/test_features: test_features.c engine.c engine.h | $(B)
	$(CC) $(CFLAGS) test_features.c engine.c -o $@
# v6: v5 + QSBR (old index tables freed), per-class page sizing, large values (>= 8 KiB) in refcounted blobs sent
#     with sendmsg iovecs (no copy into the reply buffer), 16 KiB provided receive buffers
v6:
	$(MAKE) TAG=v6 OPT=-O3 ARCH=-mcpu=cortex-a76 DEFS="-DKV_HASH_CRC=1 -DKV_FAST=1 -DKV_BATCH=1" build/v6/kvpi-server-uring build/v6/test_engine build/v6/test_features
