#include <iostream>
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

int main() {
    #ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
    #endif
    
    // Просто отправляем тестовый пакет
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(53920); // Ваш порт
    inet_pton(AF_INET, "95.107.51.200", &target.sin_addr); // Ваш IP
    
    const char* msg = "TEST_PACKET";
    sendto(sock, msg, strlen(msg), 0, (sockaddr*)&target, sizeof(target));
    
    std::cout << "Test packet sent\n";
    
    #ifdef _WIN32
        closesocket(sock);
        WSACleanup();
    #else
        close(sock);
    #endif
    
    return 0;
}