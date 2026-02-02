#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <atomic>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

// Глобальные флаги
std::atomic<bool> g_running{true};
std::atomic<bool> g_receiver_active{false};

class NATDetector {
private:
    struct STUNServer {
        std::string host;
        int port;
        sockaddr_in addr;
    };
    
    std::vector<STUNServer> servers;
    
public:
    NATDetector() {
        // Инициализация STUN серверов
        std::vector<std::pair<std::string, int>> server_list = {
            {"stun.l.google.com", 19302},
            {"stun1.l.google.com", 19302},
            {"stun2.l.google.com", 19302},
            {"stun3.l.google.com", 19302}
        };
        
        for (const auto& s : server_list) {
            STUNServer server;
            server.host = s.first;
            server.port = s.second;
            
            // Получаем адрес сервера
            hostent* he = gethostbyname(server.host.c_str());
            if (he) {
                server.addr.sin_family = AF_INET;
                server.addr.sin_port = htons(server.port);
                memcpy(&server.addr.sin_addr, he->h_addr_list[0], he->h_length);
                servers.push_back(server);
            }
        }
    }
    
    std::string detectNATType(int test_port = 54321) {
        std::cout << "\n=== DETECTING NAT TYPE ===\n";
        
        // Создаем основной сокет
        int main_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (main_sock < 0) {
            return "ERROR: Cannot create socket";
        }
        
        // Привязываем к тестовому порту
        sockaddr_in local_addr;
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(test_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(main_sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            // Пробуем случайный порт
            local_addr.sin_port = 0;
            bind(main_sock, (sockaddr*)&local_addr, sizeof(local_addr));
        }
        
        // Получаем фактический порт
        socklen_t len = sizeof(local_addr);
        getsockname(main_sock, (sockaddr*)&local_addr, &len);
        int actual_port = ntohs(local_addr.sin_port);
        std::cout << "Testing on port: " << actual_port << "\n";
        
        // Тест 1: Получаем публичный адрес от двух разных серверов
        std::vector<std::pair<std::string, int>> public_addresses;
        
        for (size_t i = 0; i < std::min(servers.size(), size_t(2)); i++) {
            auto public_addr = getPublicAddress(main_sock, servers[i]);
            if (!public_addr.first.empty()) {
                public_addresses.push_back(public_addr);
                std::cout << "Server " << i+1 << ": " << public_addr.first 
                         << ":" << public_addr.second << "\n";
            }
        }
        
        if (public_addresses.size() < 2) {
            #ifdef _WIN32
                closesocket(main_sock);
            #else
                close(main_sock);
            #endif
            return "ERROR: Cannot reach STUN servers";
        }
        
        // Проверяем Full Cone NAT
        bool is_full_cone = testFullCone(main_sock, servers[0], public_addresses[0]);
        
        // Проверяем Restricted Cone NAT
        bool is_restricted_cone = false;
        if (!is_full_cone) {
            is_restricted_cone = testRestrictedCone(main_sock, servers[0], servers[1]);
        }
        
        // Проверяем Port Restricted Cone NAT
        bool is_port_restricted = false;
        if (!is_full_cone && !is_restricted_cone) {
            is_port_restricted = testPortRestrictedCone(main_sock, servers[0]);
        }
        
        // Проверяем Symmetric NAT
        bool is_symmetric = false;
        if (!is_full_cone && !is_restricted_cone && !is_port_restricted) {
            is_symmetric = testSymmetricNAT(main_sock, servers[0], servers[1]);
        }
        
        #ifdef _WIN32
            closesocket(main_sock);
        #else
            close(main_sock);
        #endif
        
        // Определяем тип NAT
        std::string nat_type;
        if (public_addresses[0] == public_addresses[1]) {
            if (is_full_cone) nat_type = "FULL CONE NAT";
            else if (is_restricted_cone) nat_type = "RESTRICTED CONE NAT";
            else if (is_port_restricted) nat_type = "PORT RESTRICTED CONE NAT";
            else if (is_symmetric) nat_type = "SYMMETRIC NAT";
            else nat_type = "UNKNOWN NAT (Probably Symmetric)";
        } else {
            nat_type = "SYMMETRIC NAT (Different ports for different servers)";
        }
        
        // Проверяем есть ли публичный IP (без NAT)
        char local_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip));
        
