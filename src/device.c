#include <stdio.h>
#include "device.h"

int device_discover_peers(void) {
    puts("device discovery: using configured peers only");
    return 0;
}

void device_print_status(void) {
    puts("Devices:");
    puts(" - local: active");
    puts(" - peers: read from peers.conf");
}
