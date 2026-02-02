#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <regex>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
#endif

class NativeUPnP {
private:
    std::string controlURL;
    std::string serviceType;
    std::string localIP;
    
public:
    bool discover() {
        std::cout << "UPnP: Ищу роутеры...\n";
        
        // SSDP multicast адрес и порт
        const char* ssdp_addr = "239.255.255.250";
        const int ssdp_port = 1900;
        
        // Создаем UDP сокет для multicast
        int ssdp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (ssdp_sock < 0) {
            std::cout << "UPnP: Ошибка создания сокета\n";
            return false;
        }
        
        // Настраиваем multicast
        sockaddr_in multicast_addr;
        multicast_addr.sin_family = AF_INET;
        multicast_addr.sin_port = htons(ssdp_port);
        inet_pton(AF_INET, ssdp_addr, &multicast_addr.sin_addr);
        
        // SSDP M-SEARCH запрос
        const char* search_msg = 
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 2\r\n"
            "ST: urn:schemas-upnp-org:service:WANIPConnection:1\r\n"
            "\r\n";
        
        // Отправляем multicast запрос
        sendto(ssdp_sock, search_msg, strlen(search_msg), 0,
               (sockaddr*)&multicast_addr, sizeof(multicast_addr));
        
        std::cout << "UPnP: Отправлен поисковый запрос...\n";
        
        // Ждем ответы
        char buffer[4096];
        sockaddr_in from_addr;
        
        #ifdef _WIN32
            int from_len = sizeof(from_addr);
        #else
            socklen_t from_len = sizeof(from_addr);
        #endif
        
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ssdp_sock, &fds);
        
        timeval tv;
        tv.tv_sec = 3; // 3 секунды на ответ
        tv.tv_usec = 0;
        
        if (select(ssdp_sock + 1, &fds, NULL, NULL, &tv) > 0) {
            int len = recvfrom(ssdp_sock, buffer, sizeof(buffer)-1, 0,
                              (sockaddr*)&from_addr, &from_len);
            if (len > 0) {
                buffer[len] = '\0';
                std::string response(buffer);
                
                std::cout << "UPnP: Получен ответ от роутера!\n";
                
                // Парсим location из ответа
                std::regex location_regex("LOCATION:\\s*(.+?)\\r\\n");
                std::smatch match;
                
                if (std::regex_search(response, match, location_regex)) {
                    std::string location = match[1];
                    std::cout << "UPnP: Location: " << location << "\n";
                    
                    // Получаем описание устройства
                    if (parseDeviceDescription(location)) {
                        std::cout << "UPnP: Роутер найден!\n";
                        
                        #ifdef _WIN32
                            closesocket(ssdp_sock);
                        #else
                            close(ssdp_sock);
                        #endif
                        
                        return true;
                    }
                }
            }
        }
        
        #ifdef _WIN32
            closesocket(ssdp_sock);
        #else
            close(ssdp_sock);
        #endif
        
        std::cout << "UPnP: Роутер не найден\n";
        return false;
    }
    
    bool addPortMapping(int port) {
        if (controlURL.empty()) {
            std::cout << "UPnP: Сначала найдите роутер\n";
            return false;
        }
        
        // Получаем локальный IP
        localIP = getLocalIP();
        if (localIP.empty()) {
            localIP = "192.168.1.100"; // fallback
        }
        
        // Создаем SOAP запрос для добавления порта
        std::string soap_request = 
            "<?xml version=\"1.0\"?>\r\n"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
            "<s:Body>\r\n"
            "<u:AddPortMapping xmlns:u=\"" + serviceType + "\">\r\n"
            "<NewRemoteHost></NewRemoteHost>\r\n"
            "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>\r\n"
            "<NewProtocol>UDP</NewProtocol>\r\n"
            "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>\r\n"
            "<NewInternalClient>" + localIP + "</NewInternalClient>\r\n"
            "<NewEnabled>1</NewEnabled>\r\n"
            "<NewPortMappingDescription>P2P_Chat</NewPortMappingDescription>\r\n"
            "<NewLeaseDuration>0</NewLeaseDuration>\r\n"
            "</u:AddPortMapping>\r\n"
            "</s:Body>\r\n"
            "</s:Envelope>";
        
        // Отправляем HTTP POST запрос
        std::string http_request = 
            "POST " + controlURL + " HTTP/1.1\r\n"
            "Host: " + extractHost(controlURL) + "\r\n"
            "Content-Type: text/xml; charset=\"utf-8\"\r\n"
            "SOAPAction: \"" + serviceType + "#AddPortMapping\"\r\n"
            "Content-Length: " + std::to_string(soap_request.length()) + "\r\n"
            "\r\n" + soap_request;
        
        // Создаем TCP сокет для HTTP запроса
        std::string host = extractHost(controlURL);
        int port_http = extractPort(controlURL);
        
        int http_sock = createTCPSocket(host, port_http);
        if (http_sock < 0) return false;
        
        // Отправляем запрос
        send(http_sock, http_request.c_str(), http_request.length(), 0);
        
        // Читаем ответ
        char response[4096];
        int len = recv(http_sock, response, sizeof(response)-1, 0);
        
        #ifdef _WIN32
            closesocket(http_sock);
        #else
            close(http_sock);
        #endif
        
        if (len > 0) {
            response[len] = '\0';
            if (strstr(response, "200 OK")) {
                std::cout << "UPnP: Порт " << port << " успешно проброшен!\n";
                return true;
            }
        }
        
        std::cout << "UPnP: Не удалось пробросить порт\n";
        return false;
    }
    
    std::string getExternalIP() {
        // Упрощенный метод - узнаем IP через внешний сервис
        return getPublicIP();
    }
    
