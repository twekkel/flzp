# Fast LZP compressor/preprocessor 

flzp (pre-)compressor

Usage: flzp [-cdfk] [-B BITS] [-H BITS] [file ...]
  -c, --stdout       write to stdout, keep input files
  -d, --decompress   decompress
  -f, --force        overwrite output files, allow terminal I/O
  -h, --help         show this help
  -k, --keep         keep input files
  -B, --buffer BITS  history buffer 2^BITS bytes (10..30, default 22)
  -H, --hash BITS    hash table 2^BITS entries   ( 8..30, default 20)

With no FILE, or when FILE is -, read standard input.


Based on:
 * flzp.cpp  - (C) 2008, Matt Mahoney
 * flzp2.cpp - (C) 2008-2026, Matt Mahoney, xezz.

see discussion at https://encode.su/threads/95-flzp-new-LZP-compressor-preprocessor
