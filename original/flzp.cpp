/*  flzp file compressor (C) 2008, Matt Mahoney.

    LICENSE

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 3 of
    the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details at
    Visit <http://www.gnu.org/copyleft/gpl.html>.

To compress:   flzp c input output
To decompress: flzp d input output

flzp is a fast, byte oriented LZP compressor. A byte is decoded
either as a literal, match length, or end of block symbol.
A match length is decoded to the bytes that follow the last
context match within a sliding window.  Uses 8 MB memory.

flzp can be used as a preprocessor to a low-order compressor
to improve both compression ratio and speed.  The following
demonstrates flzp used as a preprocessor to improve compression
for fpaq0 (a stationary 0-order arithmetic coder), and ppmd -o2
and -o3 (PPM orders 2 and 3, flzp hurts compression for higher
orders).  enwik8 is a 100 MB text file.

57,366,279 enwik8.flzp          8 sec
63,391,013 enwik8.fpaq0         36 sec
39,879,072 enwik8.flzp.fpaq0    8+21 sec
36,800,798 enwik8.ppmd-o2
30,884,687 enwik8.flzp.ppmd-o2
30,017,979 enwik8.ppmd-o3
29,372,279 enwik8.flzp.ppmd-o3

A compressed flzp file is decoded as follows. The decoder maintains
the following data structures:

- A rotating buffer BUF[0..BN-1] of length BN bytes, initially all 0.
    An index BUF[i] is understood to mean BUF[i mod BN].
- The number of output bytes, P, initially 0.  The most recent output
    byte is BUF[P - 1].
- A context hash H of the last L bytes output (range 0..BN-1)
    initially 0.
- A hash table HT[0..HN-1] containing HN pointers into the buffer
    (range 0..BN-1), initially all 0.  HT[h] is the largest value
    less than P such that BUF[HT[h]-L..HT[h]-1] has the same hash as
    BUF[P-L..P-1]. Thus, BUF[HT[H]] predicts BUF[P] using an order-L
    context model.
- A decoding table DEC[0..255], where each element is either
    a match length (1..255), LITERAL, or EOB (end of block).
- A parse state STATE, either HEADER or DATA, initially HEADER.

If STATE is HEADER, then the next 32 bytes are used to initialize DEC.
The 256 bits are read in LSB to MSB order.  A 1 bit means LITERAL.
The first 0 bit is stored as EOB.  Subsequent 0 bits are stored
as match lengths in ascending order (1, 2, 3, ..., MAXLEN).  Then
STATE is changed to DATA.

If STATE = DATA then input byte C (0..255) is interpreted as follows:
  If DEC[C] = EOB
    then STATE <- HEADER
  else if DEC[C] = LITERAL then
    output C
    update(C)
  else if DEC[C] = n (match length)
    match <- HT[H]
    For i from 0 to n - 1 do
      c <- BUF[match + i]
      output c
      update(c)

update(c) is defined as
  HT[H] <- P
  H <- update_hash(c)
  BUF[P] <- c
  P <- P + 1 (mod BN)

update_hash(c) is defined as
  H <- (H * M) + c (mod HN)

BN and HN are powers of 2.  M is an odd multiple of ceil(log2(HN)/L).
flzp uses L = 4, BN = 2^22, HN = 2^20, M = 96.  Larger values of BN
and HN generally improve compression at the cost of speed and memory.

Compression requires dividing the input into blocks with 2 passes
over each block. In the first pass, DEC is initialized such that
each byte that appears at least once is initialized to LITERAL
and any remaining entries are initialized to EOB, 1, 2, 3, ..., MAXLEN,
in ascending order, keeping the block small enough to ensure
sufficient match codes.  The block size is as large as possible such
that MAXLEN >= 32 but not larger than 2^16 (65536).

In the second pass, DEC is output as a 32 byte header, then each byte
C of the input is coded by maintaining H, BUF, P, and HT as in decoding.
In addition, the following is maintained:
- The number of pending output bytes, LEN, initially 0 (range 0..MAXLEN).
- The location MATCH in BUF (range 0..BN-1, initially 0) of the
    start of a matching sequence of bytes.

Byte C is compressed:
  If LEN = 0
    then MATCH <- HT[H]
  If C = BUF[MATCH + LEN] and LEN < MAXLEN then
    LEN <- LEN + 1
  else
    output_match()
    MATCH <- HT[H]
    If C = BUF[MATCH] then
      LEN <- 1
    else
      Output C
  update(C)

At the end of a block, any pending matches are output:
  output_match()
  Output EOB

where output_match() is defined
  if LEN > 1 then
    Output n such that DEC[n] = LEN
  else if LEN = 1 then
    Output BUF[P - 1]  (as a literal)
  LEN <- 0

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define NDEBUG
#include <assert.h>

// Create an array p of n elements of type T
template <class T> void alloc(T*&p, int n) {
  p=(T*)calloc(n, sizeof(T));
  if (!p) printf("Out of memory\n"), exit(1);
}

// Input buffer for finding matches
struct Buf {
  enum {BN=(1<<22),    // buffer size
        HN=BN/4};      // hash table size
  unsigned char *buf;  // input buffer
  unsigned int *ht;    // hash table: h -> matched context
  int *enc;            // encoding table: -1=literal, 0=EOB, 1..maxlen=match
  unsigned int h;      // context hash
  int p;               // number of bytes in buf
  int match;           // position of context match
  int len;             // length of match
  int maxlen;          // maximum length
  Buf();                           // initialize
  void update_hash(int c);         // update hash h with c
  void update(int c);              // append c, update ht, h
  void update(int c, FILE *out);   // update and write buf to out if full
  void flush(FILE *out);           // write buf to out
  void output_match(FILE *out);    // compress pending match
  void compress(FILE *out, int c); // compress byte c
};

// Initialize everything to 0
Buf::Buf() {
  alloc(buf, BN);
  alloc(ht, HN);
  alloc(enc, 256);
  h=p=match=len=maxlen=0;
}

// Update hash of last L bytes through c
inline void Buf::update_hash(int c) {
  h=h*(3<<5)+c&HN-1;
}

// Update hash table, context hash, buf, p
inline void Buf::update(int c) {
  ht[h]=p;
  update_hash(c);
  buf[p++&BN-1]=c;
}

// Update and also write buf to out if full
inline void Buf::update(int c, FILE *out) {
  update(c);
  if (!(p&BN-1))
    fwrite(buf, 1, BN, out);
}

// Write buf to out even if not full
inline void Buf::flush(FILE *out) {
  if (p&BN-1)
    fwrite(buf, 1, p&BN-1, out);
}

// Compress pending bytes to out as a match
void Buf::output_match(FILE* out) {
  if (len>0) {
    assert(len<=maxlen && len<256);
    if (len==1)
      putc(buf[p-1&BN-1], out);
    else
      putc(enc[len], out);
    len=0;
  }
}

// Compress one byte c to out
inline void Buf::compress(FILE *out, int c) {
  if (len==0)
    match=ht[h];
  if (len<maxlen && buf[match+len&BN-1]==c)
    ++len;
  else {
    output_match(out);
    match=ht[h];
    if (buf[match&BN-1]==c)
      len=1;
    else
      putc(c, out);
  }
  update(c);
}

// Compress open file in to out
void compress(FILE *in, FILE *out) {
  Buf b;

  // While there is more input
  while (1) {

    // Pass 1, get codes for one block
    unsigned char dec[32]={0};  // decoding table
    int blocksize=0;  // number of bytes in block
    b.maxlen=255;  // highest code
    int c;
    while (b.maxlen>32 && blocksize<(1<<16) && (c=getc(in))!=EOF) {
      ++blocksize;
      if (!(dec[c>>3] & 1<<(c&7))) {
        --b.maxlen;
        dec[c>>3]|=1<<(c&7);
      }
    }
    if (blocksize<1) break;  // EOF?

    // Create encoding table
    int j=0;
    for (int i=0; i<256; ++i) {
      if (!(dec[i>>3] & 1<<(i&7)))
        b.enc[j++]=i;
    }
    assert(j==b.maxlen+1);

    // Pass 2, encode the block
    fseek(in, -blocksize, SEEK_CUR);
    fwrite(dec, 1, 32, out);
    while (blocksize--)
      b.compress(out, getc(in));
    b.output_match(out);
    putc(b.enc[0], out);  // EOB
  }
}

// Decompress open file in to out
void decompress(FILE *in, FILE *out) {
  Buf b;
  enum {HEADER, DATA} state=HEADER;
  int dec[256]={0};

  // while there is more input
  while (1) {
    if (state==HEADER) {  // read 32 bytes into decoder table
      int maxlen=-1;
      for (int i=0; i<32; ++i) {
        int c=getc(in);
        for (int j=0; j<8; ++j)
          dec[i*8+j]=c&1<<j?-1:++maxlen;
      }
      state=DATA;
    }
    else {  // decode a byte
      int c=getc(in);
      if (c==EOF) break;
      else {
        int d=dec[c];
        if (d==0)  // EOB?
          state=HEADER;
        else if (d<0)  // literal?
          b.update(c, out);
        else {  // match length?
          int match=b.ht[b.h];
          for (int i=0; i<d; ++i) {
            c=b.buf[match+i&b.BN-1];
            b.update(c, out);
          }
        }
      }
    }
  }
  b.flush(out);
}

// Compress (c) or decompress (d) input to output
// Command arguments are: c/d input output
int main(int argc, char** argv) {
  clock_t start=clock();
  if (argc<4) {
    printf("flzp file compressor ver. 1\n"
      "(C) 2008, Matt Mahoney. Free software under GPL v3.\n"
      "\n"
      "To compress/decompress: flzp c/d input output\n");
    exit(1);
  }
  FILE *in=fopen(argv[2], "rb");
  if (!in) perror(argv[2]), exit(1);
  FILE *out=fopen(argv[3], "wb");
  if (!out) perror(argv[3]), exit(1);
  if (argv[1][0]=='c')
    compress(in, out);
  else if (argv[1][0]=='d')
    decompress(in, out);
  printf("%s (%ld) -> %s (%ld) in %1.2f s\n", 
    argv[2], ftell(in), argv[3], ftell(out), 
    double(clock()-start)/CLOCKS_PER_SEC);
  return 0;
}
