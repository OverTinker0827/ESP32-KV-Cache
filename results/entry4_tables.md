### Throughput (mean ops/s) and speed-up vs static_hash on the same placement

| design | uniform (n=1) | zipf099 (n=3) | zipf12 (n=3) | hotshard_zipf12 (n=1) | hotshard_zipf12_g50 (n=1) |
|---|---:|---:|---:|---:|---:|
| static_hash | 370,743 (1.00×) | 364,514 (1.00× [1.00–1.00]) | 350,060 (1.00× [1.00–1.00]) | 228,121 (1.00×) | 226,312 (1.00×) |
| roundrobin_rw | 362,692 (0.98×) | 363,222 (1.00× [0.93–1.06]) | 363,025 (1.05× [0.89–1.15]) | 358,544 (1.57×) | 338,192 (1.49×) |
| spill_rw | 376,207 (1.01×) | 376,390 (1.04× [0.98–1.09]) | 382,411 (1.10× [1.00–1.17]) | 361,909 (1.59×) | 335,064 (1.48×) |
| migrate | 390,513 (1.05×) | 403,982 (1.11× [1.03–1.18]) | 408,042 (1.18× [1.00–1.28]) | 241,779 (1.06×) | 238,945 (1.06×) |
| migrate_hyst | 371,168 (1.00×) | 396,567 (1.09× [1.00–1.17]) | 408,104 (1.18× [1.05–1.24]) | 240,195 (1.05×) | 237,415 (1.05×) |
| hotrep_rw | 336,624 (0.91×) | 381,670 (1.05× [0.98–1.11]) | 406,468 (1.17× [1.08–1.24]) | 348,385 (1.53×) | 257,771 (1.14×) |

### Per-worker load and balancing activity (mean over repetitions)

| design | workload | offered to w0 (static map) | w0 / w1 ops/s | spills | migrations | replica hits / fallbacks | lock contended |
|---|---|---:|---:|---:|---:|---:|---:|
| static_hash | uniform | 51 % | 188,074 / 182,669 | 0 | 0 | 0 / 0 | 0.0 % |
| roundrobin_rw | uniform | 51 % | 181,111 / 181,581 | 0 | 0 | 0 / 0 | 1.1 % |
| spill_rw | uniform | 51 % | 173,523 / 202,683 | 8,168 | 0 | 0 / 0 | 0.3 % |
| migrate | uniform | 51 % | 181,156 / 209,356 | 0 | 61 | 0 / 0 | 0.0 % |
| migrate_hyst | uniform | 51 % | 188,334 / 182,834 | 0 | 0 | 0 / 0 | 0.0 % |
| hotrep_rw | uniform | 51 % | 170,186 / 166,438 | 0 | 0 | 274 / 2 | 0.0 % |
| static_hash | zipf099 | 52 % | 188,671 / 175,843 | 0 | 0 | 0 / 0 | 0.0 % |
| roundrobin_rw | zipf099 | 52 % | 181,532 / 181,690 | 0 | 0 | 0 / 0 | 1.3 % |
| spill_rw | zipf099 | 52 % | 173,263 / 203,127 | 10,318 | 0 | 0 / 0 | 0.5 % |
| migrate | zipf099 | 52 % | 183,048 / 220,933 | 0 | 46 | 0 / 0 | 0.0 % |
| migrate_hyst | zipf099 | 52 % | 185,153 / 211,413 | 0 | 1 | 0 / 0 | 0.0 % |
| hotrep_rw | zipf099 | 52 % | 161,963 / 219,706 | 0 | 0 | 21,642 / 112 | 0.0 % |
| static_hash | zipf12 | 52 % | 177,839 / 172,221 | 0 | 0 | 0 / 0 | 0.0 % |
| roundrobin_rw | zipf12 | 52 % | 181,455 / 181,571 | 0 | 0 | 0 / 0 | 1.5 % |
| spill_rw | zipf12 | 52 % | 172,586 / 209,824 | 18,786 | 0 | 0 / 0 | 0.9 % |
| migrate | zipf12 | 52 % | 182,294 / 225,747 | 0 | 28 | 0 / 0 | 0.0 % |
| migrate_hyst | zipf12 | 52 % | 183,024 / 225,079 | 0 | 2 | 0 / 0 | 0.0 % |
| hotrep_rw | zipf12 | 52 % | 164,143 / 242,325 | 0 | 0 | 36,543 / 149 | 0.0 % |
| static_hash | hotshard_zipf12 | 91 % | 207,684 / 20,436 | 0 | 0 | 0 / 0 | 0.0 % |
| roundrobin_rw | hotshard_zipf12 | 91 % | 179,068 / 179,476 | 0 | 0 | 0 / 0 | 6.9 % |
| spill_rw | hotshard_zipf12 | 91 % | 170,678 / 191,231 | 64,482 | 0 | 0 / 0 | 7.5 % |
| migrate | hotshard_zipf12 | 91 % | 205,902 / 35,876 | 0 | 3 | 0 / 0 | 0.0 % |
| migrate_hyst | hotshard_zipf12 | 91 % | 205,872 / 34,323 | 0 | 3 | 0 / 0 | 0.0 % |
| hotrep_rw | hotshard_zipf12 | 91 % | 154,193 / 194,192 | 0 | 0 | 65,188 / 2 | 0.0 % |
| static_hash | hotshard_zipf12_g50 | 92 % | 207,620 / 18,691 | 0 | 0 | 0 / 0 | 0.0 % |
| roundrobin_rw | hotshard_zipf12_g50 | 92 % | 169,094 / 169,097 | 0 | 0 | 0 / 0 | 30.1 % |
| spill_rw | hotshard_zipf12_g50 | 92 % | 162,368 / 172,696 | 58,821 | 0 | 0 / 0 | 31.2 % |
| migrate | hotshard_zipf12_g50 | 92 % | 205,776 / 33,169 | 0 | 3 | 0 / 0 | 0.0 % |
| migrate_hyst | hotshard_zipf12_g50 | 92 % | 205,668 / 31,747 | 0 | 3 | 0 / 0 | 0.0 % |
| hotrep_rw | hotshard_zipf12_g50 | 92 % | 169,439 / 88,332 | 0 | 0 | 26,795 / 22 | 0.0 % |

