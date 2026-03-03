#include <asm-generic/socket.h>
#include <netinet/ip_icmp.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <mutex>

#define PORT 44556
extern int errno;


bool sendAll(int fd, const void* data, size_t len) {
    const char* ptr = (const char*)data;
    size_t totalSent = 0;

    while (totalSent < len) {
        ssize_t n = send(fd, ptr + totalSent, len - totalSent, 0);
        if (n <= 0) {
            return false;
        }
        totalSent += n;
    }
    return true;
}

bool recvAll(int fd, void* data, size_t len) {
    char* ptr = (char*)data;
    size_t totalReceived = 0;

    while (totalReceived < len) {
        ssize_t n = recv(fd, ptr + totalReceived, len - totalReceived, 0);
        if (n <= 0) {
            return false;
        }
        totalReceived += n;
    }
    return true;
}

unsigned short checksum(void *b, int len) {
    unsigned short *buf = (unsigned short*)b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2) {
        sum += *buf++;
    }

    if (len == 1) {
        sum += *(unsigned char *)buf;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    result = ~sum;
    return result;
}

void performTrace(int cd, std::atomic<bool>& term, std::atomic<bool>& tracing, std::atomic<int>& interval, std::atomic<int>& range, std::string& IP, std::mutex& ip_lock){
    std::string currentIP;
    std::string msg;
    int sd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    struct sockaddr_in ipaddr {};
    ipaddr.sin_family = AF_INET;
    while(!term.load()){
        {
            std::lock_guard<std::mutex> lock(ip_lock);
            currentIP = IP;
        }
        if(tracing.load()){
            if(inet_pton(AF_INET, currentIP.c_str(), &ipaddr.sin_addr) <= 0){
                std::cout << cd << ": Failed with current IP. " << currentIP << " connection will retry in " << interval << " seconds.\n";
                std::this_thread::sleep_for(std::chrono::seconds(interval.load()));
                continue;
            }

            for(int ttl = 1; ttl <= range; ttl++){
                if(setsockopt(sd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0){
                    perror("Failed during setsockopt.\n");
                }

                struct icmphdr packet {};
                packet.type = ICMP_ECHO;
                packet.un.echo.id = htons(cd);
                packet.un.echo.sequence = ttl;
                packet.checksum = checksum(&packet, sizeof(packet));

                auto start_time = std::chrono::high_resolution_clock::now();
                ssize_t bytes = sendto(sd, &packet, sizeof(packet), 0, (struct sockaddr*)&ipaddr, sizeof(ipaddr));
                if(bytes <= 0){
                    perror("Couldn't send packet.");
                    break;
                }

                struct sockaddr_in reply {};
                socklen_t reply_size = sizeof(reply);
                char buffer[1024];

                struct timeval tv = {1, 0};
                setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

                while(true){
                    int result = recvfrom(sd, buffer, sizeof(buffer), 0, (struct sockaddr*)&reply, &reply_size);
                    
                    if(result < 0){
                        char msgbuff[50];
                        snprintf(msgbuff, sizeof(msgbuff), "%d) * * *\n", ttl);
                        msg += msgbuff;
                        break;
                    }
                    auto end_time = std::chrono::high_resolution_clock::now();
                    auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

                    bool correct_packet = false;
                    bool destination = false;

                    struct iphdr* ip_header = (struct iphdr*)buffer;
                    int ip_header_length = ip_header->ihl*4;
                    struct icmphdr* icmp_header = (struct icmphdr*)(buffer + ip_header_length);

                    if(icmp_header->type == ICMP_ECHOREPLY){
                        if(icmp_header->un.echo.id == htons(cd)){
                            correct_packet = true;
                            destination = true;
                        }
                    }
                    else if(icmp_header->type == ICMP_TIME_EXCEEDED){
                        struct iphdr* inner_ip = (struct iphdr*)(buffer + ip_header_length + 8);
                        int inner_ip_len = inner_ip->ihl * 4;
                        struct icmphdr* inner_icmp = (struct icmphdr*)((char*)inner_ip + inner_ip_len);

                        if (inner_icmp->un.echo.id == htons(cd)) {
                            correct_packet = true;
                        }
                    }

                    if(correct_packet){
                        char ipaddr_str[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &reply.sin_addr, ipaddr_str, INET_ADDRSTRLEN);

                        char msgbuff[50];
                        snprintf(msgbuff, sizeof(msgbuff), "%d) %s - ping: %dms\n", ttl, ipaddr_str, rtt);
                        msg += msgbuff;

                        if(destination){
                            ttl = range.load() + 1;
                        }
                        break;
                    }

                }
                
            }

            
            int32_t len = htonl(msg.size());
            sendAll(cd, &len, sizeof(len));
            sendAll(cd, msg.c_str(), msg.size());
            std::this_thread::sleep_for(std::chrono::seconds(interval.load()));
            msg.clear();
            
        }
    }
}

void ClientThread(int cd){
    std::atomic<bool> should_terminate = false;
    std::atomic<bool> tracing = false;
    std::atomic<int> trace_interval = 2;
    std::atomic<int> max_hops = 30;
    std::string IPAddress = "";
    std::mutex iplock;

    std::thread traceTask(performTrace, cd, std::ref(should_terminate), std::ref(tracing), std::ref(trace_interval), std::ref(max_hops), std::ref(IPAddress), std::ref(iplock));
    traceTask.detach();

    while(!should_terminate.load()){
        uint32_t len;
        if(!recvAll(cd, &len, sizeof(len))){
            perror("Fail when receiving data");
            break;
        }
        len = ntohl(len);
        std::vector<char> buffer(len);
        if(!recvAll(cd, buffer.data(), len)){
            perror("Failed to receive full message");
            break;
        }
        std::string CMD(buffer.begin(), buffer.end());
        if(CMD == "SET-IP"){
            uint32_t len;
            if(!recvAll(cd, &len, sizeof(len))){
               perror("Fail when receiving length");
               break;
            }
            len = ntohl(len);
            std::vector<char> ip_buf(len);
            if(!recvAll(cd, ip_buf.data(), len)){
                perror("Failed to receive full message");
                break;
            }
            std::string IP(ip_buf.begin(), ip_buf.end());

            std::string msg = "Successfully updated IP address to " + IP;
            len = htonl(msg.size());
            sendAll(cd, &len, sizeof(len));
            sendAll(cd, msg.c_str(), msg.size());

            {
                std::lock_guard<std::mutex> lock(iplock);
                IPAddress = IP;
            }
            std::cout<< cd << ": Set the IP to " << IP << std::endl;
            
        }
        else if(CMD == "QUIT"){
            should_terminate.store(true);
            std::string msg = "Disconnected. Quitting the app.";
            uint32_t len = htonl(msg.size());
            sendAll(cd, &len, sizeof(len));
            sendAll(cd, msg.c_str(), msg.size());
        }
        else if(CMD  == "START-TRACE"){
            if(tracing == false && IPAddress != ""){
                std::cout << cd << ": Starting Trace Protocol.\n";
                std::string msg = "Trace protocol started.";
                uint32_t len = htonl(msg.size());
                sendAll(cd, &len, sizeof(len));
                sendAll(cd, msg.c_str(), msg.size());
                tracing.store(true);
            }
            else{
                std::string msg = "A trace is already ongoing or IP address not set.";
                uint32_t len = htonl(msg.size());
                sendAll(cd, &len, sizeof(len));
                sendAll(cd, msg.c_str(), msg.size());
            }
            
        }
        else if(CMD == "STOP-TRACE"){
            if(tracing == true){
                std::cout << cd << ": Stopping Trace Protocol.\n";
                std::string msg = "Trace protocol stopped.";
                uint32_t len = htonl(msg.size());
                sendAll(cd, &len, sizeof(len));
                sendAll(cd, msg.c_str(), msg.size());
                tracing.store(false);
            }
            else{
                std::string msg = "No ongoing trace to stop.";
                uint32_t len = htonl(msg.size());
                sendAll(cd, &len, sizeof(len));
                sendAll(cd, msg.c_str(), msg.size());
            }
        }
        else if(CMD == "SET-INTERVAL"){
            uint32_t interval;
            if(!recvAll(cd, &interval, sizeof(interval))){
               perror("Fail when receiving length");
               break;
            }
            interval = ntohl(interval);
            trace_interval.store(interval);

            std::string msg = "Updated interval value to " + std::to_string(interval) + " seconds.";
            uint32_t len = htonl(msg.size());
            sendAll(cd, &len, sizeof(len));
            sendAll(cd, msg.c_str(), msg.size());

            std::cout << cd << ": Set Trace Update interval to " << interval << " seconds.\n";
        }
        else if(CMD == "SET-RANGE"){
            uint32_t range;
            if(!recvAll(cd, &range, sizeof(range))){
               perror("Fail when receiving length");
               break;
            }
            range = ntohl(range);
            max_hops.store(range);

            std::string msg = "Updated hop range to " + std::to_string(range) + " hops.";
            uint32_t len = htonl(msg.size());
            sendAll(cd, &len, sizeof(len));
            sendAll(cd, msg.c_str(), msg.size());

            std::cout << cd << ": Set Hop Range of Trace Protocol to " << range << " hops.\n";
        }
    }
    std::cout << cd << ": Disconnecting and closing thread.\n";
    close(cd);
}

bool isRoot() {
    return geteuid() == 0;
}

int main(){

    if(!isRoot()){
        std::cout << "Error! Run the server with sudo permissions, otherwise the app won't function.";
        return 1;
    }

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if(sd < 0){
        perror("Could not make socket.");
        return errno;
    }
    struct sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(PORT);

    int on = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if(bind(sd, (struct sockaddr*)&server, sizeof(struct sockaddr)) == -1){
        perror("Binding failed.");
        return errno;
    } 

    if ( listen(sd, 10) == -1 ) {
        perror("Error at listening!");
        return errno;
    }
    std::cout << "Server active.\n";
    while(1){
        struct sockaddr_in client{};
        socklen_t clientLength = sizeof(client);

        

        int cd = accept(sd, (struct sockaddr*) &client, &clientLength);
        if(cd < 0 ){
            perror("Accepting the client failed!");
            return errno;
        }

        std::cout << "Client connected with ID: " << cd << std::endl;

        std::thread client_thread(ClientThread, cd);
        client_thread.detach();
    }
    close(sd);
}