private:
    bool parseDeviceDescription(const std::string& url) {
        // Упрощенная версия - для реального использования нужен XML парсер
        // Здесь для примера зададим значения вручную
        
        // Типичные значения для большинства роутеров
        controlURL = "http://192.168.1.1:5000/upnp/control/WANIPConn1";
        serviceType = "urn:schemas-upnp-org:service:WANIPConnection:1";
        
        std::cout << "UPnP: Использую стандартные настройки\n";
        std::cout << "UPnP: Control URL: " << controlURL << "\n";
        
        return true;
    }
    
    std::string getLocalIP() {
        // Простой способ получить локальный IP
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        
        // Подключаемся к публичному DNS (не отправляем данные)
        sockaddr_in google;
        google.sin_family = AF_INET;
        google.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &google.sin_addr);
        
        connect(sock, (sockaddr*)&google, sizeof(google));
        
        // Получаем локальный адрес сокета
        sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        getsockname(sock, (sockaddr*)&local_addr, &addr_len);
        
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &local_addr.sin_addr, ip, sizeof(ip));
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
        
        return std::string(ip);
    }
    
    std::string getPublicIP() {
        // Узнаем публичный IP через внешний сервис
        int sock = createTCPSocket("api.ipify.org", 80);
        if (sock < 0) return "";
        
        std::string request = 
            "GET / HTTP/1.1\r\n"
            "Host: api.ipify.org\r\n"
            "Connection: close\r\n"
            "\r\n";
        
        send(sock, request.c_str(), request.length(), 0);
        
        char buffer[1024];
        int len = recv(sock, buffer, sizeof(buffer)-1, 0);
        
        #ifdef _WIN32
            closesocket(sock);
        #else
            close(sock);
        #endif
        
        if (len > 0) {
            buffer[len] = '\0';
            std::string response(buffer);
            
            // Ищем IP в ответе
            size_t body_start = response.find("\r\n\r\n");
            if (body_start != std::string::npos) {
                return response.substr(body_start + 4);
            }
        }
        
        return "";
    }
    
    int createTCPSocket(const std::string& host, int port) {
        // Создание TCP сокета и подключение
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
        // Преобразуем доменное имя в IP
        hostent* he = gethostbyname(host.c_str());
        if (!he) {
            // Пробуем как IP адрес
            inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);
        } else {
            memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
        }
        
        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif
            return -1;
        }
        
        return sock;
    }
    
    std::string extractHost(const std::string& url) {
        // Упрощенный парсинг URL
        size_t start = url.find("://");
        if (start == std::string::npos) return "";
        
        start += 3;
        size_t end = url.find(":", start);
        if (end == std::string::npos) {
            end = url.find("/", start);
        }
        
        if (end == std::string::npos) return url.substr(start);
        return url.substr(start, end - start);
    }
    
    int extractPort(const std::string& url) {
        size_t start = url.find("://");
        if (start == std::string::npos) return 80;
        
        start = url.find(":", start + 3);
        if (start == std::string::npos) return 80;
        
        start += 1;
        size_t end = url.find("/", start);
        
        if (end == std::string::npos) return std::stoi(url.substr(start));
        return std::stoi(url.substr(start, end - start));
    }
};

class SimpleP2P {
private:
    NativeUPnP upnp;
    int sock;
    int port;
    
public:
    SimpleP2P(int p = 54321) : port(p) {
        #ifdef _WIN32
            WSADATA wsa;
            WSAStartup(MAKEWORD(2,2), &wsa);
        #endif
        
        // Создаем UDP сокет
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cout << "Ошибка создания сокета\n";
            return;
        }
        
        // Привязываем
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cout << "Ошибка bind, пробую порт 0\n";
            addr.sin_port = 0;
            bind(sock, (sockaddr*)&addr, sizeof(addr));
            
