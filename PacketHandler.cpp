#include <iostream>
#include <string.h>
#include <stdlib.h>      // NULL
#include <stdio.h>       // FILE
#include <unistd.h>      // close
#include <string.h>      // memcpy()
#include <ctype.h>       // toupper()
#include <getopt.h>      // getopt_long()
#include <sys/socket.h>  // socket()
#include <sys/types.h>   // uint8_t
#include <netinet/in.h>  // IF_NET
#include <sys/ioctl.h>     // ioctl
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <vector>
#include <type_traits>
#include <utility>
#include <csignal>
#include <fstream>    
#include <syslog.h>

#include <ProducerConsumerQueue.h>
#include <PacketHandler.h>
#include <Timer.h>
#include <UnixServer.h>
//#include <netx_service.h>

#ifdef FREEBSD
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#define ETH_P_IP    0x0800      /* Internet Protocol packet */
#define ETH_P_8021Q 0x8100          /* 802.1Q VLAN Extended Header  */
#else
#include <linux/if_ether.h>
#endif

using namespace std::placeholders; 

std::mutex m_mutex;
std::condition_variable m_cv;
bool notify = false;
long int pkts = 0;

namespace distdpi {

PacketHandler::PacketHandler(std::string AgentName,
                             std::shared_ptr<FlowTable> ftbl):
    ftbl_(ftbl),
    queue_(500000),
    AgentName_(AgentName) {
}

void PacketHandler::PktRateMeasurer() {
    int pktcounter;
    for (;;) {
        sleep(5);
        if (!running_)
            break;
        if (pktcounter == 0) {
            syslog(LOG_INFO, "Packets/Second %ld", (pkts - pktcounter) / 5);
            pktcounter = pkts;
        }
        else {
            syslog(LOG_INFO, "Packets/Second %ld", (pkts - pktcounter) / 5);
            pktcounter = pkts;
        }
    }
}

void PacketHandler::PacketProducer(PktMetadata *pktmdata,
                                   uint32_t len) {
    PktMdata mdata;

    pkts++;
    mdata.dir = pktmdata->dir;
    mdata.filter = pktmdata->filterPtr;
    mdata.pkt.assign((char*)pktmdata->pktPtr, len);
    
    while(!queue_.write(mdata)) {
        continue;
    }
    notify = true;
    m_cv.notify_one();
}

void PacketHandler::StaticPacketProducer(void *obj,
                                         PktMetadata *pktmdata,
                                         uint32_t len) {
    ((PacketHandler *)obj)->PacketProducer(pktmdata, len);
}

void PacketHandler::PacketConsumer() {
    try {
        for (;;) {
            PktMdata mdata;        
            std::unique_lock<std::mutex> lock(m_mutex);

            while(!notify)
                m_cv.wait(lock);
            if (!running_)
                break;
        
            while(!queue_.isEmpty()) {
                queue_.read(mdata);
                this->classifyFlows(&mdata);
            }
            notify = false;
        }
    } catch (const std::exception& ex) {
        stop();
    }
    std::cout << "Exiting packet consumer thread " << std::endl;
}

void PacketHandler::classifyFlows(PktMdata *mdata) {
#ifdef FREEBSD
    typedef struct ip iphdr;
#endif
    const u_char *ptr = (u_char *) mdata->pkt.c_str();
    u_int len = mdata->pkt.size();
    const u_short *eth_type;
    const iphdr *iph;
    ConnKey key;

    // Read the first ether type
    len -= 12;
    eth_type = reinterpret_cast<const u_short *>(&ptr[12]);

    // Strip any vlans if present
    while (ntohs(*eth_type) == ETH_P_8021Q)
    {
            eth_type += 2;
            len -= 4;
    }

    // Ignore non-ip packets
    if (ntohs(*eth_type) != ETH_P_IP)
        return;

    len -= 2;
    iph = reinterpret_cast<const iphdr *>(++eth_type);

#ifdef FREEBSD
    // Do basic sanity of the ip header
    if (iph->ip_hl < 5 || iph->ip_v != 4 || len < ntohs(iph->ip_len))
        return;

    // Fix up the length as it may have been padded
    len = ntohs(iph->ip_len);

    // Build the 5-tuple key
    key.ip_proto = iph->ip_p;
    key.src_addr = ntohl(iph->ip_src.s_addr);
    key.dst_addr = ntohl(iph->ip_dst.s_addr);

    // Find the tcp header offset
    len -= (iph->ip_hl << 2);
    ptr = reinterpret_cast<const u_char *>(iph) + (iph->ip_hl << 2);
#else
    // Do basic sanity of the ip header
    if (iph->ihl < 5 || iph->version != 4 || len < ntohs(iph->tot_len))
        return;

    // Fix up the length as it may have been padded
    len = ntohs(iph->tot_len);

    // Build the 5-tuple key
    key.ipproto = iph->protocol;
    key.srcaddr = ntohl(iph->saddr);
    key.dstaddr = ntohl(iph->daddr);

    // Find the tcp header offset
    len -= (iph->ihl << 2);
    ptr = reinterpret_cast<const u_char *>(iph) + (iph->ihl << 2);
#endif
    populateFlowTable(ptr, len, &key, mdata->filter, mdata->dir); 
}

void PacketHandler::populateFlowTable(const u_char *ptr,
                                      u_int len,
                                      ConnKey *key,
                                      void *filter,
                                      uint8_t dir) {
    const tcphdr *th;
    const udphdr *uh;

    switch (key->ipproto)
    {
    case IPPROTO_TCP:
        th = reinterpret_cast<const tcphdr *>(ptr);
#ifdef FREEBSD
        key->srcport = ntohs(th->th_sport);
        key->dstport = ntohs(th->th_dport);
        ptr += (th->th_off << 2);
        len -= (th->th_off << 2);
#else
        key->srcport = ntohs(th->source);
        key->dstport = ntohs(th->dest);
        ptr += (th->doff << 2);
        len -= (th->doff << 2);
#endif
        break;
    case IPPROTO_UDP:
        uh = reinterpret_cast<const udphdr *>(ptr);
#ifdef FREEBSD
        key->srcport = ntohs(uh->uh_sport);
        key->dstport = ntohs(uh->uh_dport);
#else
        key->srcport = ntohs(uh->source);
        key->dstport = ntohs(uh->dest);
#endif
        ptr += sizeof(*uh);
        len -= sizeof(*uh);
        break;
    };

    ConnMetadata connmdata;
    std::string pkt_string;
    pkt_string.append((char *)ptr, len);

    ftbl_->InsertOrUpdateFlows(key, pkt_string, filter, dir);
}

void PacketHandler::ConnectToPktProducer() {
    start_netx_service(AgentName_.c_str(),
                       PacketHandler::StaticPacketProducer,
                       this);
}

void PacketHandler::start() {
    running_ = true;
    pkthdl_threads.push_back(std::thread(&PacketHandler::ConnectToPktProducer, this));
    pkthdl_threads.push_back(std::thread(&PacketHandler::PacketConsumer, this));
    //pkthdl_threads.push_back(std::thread(&PacketHandler::PktRateMeasurer, this));
}

void PacketHandler::stop() {
    running_ = false;
    notify = true;
    m_cv.notify_one();
    stop_netx_service();

    for (unsigned int i = 0; i < pkthdl_threads.size(); i++) {
        pkthdl_threads[i].join();
    }

    std::cout << " PacketHandler stop called " << std::endl;    
}

PacketHandler::~PacketHandler() {
    std::cout << "Calling PacketHandler Destructor" << std::endl;
}

} 
