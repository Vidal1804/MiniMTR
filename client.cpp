#include <iostream>
#include <netinet/in.h>
#include <string>
#include <unistd.h>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <atomic>
#include <netdb.h>

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


std::string checkConnection(struct sockaddr_in &server){
    std::string IP;
    bool connected = false;
    while(!connected){
        std::cin >> IP;
        if( inet_pton(AF_INET, IP.c_str(), &server.sin_addr) > 0){
            connected = true;
        }
        else{
            std::cout << "Invalid IP address. Please input a correct one!\n";
        }
    }
    return IP;
}

void readTraceResult(int &sd, std::atomic<bool>& flag){
    while(!flag.load()){
        uint32_t len;
        if(!recvAll(sd, &len, sizeof(len))){
            break;
        }
        len = ntohl(len);
        std::vector<char> buffer(len);
        if(!recvAll(sd, buffer.data(), len)){
            break;
        }
        std::string result(buffer.begin(), buffer.end());
        std::cout << "\033[2J\033[3J\033[H[Server]:\n" << result << std::endl;
    }
}

std::string resolveHostname(std::string host){
    struct addrinfo filter{}, *result;
    char ipstr[INET_ADDRSTRLEN];

    filter.ai_family = AF_INET;
    filter.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host.c_str(), NULL, &filter, &result);
    if(status != 0){
        return "";
    }
    if(result == nullptr){
        return "";
    }
    struct sockaddr_in* ip = (struct sockaddr_in*)result->ai_addr;
    void* ip_addr = &(ip->sin_addr);
    
    inet_ntop(result->ai_family, ip_addr, ipstr, sizeof(ipstr));
    freeaddrinfo(result);

    return std::string(ipstr);
}

void sendCommands(int &sd, std::atomic<bool>& flag){
    std::string CMD;
    std::cout << "You can now enter your commands. Type HELP to see all available commands.\n";
    while(!flag.load()){
        std::cin >> CMD;
        // why doesn't switch(string) work like in Rust..
        if(CMD == "HELP"){
            std::cout << "┌──────────────────────────CMDS──────────────────────────────┐\n";
            std::cout << "| HELP = Shows this page.                                    |\n";
            std::cout << "| QUIT = Disconnects from the server and quits application.  |\n";
            std::cout << "| SET-IP <ip> = Sets the trace IP to the current IP.         |\n";
            std::cout << "| START-TRACE = Starts tracing protocol.                     |\n";
            std::cout << "| STOP-TRACE = Stops tracing protocol.                       |\n";
            std::cout << "| SET-INTERVAL = Sets the seconds it takes to update result. |\n";
            std::cout << "| SET-RANGE = Sets the maximum number of hops for the trace. |\n";
            std::cout << "└────────────────────────────────────────────────────────────┘\n";
        }
        else if(CMD == "SET-IP"){
            std::string ip_addr;
            std::cin >> ip_addr;
            std::string actual_ip = resolveHostname(ip_addr);
            struct sockaddr_in test;
            if(inet_pton(AF_INET, actual_ip.c_str(), &(test.sin_addr)) == 1){

                uint32_t len = htonl(CMD.size());
                if(!sendAll(sd, &len, sizeof(len))){
                    perror("Failed to send length of command.");
                    break;
                }
                if(!sendAll(sd, CMD.c_str(), CMD.length())){
                    perror("Failed to send command content.");
                    break;
                }

                len = htonl(actual_ip.size());
                if(!sendAll(sd, &len, sizeof(len))){
                    perror("Failed to send length of command.");
                    break;
                }
                if(!sendAll(sd, actual_ip.c_str(), actual_ip.length())){
                    perror("Failed to send command content.");
                    break;
                }
            }
            else{
                std::cout << "Invalid IP. Please retry the command.\n";
            }
        }
        else if(CMD == "QUIT"){
            uint32_t len = htonl(CMD.size());
            if(!sendAll(sd, &len, sizeof(len))){
                perror("Failed to send length of command.");
                break;
            }
            if(!sendAll(sd, CMD.c_str(), CMD.length())){
                perror("Failed to send command content.");
                break;
            }
            flag.store(true);
            
        }
        else if(CMD == "START-TRACE" || CMD == "STOP-TRACE"){
            uint32_t len = htonl(CMD.size());
            if(!sendAll(sd, &len, sizeof(len))){
                perror("Failed to send length of command.");
                break;
            }
            if(!sendAll(sd, CMD.c_str(), CMD.length())){
                perror("Failed to send command content.");
                break;
            }
        }
        else if(CMD == "SET-INTERVAL" || CMD == "SET-RANGE"){
            uint32_t len = htonl(CMD.size());
            if(!sendAll(sd, &len, sizeof(len))){
                perror("Failed to send length of command.");
                break;
            }
            if(!sendAll(sd, CMD.c_str(), CMD.length())){
                perror("Failed to send command content.");
                break;
            }
            int value;
            std::cin >> value;
            if(!std::cin){
                std::cin.clear();
                std::cin.ignore(10000, '\n');
                std::cout << "Not a valid integer.\n";
            }
            else {
                value = htonl(value);
                if(!sendAll(sd, &value, sizeof(value))){
                    perror("Failed to send value of command.");
                    break;
                }
            }   
            
        }
        else{
            std::string fail_cmd = "UNKNOWN";
            uint32_t len = htonl(fail_cmd.size());
            if(!sendAll(sd, &len, sizeof(len))){
                perror("Failed to send length of command.");
                break;
            }
            if(!sendAll(sd, fail_cmd.c_str(), fail_cmd.length())){
                perror("Failed to send command content.");
                break;
            }
        }

        
    }
}

int main(){

    std::atomic<bool> terminateConnection = false;

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        perror("Couldn't make socket");
        return errno;
    }

    struct sockaddr_in server {};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);

    std::cout << "Welcome to MiniMTR. To start, please enter a valid IP address to connect with the worker server.\n";
    std::string IP = checkConnection(server);

    if( connect(sd, (struct sockaddr*) &server, sizeof(server)) < 0 ){
        perror("Connection failed!");
        return errno;
    }
    else {
        std::cout << "Successfully connected to the server " << IP << std::endl;
    }
    std::thread writer_task(sendCommands, std::ref(sd), std::ref(terminateConnection));
    std::thread reader_task(readTraceResult, std::ref(sd), std::ref(terminateConnection));

    writer_task.join();
    reader_task.join();

    close(sd);

}