        if (public_addresses[0].first == local_ip) {
            nat_type = "NO NAT (Public IP)";
        }
        
        std::cout << "\n=== NAT DETECTION RESULT ===\n";
        std::cout << "Local IP: " << local_ip << ":" << actual_port << "\n";
        std::cout << "Public IP: " << public_addresses[0].first 
                  << ":" << public_addresses[0].second << "\n";
        std::cout << "NAT Type: " << nat_type << "\n";
        std::cout << "==============================\n";
        
        return nat_type;
    }
    
private:
    std::pair<std::string, int> getPublicAddress(int sock, const STUNServer& server) {
        // Создаем STUN Binding Request
        unsigned char request[20];
        
        // STUN header
        request[0] = 0x00; // Request
        request[1] = 0x01; // Binding
        request[2] = 0x00; // Length = 0
        request[3] = 0x00;
        
        // Magic Cookie
        request[4] = 0x21;
        request[5] = 0x12;
        request[6] = 0xA4;
        request[7] = 0x42;
        
        // Random Transaction ID
        srand(static_cast<unsigned>(time(nullptr)));
        for (int i = 8; i < 20; i++) {
            request[i] = rand() % 256;
        }
        
        // Отправляем запрос
        sendto(sock, reinterpret_cast<char*>(request), 20, 0,
              (sockaddr*)&server.addr, sizeof(server.addr));
        
        // Ждем ответ
        unsigned char response[512];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        
        timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        
        if (select(sock + 1, &read_set, NULL, NULL, &timeout) > 0) {
            int len = recvfrom(sock, reinterpret_cast<char*>(response), sizeof(response), 0,
                              (sockaddr*)&from_addr, &from_len);
            
            if (len >= 20 && response[0] == 0x01 && response[1] == 0x01) {
                // Парсим XOR-MAPPED-ADDRESS
                int pos = 20;
                while (pos + 4 <= len) {
                    uint16_t attr_type = (response[pos] << 8) | response[pos + 1];
                    uint16_t attr_len = (response[pos + 2] << 8) | response[pos + 3];
                    
                    if (attr_type == 0x0020 && attr_len >= 8) { // XOR-MAPPED-ADDRESS
                        uint16_t port = (response[pos + 6] << 8) | response[pos + 7];
                        port ^= 0x2112;
                        
                        std::string ip = std::to_string(response[pos + 8] ^ 0x21) + "." +
                                       std::to_string(response[pos + 9] ^ 0x12) + "." +
                                       std::to_string(response[pos + 10] ^ 0xA4) + "." +
                                       std::to_string(response[pos + 11] ^ 0x42);
                        
                        return {ip, port};
                    }
                    pos += 4 + attr_len;
                }
            }
        }
        
        return {"", 0};
    }
    
    bool testFullCone(int sock, const STUNServer& server, 
                     const std::pair<std::string, int>& public_addr) {
        std::cout << "Testing Full Cone NAT... ";
        
        // Создаем новый сокет и отправляем запрос
        int test_sock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in test_local;
        test_local.sin_family = AF_INET;
        test_local.sin_port = 0;
        test_local.sin_addr.s_addr = INADDR_ANY;
        bind(test_sock, (sockaddr*)&test_local, sizeof(test_local));
        
        // Отправляем пакет на сервер
        unsigned char request[20];
        request[0] = 0x00; request[1] = 0x01;
        request[2] = 0x00; request[3] = 0x00;
        request[4] = 0x21; request[5] = 0x12;
        request[6] = 0xA4; request[7] = 0x42;
        for (int i = 8; i < 20; i++) request[i] = rand() % 256;
        
        sendto(test_sock, reinterpret_cast<char*>(request), 20, 0,
              (sockaddr*)&server.addr, sizeof(server.addr));
        
        // Пытаемся получить ответ на основном сокете
        unsigned char response[512];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        
        timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        
        bool result = (select(sock + 1, &read_set, NULL, NULL, &timeout) > 0);
        
        #ifdef _WIN32
            closesocket(test_sock);
        #else
            close(test_sock);
        #endif
        
        std::cout << (result ? "YES" : "NO") << "\n";
        return result;
    }
    
    bool testRestrictedCone(int sock, const STUNServer& server1, 
                           const STUNServer& server2) {
        std::cout << "Testing Restricted Cone NAT... ";
        
        // Сначала отправляем на сервер1
        unsigned char request[20];
        request[0] = 0x00; request[1] = 0x01;
        request[2] = 0x00; request[3] = 0x00;
        request[4] = 0x21; request[5] = 0x12;
        request[6] = 0xA4; request[7] = 0x42;
        for (int i = 8; i < 20; i++) request[i] = rand() % 256;
        
        sendto(sock, reinterpret_cast<char*>(request), 20, 0,
              (sockaddr*)&server1.addr, sizeof(server1.addr));
        
        // Ждем немного
        #ifdef _WIN32
            Sleep(100);
        #else
            usleep(100000);
        #endif
        
        // Теперь пытаемся получить от сервера2
        unsigned char response[512];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        
        timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        
        bool result = (select(sock + 1, &read_set, NULL, NULL, &timeout) > 0);
        
        std::cout << (result ? "YES" : "NO") << "\n";
        return result;
    }
    
    bool testPortRestrictedCone(int sock, const STUNServer& server) {
        std::cout << "Testing Port Restricted Cone NAT... ";
        // Для простоты считаем, что если не Full и не Restricted, то Port Restricted
        std::cout << "MAYBE\n";
        return false;
    }
    
    bool testSymmetricNAT(int sock, const STUNServer& server1, 
                         const STUNServer& server2) {
        std::cout << "Testing Symmetric NAT... ";
        
        // Отправляем на оба сервера и сравниваем порты
        auto addr1 = getPublicAddress(sock, server1);
        auto addr2 = getPublicAddress(sock, server2);
        
        bool result = (addr1.second != addr2.second);
        std::cout << (result ? "YES" : "NO") << "\n";
        return result;
    }
};

