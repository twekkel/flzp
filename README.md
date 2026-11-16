# Fast LZP compressor/preprocessor 

```
flzp (pre-)compressor

Usage: flzp [-cdfk] [-B BITS] [-H BITS] [-M MIN] [file ...]
  -c, --stdout       write to stdout, keep input files
  -d, --decompress   decompress
  -f, --force        overwrite output files, allow terminal I/O
  -h, --help         show this help
  -k, --keep         keep input files
  -B, --buffer BITS  history buffer 2^BITS bytes (10..30, default 22)
  -H, --hash BITS    hash table 2^BITS entries   ( 8..30, default 20)
  -M, --minmatch N   min match length coded as match (2..64, default 2)

With no FILE, or when FILE is -, read standard input.
```

## Benchmark

~1.4 GB log (10M lines from [supercomputer log](https://www.usenix.org/cfdr-data#hpc4)), single thread, x86-64 Linux build. Other tools shown for comparison
zpq rows at the bottom. Numbers are machine-dependent.

| Configuration                                 | c/d |  Wall(s) |  User(s) |   Sys(s) |   Memory(MB) |  Size(bytes) |   Ratio(%) |
|-----------------------------------------------|-----|----------|----------|----------|--------------|--------------|------------|
| lz4 -1                                        | c   |     2.29 |     2.43 |     0.52 |         31.9 |     74130923 |       5.23 |
| lz4 -1                                        | d   |     0.89 |     0.88 |     0.04 |         32.0 |            - |          - |
| flzp                                          | c   |     1.94 |     1.73 |     0.29 |          9.9 |     47047264 |       3.32 |
| flzp                                          | d   |     1.13 |     1.13 |     0.01 |          9.5 |            - |          - |
| zstd -3                                       | c   |     0.77 |     0.83 |     0.35 |         39.0 |     45472775 |       3.21 |
| zstd -3                                       | d   |     0.32 |     0.32 |     0.01 |          7.5 |            - |          - |
| gzip -6                                       | c   |     3.76 |     3.67 |     0.33 |          7.4 |     40599414 |       2.87 |
| gzip -6                                       | d   |     0.43 |     0.42 |     0.01 |          7.5 |            - |          - |
| zstd -11                                      | c   |     6.34 |     6.41 |     0.40 |         90.9 |     30219009 |       2.13 |
| zstd -11                                      | d   |     0.22 |     0.24 |     0.00 |          8.4 |            - |          - |
| flzp + gzip                                   | c   |     2.33 |     3.45 |     0.29 |          9.8 |     16002356 |       1.13 |
| flzp + gzip                                   | d   |     1.18 |     1.32 |     0.00 |          9.6 |            - |          - |
| kanzi -c -b64M -j 1 -t lzpx -e tpaq           | c   |    12.30 |    11.23 |     1.19 |        109.7 |      9376559 |       0.66 |
| kanzi -c -b64M -j 1 -t lzpx -e tpaq           | d   |    11.93 |    11.17 |     0.74 |        111.5 |            - |          - |
| kanzi -c -b64M -j 1 -t lzpx -e tpaqx          | c   |    18.52 |    14.90 |     3.72 |        359.4 |      9203874 |       0.65 |
| kanzi -c -b64M -j 1 -t lzpx -e tpaqx          | d   |    17.75 |    14.49 |     3.21 |        360.9 |            - |          - |
| flzp + zpq                                    | c   |    24.43 |    24.18 |     0.32 |        361.0 |      8474774 |       0.60 |
| flzp + zpq                                    | d   |    23.85 |    25.54 |     0.04 |        314.5 |            - |          - |
| flzp -B28 + zpq -z511                         | c   |    56.80 |    56.47 |     0.37 |        814.6 |      7897042 |       0.56 |
| flzp -B28 + zpq -z511                         | d   |    56.30 |    58.03 |     0.11 |        768.6 |            - |          - |

Based on:
 * flzp.cpp  - (C) 2008, Matt Mahoney
 * flzp2.cpp - (C) 2008-2026, Matt Mahoney, xezz.

see discussion at https://encode.su/threads/95-flzp-new-LZP-compressor-preprocessor
