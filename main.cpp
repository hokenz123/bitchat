#include <iostream>
#include <string>
#include <vector>
#include <thread>

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

class P2PHolePunch {
private:
    std::vector<std::string> stun_servers = {
        "stun.l.google.com:19302",
        "stun1.l.google.com:19302",
        "stun2.l.google.com:19302",
        "stun3.l.google.com:19302",
        "stun4.l.google.com:19302"
    };
    
public:
    struct PublicAddress {
        std::string ip;
        int port;
    };
    
    PublicAddress getPublicAddress(int local_port) {
        for (const auto& server : stun_servers) {
            size_t colon = server.find(':');
            std::string host = server.substr(0, colon);
            int port = std::stoi(server.substr(colon + 1));
            
            PublicAddress result = querySTUN(host, port, local_port);
            if (!result.ip.empty()) {
                return result;
            }
        }
        
        return {"", 0};
    }
    
    void startHolePunch(const std::string& peer_ip, int peer_port, 
                        const std::string& my_public_ip, int my_public_port) {
        std::cout << "\n=== STARTING HOLE PUNCH ===\n";
        std::cout << "Peer: " << peer_ip << ":" << peer_port << "\n";
        std::cout << "My public: " << my_public_ip << ":" << my_public_port << "\n";
        
        // Создаем сокет для hole punching
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        
        // Привязываем к тому же порту, что использовали для STUN
        sockaddr_in local_addr;
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(my_public_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (sockaddr*)&local_addr, sizeof(local_addr));
        
        // Целевой адрес пира
        sockaddr_in peer_addr;
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(peer_port);
        inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr);
        
        // Запускаем поток для приема
        std::atomic<bool> running(true);
        std::thread receiver([&]() {
            char buffer[1024];
            sockaddr_in from_addr;
            
            #ifdef _WIN32
                int from_len = sizeof(from_addr);
            #else
                socklen_t from_len = sizeof(from_addr);
            #endif
            
            std::cout << "Receiver thread started...\n";
            
            while (running) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(sock, &fds);
                
                timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                
                if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
                    int len = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                                      (sockaddr*)&from_addr, &from_len);
                    if (len > 0) {
                        buffer[len] = '\0';
                        char ip_str[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));
                        
                        std::cout << "\n[FROM " << ip_str << ":" 
                                 << ntohs(from_addr.sin_port) << "] "
                                 << buffer << "\n";
                    }
                }
            }
        });
        
        // Основной цикл hole punching
        std::cout << "\nHole punching in progress...\n";
        std::cout << "Send messages to peer:\n";
        
        for (int i = 0; i < 10; i++) {
            std::string msg = "PUNCH_" + std::to_string(i);
            sendto(sock, msg.c_str(), msg.length(), 0,
                  (sockaddr*)&peer_addr, sizeof(peer_addr));
            
            std::cout << "Sent punch #" << i << " to peer\n";
            
            #ifdef _WIN32
                Sleep(500);
            #else
                usleep(500000);
            #endif
        }
        
        // Теперь можем общаться
        std::string input;
        std::cin.ignore(); // Очищаем буфер
        
        while (true) {
            std::cout << "> ";
            std::getline(std::cin, input);
            
            if (input == "exit") break;
            
            sendto(sock, input.c_str(), input.length(), 0,
                  (sockaddr*)&peer_addr, sizeof(peer_addr));
            std::cout << "Message sent!\n";
        }
        
        running = false;
        receiver.join();
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
    }
    
