#include "env.h"
#include "helpers.h"
#include "net.h"
#include "pcapng_reader.h"

int main() {

    install_signals();

    PcapngReader reader;
    if (!reader.open(PCAP_FILE)) {
        return 1;
    }

    int fd = udp_send_socket(MCAST_IF);

    // read
    // udp socket
    //

    close(fd);
    return g_stop ? 130 : 0;
}