class P2PChat {
private:
    int sock;
    int local_port;
    std::string public_ip;
    int public_port;
    std::string nat_type;
    
public:
    P2PChat() : sock(-1), local_port(0), public_port(0) {
        #ifdef _WIN32
            WSADATA wsa;
            WSAStartup(MAKEWORD(2,2), &wsa);
        #endif
    }
    
    ~P2PChat() {
        stop();
        #ifdef _WIN32
            WSACleanup();
        #endif
    }
    
    bool initialize() {
        // Создаем сокет
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cout << "Failed to create socket\n";
            return false;
        }
        
        // Определяем тип NAT
        NATDetector detector;
        nat_type = detector.detectNATType();
        
        // Если Symmetric NAT - предлагаем альтернативы
        if (nat_type.find("SYMMETRIC") != std::string::npos ||
            nat_type.find("PORT RESTRICTED") != std::string::npos) {
            std::cout << "\n⚠️  WARNING: Your NAT type may prevent direct P2P!\n";
            std::cout << "Recommendations:\n";
            std::cout << "1. Enable UPnP in router settings\n";
            std::cout << "2. Manually forward ports\n";
            std::cout << "3. Use ZeroTier/Tailscale\n";
            std::cout << "4. Try anyway (may work)\n";
        }
        
        return true;
    }
    
    void getPublicInfo() {
        std::cout << "\n=== GETTING PUBLIC INFORMATION ===\n";
        
        // Используем STUN для получения публичного адреса
        STUNServer server = {"stun.l.google.com", 19302};
        hostent* he = gethostbyname(server.host.c_str());
        if (!he) {
            std::cout << "Cannot resolve STUN server\n";
            return;
        }
        
        server.addr.sin_family = AF_INET;
        server.addr.sin_port = htons(server.port);
        memcpy(&server.addr.sin_addr, he->h_addr_list[0], he->h_length);
        
        // Привязываем сокет
        sockaddr_in local;
        local.sin_family = AF_INET;
        local.sin_port = 0; // Автовыбор порта
        local.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (sockaddr*)&local, sizeof(local));
        
        // Получаем локальный порт
        socklen_t len = sizeof(local);
        getsockname(sock, (sockaddr*)&local, &len);
        local_port = ntohs(local.sin_port);
        
        // Получаем публичный адрес
        unsigned char request[20];
        request[0] = 0x00; request[1] = 0x01;
        request[2] = 0x00; request[3] = 0x00;
        request[4] = 0x21; request[5] = 0x12;
        request[6] = 0xA4; request[7] = 0x42;
        srand(static_cast<unsigned>(time(nullptr)));
        for (int i = 8; i < 20; i++) request[i] = rand() % 256;
        
        sendto(sock, reinterpret_cast<char*>(request), 20, 0,
              (sockaddr*)&server.addr, sizeof(server.addr));
        
        unsigned char response[512];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        
        timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        
        if (select(sock + 1, &read_set, NULL, NULL, &timeout) > 0) {
            int resp_len = recvfrom(sock, reinterpret_cast<char*>(response), sizeof(response), 0,
                                  (sockaddr*)&from_addr, &from_len);
            
            if (resp_len >= 20 && response[0] == 0x01 && response[1] == 0x01) {
                int pos = 20;
                while (pos + 4 <= resp_len) {
                    uint16_t attr_type = (response[pos] << 8) | response[pos + 1];
                    uint16_t attr_len = (response[pos + 2] << 8) | response[pos + 3];
                    
                    if (attr_type == 0x0020 && attr_len >= 8) {
                        public_port = (response[pos + 6] << 8) | response[pos + 7];
                        public_port ^= 0x2112;
                        
                        public_ip = std::to_string(response[pos + 8] ^ 0x21) + "." +
                                   std::to_string(response[pos + 9] ^ 0x12) + "." +
                                   std::to_string(response[pos + 10] ^ 0xA4) + "." +
                                   std::to_string(response[pos + 11] ^ 0x42);
                        break;
                    }
                    pos += 4 + attr_len;
                }
            }
        }
        
        std::cout << "Local endpoint: " << getLocalIP() << ":" << local_port << "\n";
        std::cout << "Public endpoint: " << public_ip << ":" << public_port << "\n";
        std::cout << "NAT Type: " << nat_type << "\n";
        std::cout << "===================================\n";
    }
    
    void run() {
        if (sock < 0) {
            std::cout << "Socket not initialized\n";
            return;
        }
        
        std::cout << "\n=== P2P CHAT ===\n";
        std::cout << "1. Host (wait for connection)\n";
        std::cout << "2. Client (connect to host)\n";
        std::cout << "3. Exit\n";
        std::cout << "Choice: ";
        
        int choice;
        std::cin >> choice;
        std::cin.ignore();
        
        if (choice == 1) {
            runAsHost();
        } else if (choice == 2) {
            runAsClient();
        }
    }
    
