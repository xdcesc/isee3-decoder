// K=24 r=1/2 Viterbi decoder in portable C
// Copyright Mar 2014, Phil Karn, KA9Q
// May be used under the terms of the GNU Lesser General Public License (LGPL)
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <limits.h>
#include <stdint.h>
#include "viterbi224.h"
#include "code.h"


typedef union { uint32_t w[1<<(K-6)]; uint8_t c[1<<(K-4)];} decision_t;
typedef union { uint32_t w[1<<(K-1)]; } metric_t;

static union branchtab224 { uint32_t w[1<<(K-2)]; } Branchtab224[2] __attribute__ ((aligned(16)));

// State info for instance of Viterbi decoder
struct v224 {
  int len;           // Length of decision memory
  metric_t metrics1; // path metric buffer 1
  metric_t metrics2; // path metric buffer 2
  decision_t *dp;          // Pointer to current decision
  metric_t *old_metrics,*new_metrics; // Pointers to path metrics, swapped on every bit
  decision_t *decisions;   // Beginning of decisions for block
};

static inline int parity(unsigned long long x){
  return __builtin_parityll(x);
}


// Initialize Viterbi decoder for start of new frame
int init_viterbi224(void *p,int starting_state){
  struct v224 *vp = p;
  int i;

  if(p == NULL)
    return -1;
  for(i=0;i<(1<<(K-1));i++)
    vp->metrics1.w[i] = 1000;

  vp->old_metrics = &vp->metrics1;
  vp->new_metrics = &vp->metrics2;
  vp->dp = vp->decisions;
  vp->old_metrics->w[starting_state & ((1<<(K-1))-1)] = 0; // Bias known start state
  return 0;
}

// Create a new instance of a Viterbi decoder
void *create_viterbi224(int len){
  struct v224 *vp;
  int state;

  if((vp = (struct v224 *)malloc(sizeof(struct v224))) == NULL)
    return NULL;
  vp->len = len;
  if((vp->decisions = malloc(len*sizeof(decision_t))) == NULL){
    free(vp);
    return NULL;
  }
  for(state=0;state < (1<<(K-2));state++){
    Branchtab224[0].w[state] = G1FLIP ^ parity((2*state) & POLY1) ? 255 : 0;
    Branchtab224[1].w[state] = G2FLIP ^ parity((2*state) & POLY2) ? 255 : 0;
  }
  init_viterbi224(vp,0);
  return vp;
}


// Viterbi chainback
int chainback_viterbi224(
      void *p,
      unsigned char *data, // Decoded output data
      unsigned int nbits, // Number of data bits
      unsigned int endstate){ // Terminal encoder state
  struct v224 *vp = p;
  unsigned char dbyte = 0;

  if(p == NULL)
    return -1;
  if(nbits == 0)
    return 0;

  endstate &= (1<<(K-1))-1;
  while(nbits-- != 0){
    int bit;
    decision_t *dp;

    // Accumulate decoded data bits as they fall off the right end of endstate
    dbyte = ((endstate & 1) << 7) | (dbyte >> 1);
    if((nbits & 7) == 0)
      data[nbits>>3] = dbyte;

    // Shift new bit into left end of endstate
    dp = &vp->decisions[nbits % vp->len]; // Wrap back to allow stream decoding
    bit = (dp->c[endstate >> 3] >> (endstate & 7)) & 1;
    endstate = (bit << (K-2)) | (endstate >> 1);
  }
  return 0;
}

// Chain back given distance from current state and decode 1 bit
int decodebit_viterbi224(void *p,int delay,int endstate){
  struct v224 *vp = p;
  decision_t *dp;
  int bit,i,minmetric;

  if(vp == NULL)
    return -1;

  // endstate < 0 means "find best path" - it's very slow
  if(endstate < 0){
    minmetric = vp->old_metrics->w[0];
    endstate = 0;
    for(i=1;i<(1<<23);i++){
      if(vp->old_metrics->w[i] < minmetric){
	minmetric = vp->old_metrics->w[i];
	endstate = i;
      }
    }
  }

  dp = vp->dp;
#if  0
  printf("index %ld best path %06x metric %d\n",
	 dp - vp->decisions,endstate,minmetric);
#endif
  bit = -1;
  while(delay-- > 0){
    if(--dp < vp->decisions)
      dp = &vp->decisions[vp->len-1];
    bit = (dp->c[endstate>>3] >> (endstate & 7)) & 1; // these constants do NOT change with K
    endstate = (bit << (K-2)) | (endstate >> 1);
#if 0
    printf("new bit %d endstate %6x\n",bit,endstate);
#endif
  }
#if 0
  printf("viterbi returning %d\n",bit);
#endif
  return bit;
}

// Delete instance of a Viterbi decoder
void delete_viterbi224(void *p){
  struct v224 *vp = p;

  if(vp != NULL){
    free(vp->decisions);
    free(vp);
  }
}

// Update decoder with a block of demodulated symbols
// Note that nbits is the number of decoded data bits, not the number
// of symbols!

int update_viterbi224_blk(void *p,const unsigned char *syms,int nbits){
  struct v224 *vp = p;
  void *tmp;
  decision_t *d;
  int i;

  if(p == NULL)
    return -1;
  d = (decision_t *)vp->dp;
  while(nbits--){
    memset(d,0,sizeof(decision_t));
    for(i=0;i<(1<<(K-2));i++){
      unsigned long metric,m0,m1,m2,m3,decision0,decision1;

      metric = ((Branchtab224[0].w[i] ^ syms[0]) + (Branchtab224[1].w[i] ^ syms[1]));
      m0 = vp->old_metrics->w[i] + metric;
      m1 = vp->old_metrics->w[i+(1<<(K-2))] + (510 - metric);
      m2 = vp->old_metrics->w[i] + (510-metric);
      m3 = vp->old_metrics->w[i+(1<<(K-2))] + metric;
      decision0 = (signed long)(m0-m1) >= 0;
      decision1 = (signed long)(m2-m3) >= 0;
      vp->new_metrics->w[2*i] = decision0 ? m1 : m0;
      vp->new_metrics->w[2*i+1] = decision1 ? m3 : m2;
      d->c[i/4] |= ((decision0|(decision1<<1)) << ((2*i)&7));
    }
    syms += 2;
    // Wrap around to allow continuous (non-framed) decoding
    if(++d >= &vp->decisions[vp->len])
      d = vp->decisions;
    // Swap pointers to old and new metrics
    tmp = vp->old_metrics;
    vp->old_metrics = vp->new_metrics;
    vp->new_metrics = tmp;
  }    
  vp->dp = d;
  return 0;
}

