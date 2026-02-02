#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <cstdlib>
#include <ctime>

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
#endif

class P2PHolePunch {
private:
    std::vector<std::string> stun_servers = {
        "stun.l.google.com:19302",
        "stun1.l.google.com:19302",
        "stun2.l.google.com:19302",
        "stun3.l.google.com:19302"
    };
    
public:
    struct PublicAddress {
        std::string ip;
        int port;
    };
    
    PublicAddress getPublicAddress(int local_port) {
        std::cout << "Querying STUN servers...\n";
        
        for (const auto& server : stun_servers) {
            size_t colon = server.find(':');
            std::string host = server.substr(0, colon);
            int port = std::stoi(server.substr(colon + 1));
            
            std::cout << "Trying " << host << ":" << port << "...\n";
            PublicAddress result = querySTUN(host, port, local_port);
            if (!result.ip.empty()) {
                std::cout << "Success!\n";
                return result;
            }
        }
        
        std::cout << "Failed to get public address from all STUN servers\n";
        return {"", 0};
    }
    
    void startHolePunch(const std::string& peer_ip, int peer_port, 
                        const std::string& my_public_ip, int my_public_port) {
        std::cout << "\n=== HOLE PUNCHING START ===\n";
        std::cout << "Target peer: " << peer_ip << ":" << peer_port << "\n";
        std::cout << "My public: " << my_public_ip << ":" << my_public_port << "\n";
        std::cout << "============================\n\n";
        
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cout << "Failed to create socket\n";
            return;
        }
        
        // Привязываем к публичному порту
        sockaddr_in local_addr;
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(my_public_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            std::cout << "Warning: Cannot bind to port " << my_public_port 
                      << ", using random port\n";
            local_addr.sin_port = 0;
            bind(sock, (sockaddr*)&local_addr, sizeof(local_addr));
            
            socklen_t len = sizeof(local_addr);
            getsockname(sock, (sockaddr*)&local_addr, &len);
            std::cout << "Using port: " << ntohs(local_addr.sin_port) << "\n";
        }
        
        // Целевой адрес пира
        sockaddr_in peer_addr;
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(peer_port);
        inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr);
        
        // Флаг для потока приема
        bool receiver_running = true;
        
        // Поток для приема сообщений
        std::thread receiver_thread([&]() {
            std::cout << "Receiver thread started\n";
            
            char buffer[1024];
            sockaddr_in from_addr;
            
            #ifdef _WIN32
                int from_len = sizeof(from_addr);
            #else
                socklen_t from_len = sizeof(from_addr);
            #endif
            
            while (receiver_running) {
                // Проверяем есть ли данные (неблокирующе)
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(sock, &read_fds);
                
                timeval timeout;
                timeout.tv_sec = 1;  // Проверяем каждую секунду
                timeout.tv_usec = 0;
                
                int ready = select(sock + 1, &read_fds, NULL, NULL, &timeout);
                
                if (ready > 0) {
                    int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                      (sockaddr*)&from_addr, &from_len);
                    
                    if (len > 0) {
                        buffer[len] = '\0';
                        char from_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
                        
                        std::cout << "\n[FROM " << from_ip << ":" 
                                 << ntohs(from_addr.sin_port) << "] "
                                 << buffer << "\n";
                    }
                }
            }
            
            std::cout << "Receiver thread stopped\n";
        });
        
        // Основной цикл hole punching
        std::cout << "\n=== PUNCHING PHASE ===\n";
        std::cout << "Sending 10 punch packets...\n";
        
        for (int i = 1; i <= 10; i++) {
            std::string msg = "PUNCH_" + std::to_string(i) + "_FROM_" + my_public_ip;
            int sent = sendto(sock, msg.c_str(), msg.length(), 0,
                            (sockaddr*)&peer_addr, sizeof(peer_addr));
            
            if (sent > 0) {
                std::cout << "Punch #" << i << " sent (" << sent << " bytes)\n";
            } else {
                std::cout << "Failed to send punch #" << i << "\n";
            }
            
            // Ждем немного между пакетами
            #ifdef _WIN32
                Sleep(300);
            #else
                usleep(300000);
            #endif
        }
        
        std::cout << "\n=== READY TO CHAT ===\n";
        std::cout << "Type messages to send (type 'exit' to quit):\n";
        
        // Основной цикл чата
        std::string message;
        std::cin.ignore(); // Очищаем буфер ввода
        
        while (true) {
            std::cout << "You: ";
            std::getline(std::cin, message);
            
            if (message == "exit") {
                break;
            }
            
            // Отправляем сообщение
            int sent = sendto(sock, message.c_str(), message.length(), 0,
                            (sockaddr*)&peer_addr, sizeof(peer_addr));
            
            if (sent > 0) {
                std::cout << "Message sent (" << sent << " bytes)\n";
            } else {
                std::cout << "Failed to send message\n";
            }
        }
        
        // Останавливаем поток приемника
        receiver_running = false;
        receiver_thread.join();
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
        
        std::cout << "Connection closed\n";
    }
    
