#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sys/time.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"
#define CHUNK_SIZE 65536        // 64 KB
#define PAYLOAD_SIZE 1400       // UDP payload an toàn (MTU 1500 - header)
#define NUM_CONNECTIONS 4

std::mutex file_mutex;

// void download_chunk(const std::string &filename, long offset, long chunk_size, int part) {
//     int sock = socket(AF_INET, SOCK_DGRAM, 0);
//     if (sock < 0) {
//         perror("Socket creation failed");
//         return;
//     }

//     // struct timeval tv;
//     // tv.tv_sec = 20;  // Timeout sau 5 giây
//     // tv.tv_usec = 0;

//     // setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

//     // std::cout << "Đã đặt timeout cho socket" << std::endl;

//     sockaddr_in server_addr = {AF_INET, htons(PORT)};
//     inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

//     // Gửi yêu cầu tải chunk
//     std::ostringstream request;
//     request << "DOWNLOAD " << filename << " " << offset << " " << chunk_size;

//     std::cout << "[Client] Requesting: " << filename 
//               << " from offset " << offset 
//               << " (chunk size: " << chunk_size << " bytes)" << std::endl;

//     std::cout << "[Debug] Gửi yêu cầu: " << request.str() << std::endl;

//     ssize_t sent_bytes = sendto(sock, request.str().c_str(), request.str().size(), 0, 
//                                 (struct sockaddr *)&server_addr, sizeof(server_addr));

//     if (sent_bytes < 0) {
//         perror("sendto failed");
//         close(sock);
//         return;
//     }

//     // Nhận và ghi dữ liệu theo từng gói nhỏ
//     std::ofstream file(filename + ".part" + std::to_string(part), std::ios::binary);
//     char buffer[PAYLOAD_SIZE + 8]; // 8 bytes header chứa offset
//     long received_bytes = 0;

//     while (received_bytes < chunk_size) {
//         std::cout << "[Debug] Đang chờ dữ liệu từ server..." << std::endl;
//         ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
//         std::cout << "[Debug] Nhận được " << recv_bytes << " bytes" << std::endl;
//         if (recv_bytes <= 8) {
//             std::cerr << "[Error] Không nhận được dữ liệu hoặc gói tin quá nhỏ cho part " << part << std::endl;
//             break;
//         }

//         // Đọc offset từ gói tin
//         long received_offset;
//         memcpy(&received_offset, buffer, sizeof(long));

//         // Debug offset nhận được
//         std::cout << "[Debug] Nhận offset: " << received_offset 
//                   << ", yêu cầu offset trong khoảng [" << offset << ", " << offset + chunk_size << "]" 
//                   << ", kích thước gói: " << recv_bytes << " bytes" << std::endl;

//         // Nếu offset không hợp lệ, bỏ qua gói tin nhưng tiếp tục nhận các gói khác
//         if (received_offset < offset || received_offset >= offset + chunk_size) {
//             std::cerr << "[Warning] Offset không hợp lệ, bỏ qua gói tin!" << std::endl;
//             continue;
//         }

//         // Ghi dữ liệu (bỏ qua header)
//         file.write(buffer + 8, recv_bytes - 8);
//         received_bytes += (recv_bytes - 8);
//     }

//     file.close();
//     close(sock);

//     if (received_bytes == 0) {
//         std::cerr << "[Warning] Không nhận được dữ liệu hợp lệ cho part " << part << std::endl;
//     }

//     std::cout << "[Client] Downloaded " << filename << " part " << part 
//               << " (" << received_bytes << " bytes)" << std::endl;
// }

