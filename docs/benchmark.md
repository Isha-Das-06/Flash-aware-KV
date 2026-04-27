# Benchmark results

Output of `make bench` (GCC 16, -O2). See the README for how to read it.

64 blocks x 4096 B, 400000 updates of 100-byte values per run; lifetime assumes 100k erase cycles per block

| workload | fill | policy | WA | erase min / mean / max | evenness | lifetime | GC runs | WL moves |
|---|---|---|---|---|---|---|---|---|
| uniform | 50% | greedy, no static WL | 1.23 | 229 / 231 / 233 | 99% | 20.7 GB | 14731 | 0 |
| uniform | 50% | greedy + static WL | 1.23 | 229 / 231 / 233 | 99% | 20.7 GB | 14731 | 0 |
| uniform | 50% | cost-benefit + static WL | 1.24 | 230 / 233 / 236 | 99% | 20.4 GB | 14843 | 0 |
| hot/cold 90/10 | 50% | greedy, no static WL | 1.65 | 299 / 310 / 319 | 97% | 15.1 GB | 19771 | 0 |
| hot/cold 90/10 | 50% | greedy + static WL | 1.65 | 301 / 310 / 316 | 98% | 15.2 GB | 19725 | 51 |
| hot/cold 90/10 | 50% | cost-benefit + static WL | 1.69 | 309 / 316 / 322 | 98% | 14.9 GB | 20164 | 0 |
| hot/cold 99/1 | 50% | greedy, no static WL | 1.77 | 299 / 332 / 376 | 88% | 12.8 GB | 21162 | 0 |
| hot/cold 99/1 | 50% | greedy + static WL | 1.81 | 331 / 339 / 347 | 98% | 13.9 GB | 17470 | 4174 |
| hot/cold 99/1 | 50% | cost-benefit + static WL | 1.85 | 339 / 347 / 355 | 98% | 13.6 GB | 21828 | 286 |
| static + hot | 50% | greedy, no static WL | 1.01 | 1 / 190 / 368 | 52% | 13.1 GB | 12090 | 0 |
| static + hot | 50% | greedy + static WL | 1.05 | 185 / 197 / 201 | 98% | 23.9 GB | 12082 | 440 |
| static + hot | 50% | cost-benefit + static WL | 1.06 | 194 / 199 / 210 | 95% | 22.9 GB | 12151 | 497 |
| uniform | 80% | greedy, no static WL | 2.44 | 452 / 457 / 460 | 99% | 10.5 GB | 29178 | 0 |
| uniform | 80% | greedy + static WL | 2.44 | 452 / 457 / 460 | 99% | 10.5 GB | 29178 | 0 |
| uniform | 80% | cost-benefit + static WL | 2.48 | 459 / 465 / 472 | 98% | 10.2 GB | 29680 | 0 |
| hot/cold 90/10 | 80% | greedy, no static WL | 3.24 | 597 / 607 / 617 | 98% | 7.8 GB | 38800 | 0 |
| hot/cold 90/10 | 80% | greedy + static WL | 3.24 | 600 / 608 / 615 | 99% | 7.8 GB | 38539 | 297 |
| hot/cold 90/10 | 80% | cost-benefit + static WL | 3.24 | 598 / 606 / 613 | 99% | 7.9 GB | 38571 | 161 |
| hot/cold 99/1 | 80% | greedy, no static WL | 3.84 | 454 / 720 / 769 | 94% | 6.3 GB | 45996 | 0 |
| hot/cold 99/1 | 80% | greedy + static WL | 3.95 | 730 / 739 / 746 | 99% | 6.5 GB | 38497 | 8746 |
| hot/cold 99/1 | 80% | cost-benefit + static WL | 4.09 | 758 / 766 / 773 | 99% | 6.2 GB | 47947 | 1043 |
| static + hot | 80% | greedy, no static WL | 1.01 | 1 / 190 / 866 | 22% | 5.6 GB | 12108 | 0 |
| static + hot | 80% | greedy + static WL | 1.09 | 197 / 204 / 213 | 96% | 22.6 GB | 12084 | 922 |
| static + hot | 80% | cost-benefit + static WL | 1.10 | 199 / 206 / 215 | 96% | 22.4 GB | 12267 | 862 |
