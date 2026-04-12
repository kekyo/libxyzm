/* libxyzm - A portable implementation of X/Y/ZMODEM
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libxyzm
 */

#include "libxyzm/sync.h"

int xyzm_header_smoke_sync_standalone(void);

int xyzm_header_smoke_sync_standalone(void) {
  xyzm_link_ops_t link;
  xyzm_source_ops_t source;
  xyzm_sink_ops_t sink;
  xyzm_transfer_observer_t observer;
  link.opaque = 0;
  link.send = 0;
  link.recv = 0;
  link.now_ms = 0;
  source.opaque = 0;
  source.next = 0;
  source.read = 0;
  source.end = 0;
  sink.opaque = 0;
  sink.begin = 0;
  sink.write = 0;
  sink.end = 0;
  observer.opaque = 0;
  observer.notify = 0;
  return (int)(
      link.send == 0 &&
      source.next == 0 &&
      sink.begin == 0 &&
      observer.notify == 0);
}