            socklen_t len = sizeof(addr);
            getsockname(sock, (sockaddr*)&addr, &len);
            port = ntohs(addr.sin_port);
        }
        
        std::cout << "Использую порт: " << port << "\n";
        
        // Пробуем UPnP
        std::cout << "\n=== UPnP ПРОБРОС ПОРТОВ ===\n";
        if (upnp.discover()) {
            if (upnp.addPortMapping(port)) {
                std::string external_ip = upnp.getExternalIP();
                if (!external_ip.empty()) {
                    std::cout << "\n=== ВАЖНАЯ ИНФОРМАЦИЯ ===\n";
                    std::cout << "Твой внешний IP: " << external_ip << "\n";
                    std::cout << "Порт для друга: " << port << "\n";
                    std::cout << "==========================\n\n";
                }
            }
        } else {
            std::cout << "UPnP не найден. Используй ручной проброс:\n";
            manualPortForwarding();
        }
    }
    
    ~SimpleP2P() {
        #ifdef _WIN32
            closesocket(sock);
            WSACleanup();
        #else
            close(sock);
        #endif
    }
    
    void run() {
        std::cout << "\nВыбери режим:\n";
        std::cout << "1. Ждать сообщений (сервер)\n";
        std::cout << "2. Отправить сообщение (клиент)\n";
        std::cout << "Выбор: ";
        
        int choice;
        std::cin >> choice;
        std::cin.ignore();
        
        if (choice == 1) {
            runAsServer();
        } else {
            runAsClient();
        }
    }
    
private:
    void manualPortForwarding() {
        std::cout << "\n=== РУЧНОЙ ПРОБРОС ПОРТОВ ===\n";
        std::cout << "1. Узнай свой локальный IP:\n";
        std::cout << "   Windows: ipconfig\n";
        std::cout << "   Linux: ifconfig или ip addr\n";
        std::cout << "\n2. Зайди в настройки роутера:\n";
        std::cout << "   - Открой браузер\n";
        std::cout << "   - Введи 192.168.1.1 или 192.168.0.1\n";
        std::cout << "   - Логин/пароль: admin/admin\n";
        std::cout << "\n3. Найди 'Port Forwarding' или 'Виртуальные серверы'\n";
        std::cout << "\n4. Добавь правило:\n";
        std::cout << "   - Протокол: UDP\n";
        std::cout << "   - Внешний порт: " << port << "\n";
        std::cout << "   - Внутренний порт: " << port << "\n";
        std::cout << "   - Внутренний IP: (твой локальный IP)\n";
        std::cout << "\n5. Узнай внешний IP: посети 2ip.ru\n";
        std::cout << "\n6. Дай другу внешний IP и порт " << port << "\n";
        std::cout << "================================\n\n";
    }
    
    void runAsServer() {
        std::cout << "\nРежим: СЕРВЕР\n";
        std::cout << "Жду сообщений на порту " << port << "...\n";
        
        char buffer[1024];
        sockaddr_in client_addr;
        
        #ifdef _WIN32
            int client_len = sizeof(client_addr);
        #else
            socklen_t client_len = sizeof(client_addr);
        #endif
        
        while (true) {
            std::cout << "\nОжидание... (Ctrl+C для выхода)\n";
            
            int len = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                              (sockaddr*)&client_addr, &client_len);
            
            if (len > 0) {
                buffer[len] = '\0';
                
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
                
                std::cout << "\nПолучено от " << ip_str << ": " << buffer << "\n";
                
                // Отвечаем
                std::string reply = "Получил: " + std::string(buffer);
                sendto(sock, reply.c_str(), reply.length(), 0,
                       (sockaddr*)&client_addr, client_len);
            }
        }
    }
    
    void runAsClient() {
        std::string server_ip;
        int server_port;
        
        std::cout << "\nРежим: КЛИЕНТ\n";
        std::cout << "IP сервера: ";
        std::getline(std::cin, server_ip);
        
        std::cout << "Порт сервера: ";
        std::cin >> server_port;
        std::cin.ignore();
        
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);
        
        std::cout << "\nВводи сообщения (пустая строка для выхода):\n";
        
        while (true) {
            std::cout << "> ";
            std::string message;
            std::getline(std::cin, message);
            
            if (message.empty()) break;
            
            // Отправляем
            sendto(sock, message.c_str(), message.length(), 0,
                   (sockaddr*)&server_addr, sizeof(server_addr));
            
            std::cout << "Отправлено! Жду ответа...\n";
            
            // Ждем ответ
            char buffer[1024];
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
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            
            if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
                int len = recvfrom(sock, buffer, sizeof(buffer)-1, 0,
                                  (sockaddr*)&from_addr, &from_len);
                if (len > 0) {
                    buffer[len] = '\0';
                    std::cout << "Ответ: " << buffer << "\n";
                }
            } else {
                std::cout << "Ответ не получен\n";
            }
        }
    }
};

int main() {
    setlocale(LC_ALL, "Russian");
    std::cout << "=== P2P ЧАТ (UPnP без библиотек) ===\n\n";
    
    SimpleP2P chat;
    chat.run();
    
    return 0;
}