void download_chunk(const std::string &filename, long start_offset, long end_offset, int thread_id) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    sockaddr_in server_addr = {AF_INET, htons(PORT)};
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    std::ofstream file(filename + ".part" + std::to_string(thread_id), std::ios::binary);
    char buffer[PAYLOAD_SIZE + 8]; // 8 bytes header chứa offset
    long received_bytes = 0;

    for (long offset = start_offset; offset < end_offset; offset += PAYLOAD_SIZE) {
        long send_size = std::min((long)PAYLOAD_SIZE, end_offset - offset);

        std::ostringstream request;
        request << "DOWNLOAD " << filename << " " << offset << " " << send_size;

        std::cout << "[Thread " << thread_id << "] Requesting: " << filename
                  << " from offset " << offset << " (chunk size: " << send_size << " bytes)" << std::endl;

        ssize_t sent_bytes = sendto(sock, request.str().c_str(), request.str().size(), 0, 
                                    (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (sent_bytes < 0) {
            perror("[Error] sendto failed");
            continue;
        }

        // Nhận dữ liệu
        ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        if (recv_bytes <= 8) {
            std::cerr << "[Error] Không nhận được dữ liệu hoặc gói tin quá nhỏ\n";
            continue;
        }

        // Đọc offset từ gói tin
        long received_offset;
        memcpy(&received_offset, buffer, sizeof(long));

        if (received_offset != offset) {
            std::cerr << "[Warning] Offset không khớp! Bỏ qua gói tin.\n";
            continue;
        }

        // Ghi dữ liệu vào file (bỏ qua phần header 8 byte)
        file.write(buffer + 8, recv_bytes - 8);
        received_bytes += (recv_bytes - 8);
    }

    file.close();
    close(sock);

    std::cout << "[Thread " << thread_id << "] Finished downloading " 
              << filename << " part " << thread_id 
              << " (" << received_bytes << " bytes)" << std::endl;
}


void merge_file(const std::string &filename) {
    std::ofstream outfile(filename + "_merged", std::ios::binary);
    if (!outfile) {
        std::cerr << "Error creating merged file" << std::endl;
        return;
    }

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        std::string part_filename = filename + ".part" + std::to_string(i);
        std::ifstream infile(part_filename, std::ios::binary | std::ios::ate);

        if (!infile) {
            std::cerr << "[Error] Không tìm thấy file " << part_filename << std::endl;
            continue;
        }

        std::streamsize size = infile.tellg(); // Lấy kích thước file
        infile.seekg(0, std::ios::beg);        // Quay lại đầu file để đọc

        if (size == 0) {
            std::cerr << "[Warning] File " << part_filename << " có kích thước 0!" << std::endl;
            infile.close();
            continue;
        }

        std::cout << "[Debug] Đọc file " << part_filename << " (kích thước: " << size << " bytes)" << std::endl;
        outfile << infile.rdbuf(); // Ghép nội dung vào file gốc
        infile.close();

        if (remove(part_filename.c_str()) == 0) {
            std::cout << "[Debug] Xóa file " << part_filename << " sau khi merge thành công." << std::endl;
        } else {
            std::cerr << "[Error] Không thể xóa file " << part_filename << std::endl;
        }
    }

    outfile.close();
    std::cout << "[Debug] File merge hoàn tất: " << filename + "_merged" << std::endl;
}


// void download_file(const std::string &filename, long file_size) {
//     long chunk_size = file_size / NUM_CONNECTIONS;
//     std::vector<std::thread> threads;

//     for (int i = 0; i < NUM_CONNECTIONS; i++) {
//         threads.emplace_back(download_chunk, filename, i * chunk_size, chunk_size, i);
//     }
//     for (auto &t : threads) t.join();

//     std::cout << "[Debug] Tất cả luồng đã kết thúc, chuẩn bị merge file..." << std::endl;
//     merge_file(filename);
// }

void download_file(const std::string &filename, long file_size) {
    long chunk_per_thread = file_size / NUM_CONNECTIONS;  // Mỗi thread xử lý phần này
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        long start_offset = i * chunk_per_thread;
        long end_offset = (i == NUM_CONNECTIONS - 1) ? file_size : start_offset + chunk_per_thread;

        // Tạo thread tải phần của file (gồm nhiều chunk nhỏ)
        threads.emplace_back(download_chunk, filename, start_offset, end_offset, i);
    }

    for (auto &t : threads) t.join();

    std::cout << "[Debug] Tất cả luồng đã kết thúc, chuẩn bị merge file..." << std::endl;
    merge_file(filename);
}

int main() {
    std::ifstream input_file("input.txt");
    std::string filename;
    
    while (std::getline(input_file, filename)) {
        download_file(filename, 1024 * 1024);  // Giả sử file 1MB
    }
}