### Phase 1 designs: quiet vs WiFi-like load on core 0 (40 % duty)

| design | workload | quiet ops/s | wifisim ops/s | change | p99.9 core0 µs quiet → sim | p99.9 core1 µs quiet → sim | sim lock contended |
|---|---|---:|---:|---:|---:|---:|---:|
| 1c_nolock | g90_uniform | 290,353 | 283,316 | -2 % | – | 7.5 → 9.0 | 0.0 % |
| 2c_shared_cas | g90_uniform | 325,820 | 250,290 | -23 % | 17.9 → 836.1 | 17.7 → 17.8 | 74.7 % |
| 2c_sharded_cas | g90_uniform | 420,296 | 286,442 | -32 % | 16.0 → 848.1 | 17.3 → 833.7 | 30.1 % |
| 2c_sharded_rw | g90_uniform | 487,641 | 324,670 | -33 % | 14.4 → 849.9 | 13.2 → 17.3 | 6.5 % |
| 2c_sharded_seqlock | g90_uniform | 497,852 | 395,450 | -21 % | 13.8 → 836.9 | 12.5 → 16.1 | 3.6 % |
| 2c_owned_nolock | g90_uniform | 517,301 | 414,010 | -20 % | 14.5 → 830.5 | 12.7 → 16.0 | 0.0 % |
| 1c_nolock | g50_uniform | 290,878 | 284,055 | -2 % | – | 7.1 → 9.2 | 0.0 % |
| 2c_shared_cas | g50_uniform | 350,148 | 260,386 | -26 % | 17.1 → 839.9 | 17.2 → 17.2 | 77.4 % |
| 2c_sharded_cas | g50_uniform | 431,666 | 295,301 | -32 % | 15.1 → 835.1 | 15.8 → 16.6 | 30.9 % |
| 2c_sharded_rw | g50_uniform | 455,595 | 306,662 | -33 % | 15.3 → 840.7 | 14.2 → 16.8 | 23.3 % |
| 2c_sharded_seqlock | g50_uniform | 432,715 | 333,829 | -23 % | 20.1 → 831.8 | 20.0 → 22.0 | 18.1 % |
| 2c_owned_nolock | g50_uniform | 516,854 | 413,870 | -20 % | 14.1 → 829.5 | 12.7 → 12.7 | 0.0 % |
| ring_2w_routed | g90_uniform | 429,215 | 240,041 | -44 % | (queue) 1018 → 1901 | (queue) 1018 → 1901 | 0.0 % |
