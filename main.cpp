#include <iostream>
#include <cstring>
#include <string>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  To receive:  " << argv[0] << " listen <port>\n";
        std::cout << "  To send:     " << argv[0] << " send <IP> <port> <message>\n";
        std::cout << "\nExample:\n";
        std::cout << "  On machine 1: " << argv[0] << " listen 12345\n";
        std::cout << "  On machine 2: " << argv[0] << " send 192.168.1.100 12345 \"Hello!\"\n";
        return 1;
    }
    
    // Initialize Winsock for Windows
    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            return 1;
        }
    #endif
    
    std::string mode = argv[1];
    
    if (mode == "listen") {
        // MODE 1: LISTENER (receiver)
        if (argc < 3) {
            std::cout << "Specify port: " << argv[0] << " listen <port>\n";
            return 1;
        }
        
        int port = std::stoi(argv[2]);
        
        // Create UDP socket
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Error creating socket\n";
            return 1;
        }
        
        // Configure address for receiving
        sockaddr_in my_addr;
        memset(&my_addr, 0, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(port);
        my_addr.sin_addr.s_addr = INADDR_ANY; // Accept from all addresses
        
        // Bind socket to port
        if (bind(sock, (sockaddr*)&my_addr, sizeof(my_addr)) < 0) {
            std::cerr << "Error binding to port " << port << "\n";
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            return 1;
        }
        
        std::cout << "Listening on UDP port " << port << "...\n";
        std::cout << "Press Ctrl+C to exit\n\n";
        
        char buffer[1024];
        sockaddr_in from_addr;
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        while (true) {
            memset(buffer, 0, sizeof(buffer));
            memset(&from_addr, 0, sizeof(from_addr));
            
            // Wait for data
            int bytes_received = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                                          (sockaddr*)&from_addr, &from_len);
            
            if (bytes_received > 0) {
                buffer[bytes_received] = '\0';
                
                // Get sender's IP
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));
                
                std::cout << "Received from " << ip_str << ":" 
                          << ntohs(from_addr.sin_port) << " -> "
                          << buffer << "\n";
                
                // Auto-reply
                std::string reply = "Received: " + std::string(buffer);
                sendto(sock, reply.c_str(), reply.length(), 0,
                       (sockaddr*)&from_addr, from_len);
                std::cout << "Sent reply\n";
            }
        }
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
        
    } else if (mode == "send") {
        // MODE 2: SENDER
        if (argc < 5) {
            std::cout << "Specify IP, port and message: " 
                      << argv[0] << " send <IP> <port> \"message\"\n";
            return 1;
        }
        
        std::string target_ip = argv[2];
        int target_port = std::stoi(argv[3]);
        std::string message = argv[4];
        
        // Create UDP socket
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Error creating socket\n";
            return 1;
        }
        
        // Configure receiver address
        sockaddr_in target_addr;
        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target_port);
        
        // Convert IP address
        if (inet_pton(AF_INET, target_ip.c_str(), &target_addr.sin_addr) <= 0) {
            std::cerr << "Invalid IP address: " << target_ip << "\n";
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            return 1;
        }
        
        std::cout << "Sending \"" << message << "\" to " 
                  << target_ip << ":" << target_port << "\n";
        
        // Send message
        int bytes_sent = sendto(sock, message.c_str(), message.length(), 0,
                                (sockaddr*)&target_addr, sizeof(target_addr));
        
        if (bytes_sent < 0) {
            std::cerr << "Error sending\n";
        } else {
            std::cout << "Sent " << bytes_sent << " bytes\n";
            
            // Wait for reply (5 seconds)
            std::cout << "Waiting for reply...\n";
            
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sock, &read_fds);
            
            struct timeval timeout;
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;
            
            char buffer[1024];
            sockaddr_in from_addr;
            #ifdef _WIN32
                int from_len = sizeof(from_addr);
            #else
                socklen_t from_len = sizeof(from_addr);
            #endif
            
            int ready = select(sock + 1, &read_fds, NULL, NULL, &timeout);
            if (ready > 0) {
                memset(buffer, 0, sizeof(buffer));
                int bytes_received = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                                              (sockaddr*)&from_addr, &from_len);
                if (bytes_received > 0) {
                    buffer[bytes_received] = '\0';
                    std::cout << "Reply: " << buffer << "\n";
                }
            } else {
                std::cout << "No reply received (5 sec timeout)\n";
            }
        }
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
    }
    
    #ifdef _WIN32
        WSACleanup();
    #endif
    
    return 0;
}