/* flzp file compressor - plain C port
 * based on:
 * flzp.cpp  - (C) 2008, Matt Mahoney
 * flzp2.cpp - (C) 2008-2026, Matt Mahoney, xezz.
 *
 * Vibe/Mistral improvements 2026, Stream format v2 (not backwards compatible):
 *   header: 'F', version 2, bbits, hbits, minmatch (5 bytes)
 *   then per block: 32-byte literal bitmap (bit set = literal byte),
 *   tokens, EOB. Absent bytes, ascending: first = EOB, rest = match
 *   lengths minmatch..maxlen; length >= maxlen is top code + varint
 *   excess. A literal is the byte itself; a match copies len bytes from
 *   ht[hash of last 4 bytes] in the ring buffer.
 */
#define _POSIX_C_SOURCE 200809L  /* isatty/access/unlink under -std=c99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#define NDEBUG
#include <assert.h>

#define DEF_BBITS 22
#define DEF_HBITS 20
#define DEF_MMATCH 2
#define MIN_ABSENT 16   /* min absent-byte codes reserved per block (LZPX) */
#define SUFFIX ".flzp"

static unsigned BN, HN;  /* buffer size, hash table size (powers of 2) */
static const char *prog = "flzp";

/* Buffered stdin, bypassing per-byte stdio locking */
static unsigned char ibuf[1 << 16];
static size_t ipos = 0, ilen = 0;

static inline int get(void) {
  if (ipos == ilen) {
    ilen = fread(ibuf, 1, sizeof ibuf, stdin);
    ipos = 0;
    if (ilen == 0) return EOF;
  }
  return ibuf[ipos++];
}

/* Compressor output: header (5) + one block is at most 32 (header)
 * + 262144 (tokens) + 1 (EOB) bytes, so obuf never overflows if flushed
 * after each block. A long match costs at most 4 bytes for >= 33 input
 * bytes, so this bound still holds. */
static unsigned char obuf[(1 << 18) + 64];
static size_t opos = 0;

static inline void put(int c) {
  obuf[opos++] = (unsigned char)c;
}

static inline void put_block(const unsigned char *p, size_t n) {
  memcpy(obuf + opos, p, n);
  opos += n;
}

static inline void flush_out(void) {
  if (opos) {
    fwrite(obuf, 1, opos, stdout);
    opos = 0;
  }
}

/* Allocate n zeroed elements of size sz, or exit */
static void *alloc(size_t n, size_t sz) {
  void *p = calloc(n, sz);
  if (!p) {
    fprintf(stderr, "Out of memory (try smaller -B / -H)\n");
    exit(1);
  }
  return p;
}

/* Input buffer for finding matches */
typedef struct {
  unsigned char *buf;  /* history buffer (BN bytes) */
  unsigned int *ht;    /* hash table: h -> matched context (HN entries) */
  int *enc;            /* encoding table: -1=literal, 0=EOB, 1..maxlen=match */
  unsigned int h;      /* context hash */
  unsigned int p;      /* number of bytes in buf (wraps mod 2^32) */
  int maxlen;          /* highest match code (start of long-match range) */
} Buf;

static void buf_init(Buf *b) {
  b->buf = (unsigned char *)alloc(BN, sizeof(unsigned char));
  b->ht  = (unsigned int *)alloc(HN, sizeof(unsigned int));
  b->enc = (int *)alloc(256, sizeof(int));
  b->h = 0;
  b->p = 0;
  b->maxlen = 0;
}

static void buf_free(Buf *b) {
  free(b->buf);
  free(b->ht);
  free(b->enc);
}

static inline void buf_update_hash(Buf *b, int c) {
  /* mask only at the ht index: the low HN bits are identical either way */
  b->h = b->h * 96u + (unsigned)c;
}

static inline void buf_update(Buf *b, int c) {
  b->ht[b->h & (HN - 1)] = b->p;
  buf_update_hash(b, c);
  b->buf[b->p++ & (BN - 1)] = (unsigned char)c;
}

/* Decompressor: update and write buf directly to stdout when full */
static inline void buf_update_out(Buf *b, int c) {
  buf_update(b, c);
  if (!(b->p & (BN - 1)))
    fwrite(b->buf, 1, BN, stdout);
}

static void buf_flush(Buf *b) {
  if (b->p & (BN - 1))
    fwrite(b->buf, 1, (size_t)(b->p & (BN - 1)), stdout);
}

/* Emit a match of len >= minmatch bytes: len < maxlen a single code,
 * longer matches the top code + varint excess */
