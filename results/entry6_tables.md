
### Capacity: max keys (16 B keys `key:%012d`), store bytes per key in brackets

| engine | 8 B | 16 B | 32 B | 64 B | 128 B | 256 B | 512 B | 1024 B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| basic | 5,145 (54) | 4,406 (56) | 3,416 (72) | 2,367 (104) | 1,431 (173) | 798 (309) | 425 (581) | 225 (1100) |
| opt0 | 5,131 (48) | 5,131 (48) | 3,942 (62) | 2,268 (108) | 1,350 (181) | 648 (376) | 324 (753) | 162 (1506) |
| opt1 | 5,075 (47) | 5,075 (47) | 3,869 (62) | 2,226 (108) | 1,325 (181) | 636 (377) | 318 (754) | 159 (1507) |
| opt2 | 5,447 (44) | 5,447 (44) | 4,056 (58) | 2,392 (99) | 1,300 (182) | 728 (326) | 364 (652) | 156 (1520) |
| opt2 + prefix | 5,447 (44) | 5,447 (44) | 4,056 (58) | 2,392 (99) | 1,300 (182) | 728 (326) | 364 (652) | 156 (1520) |

### Capacity: max keys (24 B keys `sensor:temp:roomNN:NNNNN`), store bytes per key in brackets

| engine | 8 B | 16 B | 32 B | 64 B | 128 B | 256 B | 512 B | 1024 B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| basic | 4,406 (56) | 3,856 (64) | 3,076 (80) | 2,199 (112) | 1,371 (180) | 798 (309) | 425 (581) | 225 (1100) |
| opt0 | 5,131 (48) | 4,590 (53) | 3,402 (72) | 2,268 (108) | 1,350 (181) | 648 (376) | 324 (753) | 162 (1506) |
| opt1 | 5,075 (47) | 4,505 (53) | 3,339 (72) | 2,226 (108) | 1,325 (181) | 636 (377) | 318 (754) | 159 (1507) |
| opt2 | 5,447 (44) | 4,784 (50) | 3,536 (67) | 2,184 (109) | 1,300 (182) | 728 (326) | 364 (652) | 156 (1520) |
| opt2 + prefix | 5,447 (44) | 5,447 (44) | 4,784 (50) | 2,652 (89) | 1,456 (163) | 780 (304) | 364 (652) | 156 (1520) |

### Mixed value sizes (16-512 B, IoT keys): keys stored, payload / store bytes

| engine | keys stored | rejected SETs | store KB | payload efficiency |
|---|---:|---:|---:|---:|
| basic | 766 | 0 | 241 | 0.90 |
| opt0 | 856 | 3441 | 238 | 0.75 |
| opt1 | 844 | 3453 | 234 | 0.75 |
| opt2 | 1,189 | 8470 | 232 | 0.82 |
| opt2 + prefix | 1,645 | 12394 | 232 | 0.94 |

### Speed regression check (Entry 2 benchmark, same firmware): ops/s, GET p50/p99 µs

| engine | scenario | SET/s | GET/s | DEL/s | GET p50 / p99 µs |
|---|---|---:|---:|---:|---:|
| basic-malloc-chained | quiet | 80,776 | 76,885 | 99,714 | 12.82 / 16.88 |
| basic-malloc-chained | sim_other_core | 78,361 | 75,378 | 96,596 | 12.82 / 18.49 |
| basic-malloc-chained | sim_same_core | 46,456 | 41,026 | 56,533 | 12.82 / 839.11 |
| opt-slab-oa | quiet | 434,272 | 357,283 | 389,468 | 2.73 / 3.11 |
| opt-slab-oa | sim_other_core | 417,466 | 349,662 | 384,985 | 2.73 / 3.29 |
| opt-slab-oa | sim_same_core | 251,155 | 200,936 | 231,642 | 2.73 / 6.46 |
| opt1-slab-oa-1sh-nolock | quiet | 380,474 | 324,991 | 366,743 | 3.01 / 3.33 |
| opt1-slab-oa-1sh-nolock | sim_other_core | 369,030 | 317,874 | 361,232 | 3.01 / 3.65 |
| opt1-slab-oa-1sh-nolock | sim_same_core | 223,244 | 184,199 | 219,058 | 3.01 / 6.84 |
| opt2-slab-oa-1sh-nolock | quiet | 404,350 | 338,329 | 359,544 | 2.89 / 3.33 |
| opt2-slab-oa-1sh-nolock | sim_other_core | 392,110 | 330,983 | 354,496 | 2.89 / 3.46 |
| opt2-slab-oa-1sh-nolock | sim_same_core | 225,942 | 197,640 | 204,641 | 2.89 / 6.72 |

### Adaptive reserve under packet-buffer bursts (v2 engine, 6 s each)

| mode | buffer allocs failed / attempted | cache hit rate | arena pages avg (min-max) | pages released / regrown | lowest free heap |
|---|---:|---:|---:|---:|---:|
| fixed_40k | 1,215 / 1,296 | 0.672 | 52.0 (52-52) | 0 / 0 | 4.7 KB |
| fixed_80k | 0 / 176 | 0.555 | 43.0 (43-43) | 0 / 0 | 15.0 KB |
| fixed_100k | 0 / 176 | 0.491 | 38.0 (38-38) | 0 / 0 | 37.0 KB |
| adaptive_20k_48k | 28 / 204 | 0.591 | 45.6 (40-52) | 40 / 28 | 10.1 KB |
| adaptive_40k_64k | 21 / 197 | 0.536 | 41.4 (35-51) | 49 / 32 | 30.1 KB |
| adaptive_56k_80k | 19 / 195 | 0.484 | 37.4 (31-47) | 53 / 32 | 55.0 KB |