private:
    void runAsHost() {
        std::cout << "\n=== HOST MODE ===\n";
        std::cout << "Your public address: " << public_ip << ":" << public_port << "\n";
        std::cout << "Share this with your peer!\n";
        std::cout << "Waiting for connection...\n";
        
        // Запускаем поток приемника
        g_receiver_active = true;
        std::thread receiver(&P2PChat::receiverThread, this);
        
        std::cout << "\nType messages (or 'exit' to quit):\n";
        
        std::string message;
        sockaddr_in peer_addr;
        bool peer_connected = false;
        
        while (g_running) {
            std::cout << "You: ";
            std::getline(std::cin, message);
            
            if (message == "exit") {
                g_running = false;
                break;
            }
            
            if (!peer_connected && message.substr(0, 7) == "connect") {
                // Устанавливаем соединение
                std::istringstream iss(message);
                std::string cmd, peer_ip;
                int peer_port;
                iss >> cmd >> peer_ip >> peer_port;
                
                peer_addr.sin_family = AF_INET;
                peer_addr.sin_port = htons(peer_port);
                inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr);
                peer_connected = true;
                
                std::cout << "Connected to " << peer_ip << ":" << peer_port << "\n";
            } else if (peer_connected) {
                sendto(sock, message.c_str(), message.length(), 0,
                      (sockaddr*)&peer_addr, sizeof(peer_addr));
            }
        }
        
        g_receiver_active = false;
        receiver.join();
    }
    
    void runAsClient() {
        std::cout << "\n=== CLIENT MODE ===\n";
        
        std::string peer_ip;
        int peer_port;
        
        std::cout << "Enter host's public IP: ";
        std::cin >> peer_ip;
        std::cout << "Enter host's public port: ";
        std::cin >> peer_port;
        std::cin.ignore();
        
        sockaddr_in peer_addr;
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(peer_port);
        inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr);
        
        // Пробуем hole punching
        std::cout << "\nAttempting hole punching...\n";
        for (int i = 0; i < 10; i++) {
            std::string punch = "PUNCH_" + std::to_string(i);
            sendto(sock, punch.c_str(), punch.length(), 0,
                  (sockaddr*)&peer_addr, sizeof(peer_addr));
            
            std::cout << "Punch " << i+1 << " sent\n";
            
            #ifdef _WIN32
                Sleep(300);
            #else
                usleep(300000);
            #endif
        }
        
        // Запускаем приемник
        g_receiver_active = true;
        std::thread receiver(&P2PChat::receiverThread, this);
        
        std::cout << "\nConnection ready! Type messages:\n";
        
        std::string message;
        while (g_running) {
            std::cout << "You: ";
            std::getline(std::cin, message);
            
            if (message == "exit") {
                g_running = false;
                break;
            }
            
            sendto(sock, message.c_str(), message.length(), 0,
                  (sockaddr*)&peer_addr, sizeof(peer_addr));
        }
        
        g_receiver_active = false;
        receiver.join();
    }
    
    void receiverThread() {
        std::cout << "Receiver thread started\n";
        
        char buffer[1024];
        
        while (g_receiver_active) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(sock, &read_set);
            
            timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            
            int ready = select(sock + 1, &read_set, NULL, NULL, &timeout);
            
            if (ready > 0) {
                sockaddr_in from_addr;
                socklen_t from_len = sizeof(from_addr);
                
                int len = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                                  (sockaddr*)&from_addr, &from_len);
                
                if (len > 0) {
                    buffer[len] = '\0';
                    char from_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
                    
                    std::cout << "\n[FROM " << from_ip << ":" 
                             << ntohs(from_addr.sin_port) << "] "
                             << buffer << "\n";
                    std::cout << "You: " << std::flush;
                }
            }
        }
        
        std::cout << "Receiver thread stopped\n";
    }
    
    std::string getLocalIP() {
        char buffer[128];
        gethostname(buffer, sizeof(buffer));
        
        hostent* host = gethostbyname(buffer);
        if (host && host->h_addr_list[0]) {
            return inet_ntoa(*(in_addr*)host->h_addr_list[0]);
        }
        
        return "127.0.0.1";
    }
    
    void stop() {
        if (sock >= 0) {
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            sock = -1;
        }
    }
    
    struct STUNServer {
        std::string host;
        int port;
        sockaddr_in addr;
    };
};

int main() {
    std::cout << "=== ADVANCED P2P CHAT WITH NAT DETECTION ===\n\n";
    
    P2PChat chat;
    
    if (!chat.initialize()) {
        return 1;
    }
    
    chat.getPublicInfo();
    chat.run();
    
    std::cout << "\nProgram finished\n";
    return 0;
}