static inline void output_match(Buf *b, int len, int minmatch) {
  if (len < b->maxlen)
    put(b->enc[len - minmatch + 1]);
  else {
    unsigned n = (unsigned)(len - b->maxlen);
    put(b->enc[b->maxlen - minmatch + 1]);
    for (; n > 127; n >>= 7) {
      n -= 128;
      put((int)((n & 127) | 128));
    }
    put((int)n);
  }
}

/* Match length of src[0..limit) against history at absolute position m.
 * History byte at m+k is buf[(m+k)&(BN-1)] for m+k < b->p (ring), and for
 * overlapping matches (k >= d, d = p-m) it is src[k-d] already in blk.
 * Compared 8 bytes at a time, split at the ring wrap. */
static size_t match_extend(const Buf *b, const unsigned char *src,
                           size_t limit, unsigned int m) {
  unsigned int d = b->p - m;
  size_t k = 0;
  size_t lim1 = d < limit ? (size_t)d : limit;
  while (k < lim1) {
    unsigned int pos = (m + (unsigned int)k) & (BN - 1);
    size_t seg = BN - pos;
    if (seg > lim1 - k) seg = lim1 - k;
    const unsigned char *q = b->buf + pos;
    size_t j = 0;
    while (j + 8 <= seg) {
      uint64_t x, y;
      memcpy(&x, src + k + j, 8);
      memcpy(&y, q + j, 8);
      if (x != y)
        return k + j + ((size_t)__builtin_ctzll(x ^ y) >> 3);
      j += 8;
    }
    while (j < seg && src[k + j] == q[j]) ++j;
    k += j;
    if (j < seg) return k;
  }
  while (k < limit) {
    size_t j = 0;
    while (k + j + 8 <= limit) {
      uint64_t x, y;
      memcpy(&x, src + k + j, 8);
      memcpy(&y, src + k + j - d, 8);
      if (x != y)
        return k + j + ((size_t)__builtin_ctzll(x ^ y) >> 3);
      j += 8;
    }
    while (k + j < limit && src[k + j] == src[k + j - d]) ++j;
    k += j;
    if (k < limit) return k;
  }
  return k;
}

static void compress(int bbits, int hbits, int minmatch) {
  Buf b;
  static unsigned char blk[1 << 18];  /* current block */

  BN = 1u << bbits;
  HN = 1u << hbits;
  buf_init(&b);
  ipos = ilen = 0;

  put(0x46);  /* 'F' magic */
  put(2);     /* stream format version */
  put(bbits);
  put(hbits);
  put(minmatch);

  /* Pass 1 reads straight into blk with fread; bytes past the block end
   * are carried over to the next block (have = leftover count). */
  size_t have = 0;
  while (1) {
    size_t n = fread(blk + have, 1, (1 << 18) - have, stdin);
    size_t avail = have + n;
    if (avail == 0) break;
    unsigned char seen[256] = {0};
    size_t blocksize, limit = avail < (1 << 18) ? avail : (1 << 18);
    int distinct = 0;
    for (blocksize = 0; blocksize < limit; ++blocksize) {
      unsigned c = blk[blocksize];
      if (!seen[c]) { seen[c] = 1; ++distinct; }
      if (distinct > 256 - MIN_ABSENT) { ++blocksize; break; }
    }
    b.maxlen = minmatch + (256 - distinct) - 2;  /* top match length code */
    if (blocksize == 0) break;

    /* Header and encoding table */
    unsigned char dec[32] = {0};
    int j = 0;
    for (int i = 0; i < 256; ++i) {
      if (seen[i]) dec[i >> 3] |= (unsigned char)(1 << (i & 7));
      else b.enc[j++] = i;
    }
    assert(j == b.maxlen - minmatch + 2);

    /* Pass 2: encode the block from memory; matches are extended word-wise
     * from blk since the whole block is available */
    put_block(dec, 32);
    size_t i = 0;
    while (i < blocksize) {
      int c = blk[i];
      unsigned int m = b.ht[b.h & (HN - 1)];
      if (b.p == m || b.buf[m & (BN - 1)] != (unsigned char)c) {
        put(c);
        buf_update(&b, c);
        ++i;
        continue;
      }
      size_t room = blocksize - i;
      if (room > (size_t)BN) room = BN;   /* match length cap, as before */
      int len = (int)match_extend(&b, blk + i, room, m);
      if (len < minmatch) {
        for (int k = 0; k < len; ++k)
          put(blk[i + k]);
      } else
        output_match(&b, len, minmatch);
      for (int k = 0; k < len; ++k)
        buf_update(&b, blk[i + k]);
      i += (size_t)len;
    }
    put(b.enc[0]);  /* EOB */
    flush_out();    /* one write per block */
    /* carry bytes past the block end into the next iteration, but only
     * after pass 2 has consumed blk[0..blocksize) */
    have = avail - blocksize;
    if (have) memmove(blk, blk + blocksize, have);
  }
  flush_out();      /* header-only output for empty input */
  buf_free(&b);
}

