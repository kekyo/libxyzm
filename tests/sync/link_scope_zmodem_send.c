#include "libxyzm/xyzm.h"

static xyzm_status_t (*volatile link_scope_entry)(
    const xyzm_zmodem_send_request_t *request,
    xyzm_transfer_report_t *report) = xyzm_zmodem_send;

int main(void) {
  return link_scope_entry == 0;
}
