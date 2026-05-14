#include <stdio.h>
#include "device.h"

// Stub implementation: real implementation should call tailscale or other discovery API
int device_discover_peers(void) {
    puts("device_discover_peers: stub - pretend we found peers A,B,C");
    return 0; // success
}

void device_print_status(void) {
    puts("Devices:\n - A (local)\n - B (peer)\n - C (peer)");
}