static int decompress(void) {
  Buf b;
  enum { HEADER, DATA } state = HEADER;
  int dec[256] = {0};
  int maxlen = 0;  /* highest match code in the current block */

  ipos = ilen = 0;
  int magic = get();
  if (magic == EOF) return 0;  /* empty input -> empty output */
  int ver = get();
  int bbits = get();
  int hbits = get();
  int minmatch = get();
  if (magic != 0x46 || ver != 2 || bbits < 10 || bbits > 30 ||
      hbits < 8 || hbits > 30 || minmatch < 2 || minmatch > 64) {
    fprintf(stderr, "%s: not a flzp stream (bad header)\n", prog);
    return 1;
  }
  BN = 1u << bbits;
  HN = 1u << hbits;
  buf_init(&b);

  while (1) {
    if (state == HEADER) {
      int k = 0;
      for (int i = 0; i < 32; ++i) {
        int c = get();
        for (int j = 0; j < 8; ++j) {
          if (c & (1 << j))
            dec[i * 8 + j] = -1;
          else
            dec[i * 8 + j] = (k++ == 0) ? 0 : minmatch + k - 2;
        }
      }
      maxlen = minmatch + k - 2;
      state = DATA;
    }
    else {
      int c = get();
      if (c == EOF) break;
      else {
        int d = dec[c];
        if (d == 0)
          state = HEADER;
        else if (d < 0)
          buf_update_out(&b, c);
        else {
          long long n = d;
          if (d == maxlen) {      /* long match: read varint excess */
            int s = 0, e;
            do {
              e = get();
              if (e == EOF || s > 28) {
                fprintf(stderr, "%s: corrupt stream\n", prog);
                buf_free(&b);
                return 1;
              }
              n += (long long)e << s;
              s += 7;
            } while (e > 127);
            if (n > (long long)BN) {
              fprintf(stderr, "%s: corrupt stream (match too long)\n", prog);
              buf_free(&b);
              return 1;
            }
          }
          unsigned match = b.ht[b.h & (HN - 1)];
          for (long long i = 0; i < n; ++i) {
            c = b.buf[(match + (unsigned)i) & (BN - 1)];
            buf_update_out(&b, c);
          }
        }
      }
    }
  }
  buf_flush(&b);
  buf_free(&b);
  return 0;
}

static void usage(int status) {
  fprintf(status ? stderr : stdout,
    "flzp (pre-)compressor\n"
    "(C) 2008, Matt Mahoney. Free software under GPL v3.\n"
    "\n"
    "Usage: %s [-cdfk] [-B BITS] [-H BITS] [-m MIN] [file ...]\n"
    "  -c, --stdout       write to stdout, keep input files\n"
    "  -d, --decompress   decompress\n"
    "  -f, --force        overwrite output files, allow terminal I/O\n"
    "  -h, --help         show this help\n"
    "  -k, --keep         keep input files\n"
    "  -B, --buffer BITS  history buffer 2^BITS bytes (10..30, default %d)\n"
    "  -H, --hash BITS    hash table 2^BITS entries   ( 8..30, default %d)\n"
    "  -m, --minmatch N   min match length coded as match (2..64, default %d)\n"
    "\n"
    "With no FILE, or when FILE is -, read standard input.\n",
    prog, DEF_BBITS, DEF_HBITS, DEF_MMATCH);
  exit(status);
}

static int parse_bits(const char *s, int lo, int hi, char name) {
  char *end;
  long v = strtol(s, &end, 10);
  if (*s == 0 || *end || v < lo || v > hi) {
    fprintf(stderr, "%s: bad value for -%c (must be %d..%d)\n",
            prog, name, lo, hi);
    exit(1);
  }
  return (int)v;
}

static int saved_in = -1, saved_out = -1;  /* the original stdin/stdout */

/* Run one job. Files are attached to fd 0/1 with dup2 (not freopen) so the
 * original stdin/stdout can be restored for later "-" arguments.
 * Returns 0 ok, 1 failed after output was opened (caller removes a partial
 * output file), 2 failed before anything was written. */
static int process(const char *in, const char *out, int decomp,
                   int bbits, int hbits, int minmatch) {
  int fi = -1, fo = -1, rc = 0;
  if (in && (fi = open(in, O_RDONLY)) < 0) { perror(in); return 2; }
  if (out && (fo = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    perror(out);
    if (fi >= 0) close(fi);
    return 2;
  }
  if (fi >= 0) { dup2(fi, STDIN_FILENO);  close(fi); clearerr(stdin); }
  if (fo >= 0) { fflush(stdout); dup2(fo, STDOUT_FILENO); close(fo); }

  if (decomp) rc = decompress();
  else        compress(bbits, hbits, minmatch);
  if (fflush(stdout) || ferror(stdout)) {
    fprintf(stderr, "%s: write error\n", prog);
    rc = 1;
  }

  if (fi >= 0 && saved_in  >= 0) { dup2(saved_in,  STDIN_FILENO);  clearerr(stdin); }
  if (fo >= 0 && saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); clearerr(stdout); }
  return rc;
}