private:
    PublicAddress querySTUN(const std::string& host, int port, int local_port) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            return {"", 0};
        }
        
        // Привязываем сокет
        sockaddr_in local_addr;
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(local_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            return {"", 0};
        }
        
        // Создаем STUN запрос
        unsigned char stun_request[20];
        
        // Заголовок запроса
        stun_request[0] = 0x00; // Request
        stun_request[1] = 0x01; // Binding
        stun_request[2] = 0x00; // Length = 0
        stun_request[3] = 0x00;
        
        // Magic Cookie
        stun_request[4] = 0x21;
        stun_request[5] = 0x12;
        stun_request[6] = 0xA4;
        stun_request[7] = 0x42;
        
        // Transaction ID (случайный)
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        for (int i = 8; i < 20; i++) {
            stun_request[i] = std::rand() % 256;
        }
        
        // Отправляем запрос
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
        // Разрешаем доменное имя
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
        
        if (sendto(sock, reinterpret_cast<char*>(stun_request), 20, 0,
                  (sockaddr*)&server_addr, sizeof(server_addr)) <= 0) {
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            return {"", 0};
        }
        
        // Ждем ответ (3 секунды)
        unsigned char response[512];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        
        timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        
        int ready = select(sock + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready > 0) {
            int len = recvfrom(sock, reinterpret_cast<char*>(response), 
                              sizeof(response), 0,
                              (sockaddr*)&from_addr, &from_len);
            
            if (len >= 20) {
                // Проверяем что это успешный STUN ответ
                if (response[0] == 0x01 && response[1] == 0x01) {
                    // Ищем MAPPED-ADDRESS или XOR-MAPPED-ADDRESS
                    int pos = 20;
                    
                    while (pos + 4 <= len) {
                        uint16_t attr_type = (response[pos] << 8) | response[pos + 1];
                        uint16_t attr_length = (response[pos + 2] << 8) | response[pos + 3];
                        
                        if (attr_type == 0x0001) { // MAPPED-ADDRESS
                            if (pos + 4 + attr_length <= len && attr_length >= 8) {
                                uint16_t port = (response[pos + 6] << 8) | response[pos + 7];
                                std::string ip = std::to_string(response[pos + 8]) + "." +
                                               std::to_string(response[pos + 9]) + "." +
                                               std::to_string(response[pos + 10]) + "." +
                                               std::to_string(response[pos + 11]);
                                
                                #ifdef _WIN32
                                    closesocket(sock);
                                #else
                                    close(sock);
                                #endif
                                
                                return {ip, port};
                            }
                        }
                        else if (attr_type == 0x0020) { // XOR-MAPPED-ADDRESS
                            if (pos + 4 + attr_length <= len && attr_length >= 8) {
                                uint16_t port = (response[pos + 6] << 8) | response[pos + 7];
                                port ^= 0x2112; // XOR с первыми 2 байтами Magic Cookie
                                
                                std::string ip = std::to_string(response[pos + 8] ^ 0x21) + "." +
                                               std::to_string(response[pos + 9] ^ 0x12) + "." +
                                               std::to_string(response[pos + 10] ^ 0xA4) + "." +
                                               std::to_string(response[pos + 11] ^ 0x42);
                                
                                #ifdef _WIN32
                                    closesocket(sock);
                                #else
                                    close(sock);
                                #endif
                                
                                return {ip, port};
                            }
                        }
                        
                        pos += 4 + attr_length;
                        // Выравнивание до 4 байт
                        if (attr_length % 4 != 0) {
                            pos += 4 - (attr_length % 4);
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

void printUsage() {
    std::cout << "\n=== P2P HOLE PUNCHING TOOL ===\n";
    std::cout << "\nUsage:\n";
    std::cout << "1. Run the program\n";
    std::cout << "2. Get your public IP:Port from STUN\n";
    std::cout << "3. Share with your peer\n";
    std::cout << "4. Enter peer's IP:Port\n";
    std::cout << "5. Start hole punching\n";
    std::cout << "\nNote: Works best with Full Cone or Restricted Cone NAT\n";
    std::cout << "May not work with Symmetric NAT\n";
    std::cout << "================================\n\n";
}

int main() {
    // Инициализация Winsock для Windows
    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cout << "Failed to initialize Winsock\n";
            return 1;
        }
    #endif
    
    printUsage();
    
    // Создаем экземпляр P2P
    P2PHolePunch p2p;
    
    // Шаг 1: Получаем наш публичный адрес
    std::cout << "Step 1: Getting your public address via STUN...\n";
    auto my_public = p2p.getPublicAddress(0); // 0 = любой свободный порт
    
    if (my_public.ip.empty() || my_public.port == 0) {
        std::cout << "\nERROR: Cannot determine public address!\n";
        std::cout << "Possible reasons:\n";
        std::cout << "1. You have Symmetric NAT (common with mobile/corporate networks)\n";
        std::cout << "2. Firewall is blocking STUN requests\n";
        std::cout << "3. No internet connection\n";
        std::cout << "\nTry these alternatives:\n";
        std::cout << "1. Use ZeroTier or Tailscale for virtual LAN\n";
        std::cout << "2. Manually forward ports in your router\n";
        std::cout << "3. Use a public relay server\n";
        
        #ifdef _WIN32
            WSACleanup();
        #endif
        return 1;
    }
    
    std::cout << "\nSUCCESS! Your public address is:\n";
    std::cout << "================================\n";
    std::cout << "PUBLIC IP:  " << my_public.ip << "\n";
    std::cout << "PUBLIC PORT: " << my_public.port << "\n";
    std::cout << "================================\n";
    std::cout << "\nShare this information with your peer!\n";
    
    // Шаг 2: Получаем данные пира
    std::cout << "\nStep 2: Enter peer's information\n";
    
    std::string peer_ip;
    int peer_port;
    
    std::cout << "Peer's PUBLIC IP: ";
    std::cin >> peer_ip;
    
    std::cout << "Peer's PUBLIC PORT: ";
    std::cin >> peer_port;
    
    // Проверяем ввод
    if (peer_ip.empty() || peer_port <= 0 || peer_port > 65535) {
        std::cout << "Invalid peer information\n";
        #ifdef _WIN32
            WSACleanup();
        #endif
        return 1;
    }
    
    // Шаг 3: Запускаем hole punching
    std::cout << "\nStep 3: Starting hole punching...\n";
    p2p.startHolePunch(peer_ip, peer_port, my_public.ip, my_public.port);
    
    // Завершение
    #ifdef _WIN32
        WSACleanup();
    #endif
    
    std::cout << "\nProgram finished\n";
    return 0;
}