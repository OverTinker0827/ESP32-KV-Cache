
### No network, g90_uniform (5 runs x 3 s each)

| design | ops/s mean | std dev | min - max | vs single core | p50 / p99 / p99.9 µs | CPU core0 / core1 |
|---|---:|---:|---:|---:|---:|---:|
| single_core | 299,774 | 57 (0.0 %) | 299,694 - 299,846 | 1.00× | 3.18 / 3.60 / 7.21 | 0 % / 100 % |
| shared_seqlock | 447,320 | 1,447 (0.3 %) | 445,512 - 449,211 | 1.49× | 3.91 / 11.02 / 15.68 | 100 % / 100 % |
| shared_rw | 440,022 | 1,626 (0.4 %) | 438,854 - 442,876 | 1.47× | 4.05 / 6.85 / 14.74 | 100 % / 100 % |
| sharded8_seqlock | 477,230 | 342 (0.1 %) | 476,881 - 477,658 | 1.59× | 3.94 / 8.19 / 13.79 | 100 % / 100 % |
| sharded8_rw | 471,926 | 192 (0.0 %) | 471,722 - 472,209 | 1.57× | 4.04 / 6.15 / 13.73 | 100 % / 100 % |
| owner | 543,553 | 383 (0.1 %) | 542,987 - 544,012 | 1.81× | 3.50 / 4.01 / 13.29 | 100 % / 100 % |
| dispatch_1worker | 275,761 | 84 (0.0 %) | 275,713 - 275,908 | 0.92× | – | 100 % / 100 % |
| dispatch_2owners | 488,448 | 4,512 (0.9 %) | 481,590 - 493,558 | 1.63× | – | 100 % / 100 % |
| dispatch_pool | 377,183 | 1,240 (0.3 %) | 376,293 - 379,055 | 1.26× | – | 100 % / 97 % |
| microredis | 6,910 | 33 (0.5 %) | 6,894 - 6,969 | 0.02× | – | one core (MicroPython) |

### No network, g50_uniform (5 runs x 3 s each)

| design | ops/s mean | std dev | min - max | vs single core | p50 / p99 / p99.9 µs | CPU core0 / core1 |
|---|---:|---:|---:|---:|---:|---:|
| single_core | 296,909 | 74 (0.0 %) | 296,798 - 296,986 | 1.00× | 3.26 / 3.63 / 7.12 | 0 % / 100 % |
| shared_seqlock | 332,185 | 1,029 (0.3 %) | 330,850 - 333,375 | 1.12× | 4.32 / 25.15 / 42.21 | 100 % / 100 % |
| shared_rw | 345,044 | 1,025 (0.3 %) | 343,930 - 346,325 | 1.16× | 6.28 / 7.17 / 17.14 | 100 % / 100 % |
| sharded8_seqlock | 447,679 | 674 (0.2 %) | 446,993 - 448,609 | 1.51× | 4.20 / 9.10 / 14.57 | 100 % / 100 % |
| sharded8_rw | 462,213 | 94 (0.0 %) | 462,062 - 462,297 | 1.56× | 4.04 / 6.54 / 14.18 | 100 % / 100 % |
| owner | 536,184 | 160 (0.0 %) | 536,014 - 536,416 | 1.81× | 3.57 / 4.01 / 13.36 | 100 % / 100 % |
| dispatch_1worker | 273,083 | 14 (0.0 %) | 273,068 - 273,093 | 0.92× | – | 100 % / 100 % |
| dispatch_2owners | 487,117 | 4,474 (0.9 %) | 481,870 - 491,592 | 1.64× | – | 100 % / 100 % |
| dispatch_pool | 301,763 | 1,238 (0.4 %) | 300,597 - 303,438 | 1.02× | – | 100 % / 96 % |
| microredis | 4,232 | 60 (1.4 %) | 4,151 - 4,276 | 0.01× | – | one core (MicroPython) |

### No network, correctness (one checked 3 s run per design and workload)

| design | workload | GETs checked | SETs | torn | unpublished version | went backwards | misses | final state wrong | verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| single_core | g90_uniform | 421,740 | 49,731 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_seqlock | g90_uniform | 806,912 | 41,756 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_rw | g90_uniform | 791,729 | 41,392 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_seqlock | g90_uniform | 816,362 | 43,790 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_rw | g90_uniform | 808,309 | 38,663 | 0 | 0 | 0 | 0 | 0 | PASS |
| owner | g90_uniform | 822,489 | 88,887 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_1worker | g90_uniform | 409,477 | 44,930 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_2owners | g90_uniform | 766,044 | 88,917 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_pool | g90_uniform | 684,443 | 71,605 | 0 | 0 | 0 | 0 | 0 | PASS |
| single_core | g50_uniform | 236,875 | 237,077 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_seqlock | g50_uniform | 597,233 | 194,715 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_rw | g50_uniform | 613,067 | 200,274 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_seqlock | g50_uniform | 633,362 | 208,020 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_rw | g50_uniform | 619,495 | 216,159 | 0 | 0 | 0 | 0 | 0 | PASS |
| owner | g50_uniform | 452,876 | 454,161 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_1worker | g50_uniform | 222,098 | 228,725 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_2owners | g50_uniform | 435,862 | 424,911 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_pool | g50_uniform | 353,395 | 344,573 | 0 | 0 | 0 | 0 | 0 | PASS |
| microredis (512 keys) | g90_uniform | 4,522 | 541 | – | – | – | 0 wrong | 0 | PASS |
| microredis (512 keys) | g50_uniform | 2,632 | 2,576 | – | – | – | 0 wrong | 0 | PASS |
