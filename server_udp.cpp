#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>

#define PORT 8080
#define HEADER_SIZE 10  // Tăng kích thước header để tránh lỗi bị cắt dữ liệu
#define MAX_CHUNK_SIZE 4096  // Giới hạn kích thước chunk tối đa

void handle_client(int sock, sockaddr_in client_addr, socklen_t addr_len) {
    char request[1024];
    ssize_t recv_bytes = recvfrom(sock, request, sizeof(request) - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
    
    if (recv_bytes <= 0) {
        std::cerr << "[ERROR] Failed to receive request" << std::endl;
        return;
    }
    request[recv_bytes] = '\0';

    // Parse request
    std::istringstream iss(request);
    std::string command, filename;
    long offset, requested_chunk_size;
    iss >> command >> filename >> offset >> requested_chunk_size;

    if (command != "DOWNLOAD") {
        std::cerr << "[ERROR] Invalid command: " << command << std::endl;
        return;
    }

    // Mở file
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file: " << filename << std::endl;
        return;
    }

    // Xác định kích thước file
    long file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Kiểm tra offset hợp lệ
    if (offset >= file_size) {
        std::cerr << "[ERROR] Offset " << offset << " exceeds file size " << file_size << std::endl;
        return;
    }

    // Tính toán chunk size hợp lý
    long chunk_size = std::min({requested_chunk_size, file_size - offset, (long)MAX_CHUNK_SIZE});

    // Debug
    std::cout << "[DEBUG] File size: " << file_size << " bytes\n";
    std::cout << "[DEBUG] Offset requested: " << offset << "\n";
    std::cout << "[DEBUG] Chunk size: " << chunk_size << " bytes\n";

    // Cấp phát buffer động
    long buffer_size = chunk_size + sizeof(int64_t);
    char *buffer = new char[buffer_size];

    // **Sửa lỗi ghi offset: Ghi trực tiếp dưới dạng số nguyên**
    memcpy(buffer, &offset, sizeof(int64_t));

    // Đọc dữ liệu từ file
    file.seekg(offset, std::ios::beg);
    file.read(buffer + sizeof(int64_t), chunk_size);
    long bytes_read = file.gcount();

    if (bytes_read <= 0) {
        std::cerr << "[ERROR] Failed to read file at offset " << offset << std::endl;
        delete[] buffer;
        return;
    }

    std::cout << "[DEBUG] Sending " << bytes_read << " bytes to client\n";

    // Gửi dữ liệu tới client
    sendto(sock, buffer, bytes_read + sizeof(int64_t), 0, (struct sockaddr *)&client_addr, addr_len);

    // Giải phóng bộ nhớ
    delete[] buffer;
}

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        return EXIT_FAILURE;
    }

    std::cout << "Server UDP listening on port " << PORT << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        handle_client(sock, client_addr, addr_len);
    }

    close(sock);
    return 0;
}