private:
    PublicAddress querySTUN(const std::string& host, int port, int local_port) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        
        // Привязываем сокет
        sockaddr_in local_addr;
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(local_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (sockaddr*)&local_addr, sizeof(local_addr));
        
        // STUN запрос
        unsigned char stun_request[] = {
            0x00, 0x01, // Message Type: Binding Request
            0x00, 0x00, // Message Length: 0
            0x21, 0x12, 0xA4, 0x42, // Magic Cookie
            0x00, 0x00, 0x00, 0x00, // Transaction ID
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        
        // Генерируем случайный Transaction ID
        for (int i = 8; i < 20; i++) {
            stun_request[i] = rand() % 256;
        }
        
        // Отправляем на STUN сервер
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
        hostent* he = gethostbyname(host.c_str());
        if (!he) {
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            return {"", 0};
        }
        
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
        
        sendto(sock, (char*)stun_request, sizeof(stun_request), 0,
              (sockaddr*)&server_addr, sizeof(server_addr));
        
        // Ждем ответ
        unsigned char response[1024];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        
        timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        
        if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
            int len = recvfrom(sock, (char*)response, sizeof(response), 0,
                              (sockaddr*)&from_addr, &from_len);
            
            if (len > 0) {
                // Парсим ответ STUN
                if (len >= 20 && response[0] == 0x01 && response[1] == 0x01) {
                    // Binding Success Response
                    int pos = 20;
                    while (pos < len) {
                        uint16_t attr_type = (response[pos] << 8) | response[pos+1];
                        uint16_t attr_len = (response[pos+2] << 8) | response[pos+3];
                        
                        if (attr_type == 0x0001) { // MAPPED-ADDRESS
                            if (pos + 8 <= len) {
                                uint16_t port = (response[pos+6] << 8) | response[pos+7];
                                std::string ip = std::to_string(response[pos+8]) + "." +
                                               std::to_string(response[pos+9]) + "." +
                                               std::to_string(response[pos+10]) + "." +
                                               std::to_string(response[pos+11]);
                                
                                #ifdef _WIN32
                                    closesocket(sock);
                                #else
                                    close(sock);
                                #endif
                                
                                return {ip, port};
                            }
                        }
                        else if (attr_type == 0x0020) { // XOR-MAPPED-ADDRESS
                            if (pos + 12 <= len) {
                                // XOR с Magic Cookie
                                uint16_t port = (response[pos+6] << 8) | response[pos+7];
                                port ^= 0x2112; // Первые 2 байта Magic Cookie
                                
                                std::string ip = std::to_string(response[pos+8] ^ 0x21) + "." +
                                               std::to_string(response[pos+9] ^ 0x12) + "." +
                                               std::to_string(response[pos+10] ^ 0xA4) + "." +
                                               std::to_string(response[pos+11] ^ 0x42);
                                
                                #ifdef _WIN32
                                    closesocket(sock);
                                #else
                                    close(sock);
                                #endif
                                
                                return {ip, port};
                            }
                        }
                        
                        pos += 4 + attr_len;
                        if (attr_len % 4 != 0) {
                            pos += 4 - (attr_len % 4); // Padding
                        }
                    }
                }
            }
        }
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
        
        return {"", 0};
    }
};

int main() {
    #ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
    #endif
    
    std::cout << "=== P2P HOLE PUNCHING TOOL ===\n\n";
    
    // Получаем наш публичный адрес через STUN
    P2PHolePunch p2p;
    
    std::cout << "Getting public address via STUN...\n";
    auto my_public = p2p.getPublicAddress(0); // 0 - любой свободный порт
    
    if (my_public.ip.empty()) {
        std::cout << "ERROR: Cannot get public address!\n";
        std::cout << "Possible reasons:\n";
        std::cout << "1. Symmetric NAT (most restrictive)\n";
        std::cout << "2. No internet connection\n";
        std::cout << "3. Firewall blocking\n";
        return 1;
    }
    
    std::cout << "SUCCESS! Your public address:\n";
    std::cout << "IP: " << my_public.ip << "\n";
    std::cout << "Port: " << my_public.port << "\n\n";
    
    std::cout << "Share this info with your peer.\n";
    std::cout << "Then enter peer's info:\n";
    
    std::string peer_ip;
    int peer_port;
    
    std::cout << "Peer IP: ";
    std::cin >> peer_ip;
    
    std::cout << "Peer Port: ";
    std::cin >> peer_port;
    
    // Запускаем hole punching
    p2p.startHolePunch(peer_ip, peer_port, my_public.ip, my_public.port);
    
    #ifdef _WIN32
        WSACleanup();
    #endif
    
    return 0;
}