
#include <stdlib.h>

#include "cm-event.h"
#include "cm-util.h"

#include "ringbuffer.h"


ringBuffer_typedef(unsigned long, IgnoreErrRingbuf);
static IgnoreErrRingbuf ignore_ringbuf;
static IgnoreErrRingbuf* p_ignore_ringbuf = &ignore_ringbuf;



void set_ignore(Display *dpy, unsigned long sequence) {
  if(unlikely(isBufferFull(p_ignore_ringbuf))) {
    bufferIncrease(p_ignore_ringbuf, p_ignore_ringbuf->size*2);
  }
  bufferWrite(p_ignore_ringbuf, sequence);
}


int should_ignore(Display *dpy, unsigned long sequence) {
  unsigned long buf_seq;
  discard_ignore(dpy, sequence);
  if(isBufferEmpty(p_ignore_ringbuf)) return False;
  buf_seq = bufferReadPeek(p_ignore_ringbuf);
  return buf_seq == sequence;
}


void discard_ignore(Display *dpy, unsigned long sequence) {
  while(! isBufferEmpty(p_ignore_ringbuf)){
    unsigned long buf_seq;
    buf_seq = bufferReadPeek(p_ignore_ringbuf);
    if ((long) (sequence - buf_seq) > 0) {
      bufferReadSkip(p_ignore_ringbuf);
    } else {
      break;
    }
  }
}

bool event_init()
{
  bufferInit(ignore_ringbuf, 2047, unsigned long);
  return true;
}
