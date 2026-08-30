#include "net.h"
#include "pcapng_reader.h"
#include "env.h"
int main() {

    install_sigint();

    PcapngReader reader;
    if(!reader.open(PCAP_FILE))
        return 1;

    //read
    //udp socket
    //
}