/* Like gzip: refuse binary data on a terminal unless -f. Returns 1 if refused. */
static int tty_refused(int decomp) {
  if (decomp ? isatty(STDIN_FILENO) : isatty(STDOUT_FILENO)) {
    fprintf(stderr, "%s: compressed data not %s a terminal. "
                    "Use -f to force %scompression.\n",
            prog, decomp ? "read from" : "written to", decomp ? "de" : "");
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  int decomp = 0, tostdout = 0, keep = 0, force = 0;
  int bbits = DEF_BBITS, hbits = DEF_HBITS, minmatch = DEF_MMATCH;
  static const struct option lopts[] = {
    {"decompress", no_argument,       0, 'd'},
    {"uncompress", no_argument,       0, 'd'},
    {"stdout",     no_argument,       0, 'c'},
    {"to-stdout",  no_argument,       0, 'c'},
    {"keep",       no_argument,       0, 'k'},
    {"force",      no_argument,       0, 'f'},
    {"buffer",     required_argument, 0, 'B'},
    {"hash",       required_argument, 0, 'H'},
    {"minmatch",   required_argument, 0, 'm'},
    {"help",       no_argument,       0, 'h'},
    {0, 0, 0, 0}
  };

  int o;
  while ((o = getopt_long(argc, argv, "dckfB:H:m:h", lopts, NULL)) != -1) {
    switch (o) {
      case 'd': decomp = 1;   break;
      case 'c': tostdout = 1; break;
      case 'k': keep = 1;     break;
      case 'f': force = 1;    break;
      case 'B': bbits = parse_bits(optarg, 10, 30, 'B'); break;
      case 'H': hbits = parse_bits(optarg,  8, 30, 'H'); break;
      case 'm': minmatch = parse_bits(optarg, 2, 64, 'm'); break;
      case 'h': usage(0); break;
      default:  usage(1);
    }
  }

  if (!force) {
    if (optind == argc && tty_refused(decomp)) return 1;
    if (optind < argc && tostdout && !decomp && tty_refused(0)) return 1;
  }

  saved_in  = dup(STDIN_FILENO);   /* for restoring after file jobs */
  saved_out = dup(STDOUT_FILENO);

  if (optind == argc)   /* no files: stdin -> stdout */
    return process(NULL, NULL, decomp, bbits, hbits, minmatch) != 0;

  const size_t sl = strlen(SUFFIX);
  int status = 0;
  for (int i = optind; i < argc; ++i) {
    const char *in = argv[i];
    size_t n = strlen(in);

    if (!strcmp(in, "-")) {            /* "-" means stdin -> stdout */
      if (!force && tty_refused(decomp)) { status = 1; continue; }
      if (process(NULL, NULL, decomp, bbits, hbits, minmatch)) status = 1;
      continue;
    }
    char *out = NULL;
    int has_suffix = n > sl && !strcmp(in + n - sl, SUFFIX);

    if (!tostdout) {
      if (decomp) {
        if (!has_suffix) {
          fprintf(stderr, "%s: %s: unknown suffix -- ignored\n", prog, in);
          status = 1;
          continue;
        }
        out = (char *)malloc(n - sl + 1);
        if (!out) { fprintf(stderr, "%s: out of memory\n", prog); return 1; }
        memcpy(out, in, n - sl);
        out[n - sl] = 0;
      } else {
        if (has_suffix) {
          fprintf(stderr, "%s: %s already has %s suffix -- unchanged\n",
                  prog, in, SUFFIX);
          status = 1;
          continue;
        }
        out = (char *)malloc(n + sl + 1);
        if (!out) { fprintf(stderr, "%s: out of memory\n", prog); return 1; }
        memcpy(out, in, n);
        memcpy(out + n, SUFFIX, sl + 1);
      }
      if (!force && access(out, F_OK) == 0) {
        fprintf(stderr, "%s: %s already exists; not overwritten (use -f)\n",
                prog, out);
        status = 1;
        free(out);
        continue;
      }
    }

    int rc = process(in, out, decomp, bbits, hbits, minmatch);
    if (out) {
      if (rc == 1)        unlink(out);        /* no partial output */
      else if (!rc && !keep) unlink(in);      /* success: drop the input */
      free(out);
    }
    if (rc) status = 1;
  }
  return status;
}
