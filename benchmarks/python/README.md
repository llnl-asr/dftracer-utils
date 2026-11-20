# Python micro benchmarks

Standard library only. Run from an environment that holds the extension, pandas
and a C++ compiler: `python benchmarks/python/micro_bench.py --rounds 5`
(medians of alternating rounds, in milliseconds; `--case NAME` runs one case).
