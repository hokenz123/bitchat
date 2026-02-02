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
#include <condition_variable>
#include <mutex>

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
std::atomic<bool> g_connection_established{false};
std::mutex g_cout_mutex;

class P2PChat {
private:
    int sock;
    int local_port;
    std::string public_ip;
    int public_port;
    std::string nat_type;
    sockaddr_in peer_addr;
    bool peer_configured;
    
    // Синхронизация
    std::mutex mtx;
    std::condition_variable cv;
    bool ready_for_punching;
    
public:
    P2PChat() : sock(-1), local_port(0), public_port(0), 
                peer_configured(false), ready_for_punching(false) {
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
        
        // Устанавливаем неблокирующий режим (для Windows)
        #ifdef _WIN32
            u_long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);
        #endif
        
        return true;
    }
    
    void getPublicInfo() {
        std::cout << "\n=== GETTING PUBLIC INFORMATION ===\n";
        
        // Используем STUN для получения публичного адреса
        const char* stun_host = "stun.l.google.com";
        int stun_port = 19302;
        
        // Получаем адрес STUN сервера
        hostent* he = gethostbyname(stun_host);
        if (!he) {
            std::cout << "Cannot resolve STUN server\n";
            return;
        }
        
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(stun_port);
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
        
        // Привязываем сокет к любому порту
        sockaddr_in local;
        local.sin_family = AF_INET;
        local.sin_port = 0; // Автовыбор порта
        local.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (sockaddr*)&local, sizeof(local)) < 0) {
            std::cout << "Bind failed\n";
            return;
        }
        
        // Получаем локальный порт
        socklen_t len = sizeof(local);
        getsockname(sock, (sockaddr*)&local, &len);
        local_port = ntohs(local.sin_port);
        
        // Получаем локальный IP
        char local_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &local.sin_addr, local_ip_str, sizeof(local_ip_str));
        
        // Получаем публичный адрес через STUN
        unsigned char request[20];
        request[0] = 0x00; request[1] = 0x01;
        request[2] = 0x00; request[3] = 0x00;
        request[4] = 0x21; request[5] = 0x12;
        request[6] = 0xA4; request[7] = 0x42;
        
        srand(static_cast<unsigned>(time(nullptr)));
        for (int i = 8; i < 20; i++) {
            request[i] = rand() % 256;
        }
        
        // Отправляем STUN запрос
        sendto(sock, reinterpret_cast<char*>(request), 20, 0,
              (sockaddr*)&server_addr, sizeof(server_addr));
        
        // Ждем ответ
        unsigned char response[512];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        // Даем несколько попыток
        for (int attempt = 0; attempt < 3; attempt++) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(sock, &read_set);
            
            timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            
            if (select(sock + 1, &read_set, NULL, NULL, &timeout) > 0) {
                int resp_len = recvfrom(sock, reinterpret_cast<char*>(response), 
                                       sizeof(response), 0,
                                       (sockaddr*)&from_addr, &from_len);
                
                if (resp_len >= 20 && response[0] == 0x01 && response[1] == 0x01) {
                    // Парсим XOR-MAPPED-ADDRESS
                    int pos = 20;
                    while (pos + 4 <= resp_len) {
                        uint16_t attr_type = (response[pos] << 8) | response[pos + 1];
                        uint16_t attr_len = (response[pos + 2] << 8) | response[pos + 3];
                        
                        if (attr_type == 0x0020 && attr_len >= 8) { // XOR-MAPPED-ADDRESS
                            public_port = (response[pos + 6] << 8) | response[pos + 7];
                            public_port ^= 0x2112;
                            
                            public_ip = std::to_string(response[pos + 8] ^ 0x21) + "." +
                                       std::to_string(response[pos + 9] ^ 0x12) + "." +
                                       std::to_string(response[pos + 10] ^ 0xA4) + "." +
                                       std::to_string(response[pos + 11] ^ 0x42);
                            
                            std::cout << "Local endpoint: " << local_ip_str << ":" << local_port << "\n";
                            std::cout << "Public endpoint: " << public_ip << ":" << public_port << "\n";
                            std::cout << "NAT Type: RESTRICTED CONE NAT (based on your info)\n";
                            std::cout << "===================================\n";
                            return;
                        }
                        pos += 4 + attr_len;
                    }
                }
            }
            
            // Повторяем запрос если не получили ответ
            if (attempt < 2) {
                for (int i = 8; i < 20; i++) request[i] = rand() % 256;
                sendto(sock, reinterpret_cast<char*>(request), 20, 0,
                      (sockaddr*)&server_addr, sizeof(server_addr));
            }
        }
        
        std::cout << "Failed to get public address\n";
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
        std::cout << "Share this with your peer!\n\n";
        
        // Ждем когда клиент будет готов
        std::cout << "When your peer is ready, type 'ready' and press Enter: ";
        std::string input;
        std::getline(std::cin, input);
        
        if (input != "ready") {
            std::cout << "Aborted\n";
            return;
        }
        
        // Получаем данные клиента
        std::string client_ip;
        int client_port;
        
        std::cout << "Enter client's PUBLIC IP: ";
        std::getline(std::cin, client_ip);
        
        std::cout << "Enter client's PUBLIC PORT: ";
        std::cin >> client_port;
        std::cin.ignore();
        
        // Настраиваем адрес пира
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(client_port);
        inet_pton(AF_INET, client_ip.c_str(), &peer_addr.sin_addr);
        peer_configured = true;
        
        std::cout << "\nTarget set: " << client_ip << ":" << client_port << "\n";
        
        // Запускаем поток приемника
        g_receiver_active = true;
        std::thread receiver(&P2PChat::receiverThread, this);
        
        // Даем время потоку запуститься
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        std::cout << "\n=== STARTING HOLE PUNCHING ===\n";
        std::cout << "Both sides must punch simultaneously!\n";
        std::cout << "Starting in 3 seconds...\n";
        
        for (int i = 3; i > 0; i--) {
            std::cout << i << "...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        std::cout << "GO! Punching...\n";
        
        // Отправляем 15 пакетов для hole punching
        for (int i = 0; i < 15; i++) {
            std::string punch_msg = "PUNCH_" + std::to_string(i) + "_HOST";
            sendto(sock, punch_msg.c_str(), punch_msg.length(), 0,
                  (sockaddr*)&peer_addr, sizeof(peer_addr));
            
            {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                std::cout << "Sent punch #" << i << "\n";
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        
        std::cout << "\n✅ Hole punching completed!\n";
        std::cout << "You should now be able to exchange messages.\n";
        std::cout << "Type 'exit' to quit.\n\n";
        
        // Основной цикл чата
        chatLoop();
        
        g_receiver_active = false;
        receiver.join();
    }
    
    void runAsClient() {
        std::cout << "\n=== CLIENT MODE ===\n";
        
        // Получаем данные хоста
        std::string host_ip;
        int host_port;
        
        std::cout << "Enter host's PUBLIC IP: ";
        std::getline(std::cin, host_ip);
        
        std::cout << "Enter host's PUBLIC PORT: ";
        std::cin >> host_port;
        std::cin.ignore();
        
        // Настраиваем адрес пира
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(host_port);
        inet_pton(AF_INET, host_ip.c_str(), &peer_addr.sin_addr);
        peer_configured = true;
        
        std::cout << "\nTarget set: " << host_ip << ":" << host_port << "\n";
        
        // Ждем сигнал от хоста
        std::cout << "\nTell the host you're ready.\n";
        std::cout << "When host says 'ready', type 'ready' and press Enter: ";
        
        std::string input;
        std::getline(std::cin, input);
        
        if (input != "ready") {
            std::cout << "Aborted\n";
            return;
        }
        
        // Запускаем поток приемника
        g_receiver_active = true;
        std::thread receiver(&P2PChat::receiverThread, this);
        
        // Даем время потоку запуститься
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        std::cout << "\n=== STARTING HOLE PUNCHING ===\n";
        std::cout << "Both sides must punch simultaneously!\n";
        std::cout << "Starting in 3 seconds...\n";
        
        for (int i = 3; i > 0; i--) {
            std::cout << i << "...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        std::cout << "GO! Punching...\n";
        
        // Отправляем 15 пакетов для hole punching
        for (int i = 0; i < 15; i++) {
            std::string punch_msg = "PUNCH_" + std::to_string(i) + "_CLIENT";
            sendto(sock, punch_msg.c_str(), punch_msg.length(), 0,
                  (sockaddr*)&peer_addr, sizeof(peer_addr));
            
            {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                std::cout << "Sent punch #" << i << "\n";
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        
        std::cout << "\n✅ Hole punching completed!\n";
        std::cout << "You should now be able to exchange messages.\n";
        std::cout << "Type 'exit' to quit.\n\n";
        
        // Основной цикл чата
        chatLoop();
        
        g_receiver_active = false;
        receiver.join();
    }
    
    void chatLoop() {
        std::string message;
        
        while (g_running) {
            std::cout << "You: ";
            std::getline(std::cin, message);
            
            if (message == "exit") {
                g_running = false;
                break;
            }
            
            if (peer_configured && !message.empty()) {
                int sent = sendto(sock, message.c_str(), message.length(), 0,
                                (sockaddr*)&peer_addr, sizeof(peer_addr));
                
                if (sent > 0) {
                    std::cout << "Message sent (" << sent << " bytes)\n";
                }
            }
        }
    }
    
    void receiverThread() {
        std::cout << "Receiver thread started\n";
        
        char buffer[1024];
        int packet_count = 0;
        
        while (g_receiver_active) {
            // Проверяем сокет на наличие данных
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(sock, &read_set);
            
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; // 100ms
            
            int ready = select(sock + 1, &read_set, NULL, NULL, &timeout);
            
            if (ready > 0) {
                sockaddr_in from_addr;
                socklen_t from_len = sizeof(from_addr);
                
                int len = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                                  (sockaddr*)&from_addr, &from_len);
                
                if (len > 0) {
                    buffer[len] = '\0';
                    packet_count++;
                    
                    char from_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
                    int from_port = ntohs(from_addr.sin_port);
                    
                    // Проверяем, это наш пир?
                    bool is_peer = (peer_configured && 
                                   from_addr.sin_addr.s_addr == peer_addr.sin_addr.s_addr &&
                                   from_port == ntohs(peer_addr.sin_port));
                    
                    {
                        std::lock_guard<std::mutex> lock(g_cout_mutex);
                        std::cout << "\n═══════════════════════════════════════\n";
                        
                        if (std::string(buffer).find("PUNCH_") == 0) {
                            std::cout << "[HOLE PUNCH PACKET #" << packet_count << "]\n";
                        } else {
                            std::cout << "[MESSAGE #" << packet_count << "]\n";
                        }
                        
                        std::cout << "From: " << from_ip << ":" << from_port;
                        
                        if (is_peer) {
                            std::cout << " (PEER)";
                        } else if (packet_count == 1) {
                            // Первый пакет - сохраняем как пира
                            peer_addr = from_addr;
                            peer_configured = true;
                            g_connection_established = true;
                            std::cout << " (NEW PEER - CONNECTION ESTABLISHED!)";
                        }
                        
                        std::cout << "\nContent: " << buffer << "\n";
                        std::cout << "═══════════════════════════════════════\n";
                        std::cout << "You: " << std::flush;
                    }
                    
                    // Автоответ на первое сообщение
                    if (packet_count == 1 && std::string(buffer).find("PUNCH_") != 0) {
                        std::string reply = "AUTO-REPLY: Got your message!";
                        sendto(sock, reply.c_str(), reply.length(), 0,
                              (sockaddr*)&from_addr, from_len);
                    }
                }
            }
        }
        
        std::cout << "Receiver thread stopped. Total packets: " << packet_count << "\n";
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
};

void printBanner() {
    std::cout << "╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║         RESTRICTED CONE NAT P2P CHAT v2.0            ║\n";
    std::cout << "║       Hole Punching with Synchronized Timing         ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════╝\n\n";
}

int main() {
    printBanner();
    
    std::cout << "IMPORTANT: Both users must coordinate timing!\n";
    std::cout << "1. One person chooses HOST, other chooses CLIENT\n";
    std::cout << "2. HOST shares public IP:Port\n";
    std::cout << "3. Both say 'ready' at the same time\n";
    std::cout << "4. System counts down and starts hole punching\n\n";
    
    P2PChat chat;
    
    if (!chat.initialize()) {
        return 1;
    }
    
    chat.getPublicInfo();
    chat.run();
    
    std::cout << "\nProgram finished\n";
    return 0;
}