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
#include <openssl/sha.h>  // Thư viện OpenSSL để tính checksum
#include <zlib.h>  // Thư viện hỗ trợ CRC32
#include <cstdio>  // remove()
#include <sys/stat.h> // stat()
#include <csignal>
#include <set>


#define PORT 8080
#define CHUNK_SIZE 65536        // 64 KB
#define PAYLOAD_SIZE 1024       // UDP payload an toàn (MTU 1500 - header)
#define NUM_CONNECTIONS 4
#define RETRY_LIMIT 40

std::mutex file_mutex;
std::mutex progress_mutex;
long file_total_size = 1;  // Tránh chia cho 0
long total_downloaded = 0;

std::string calculate_checksum(const char *data, size_t len) {
    uLong crc = crc32(0L, Z_NULL, 0);  // Khởi tạo CRC
    crc = crc32(crc, reinterpret_cast<const Bytef *>(data), len);

    return std::string(reinterpret_cast<const char *>(&crc), sizeof(crc)); // 4 byte CRC32
}

void download_chunk(int sock,
                    std::ofstream &file,
                    long &current_offset,
                    long start_offset,
                    long end_offset,
                    const std::string &filename,
                    int chunk_id,
                    const sockaddr_in &server_addr)
{
    struct AckPacket {
        long offset;
        int chunk_id;
    };
    char buffer[PAYLOAD_SIZE + 12];
    sockaddr_in from_addr{};
    socklen_t addr_len = sizeof(from_addr);

    ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&from_addr, &addr_len);

    if (recv_bytes < 12) {
        std::cerr << "[Chunk " << chunk_id << "] Không nhận được dữ liệu hoặc gói quá nhỏ.\n";
        return;
    }

    // Đọc offset từ gói tin
    long received_offset;
    memcpy(&received_offset, buffer, sizeof(long));

    if (received_offset != current_offset) {
        std::cerr << "[Chunk " << chunk_id << "] Offset không khớp! Nhận: "
                  << received_offset << ", mong đợi: " << current_offset << "\n";
        return;
    }

    // Đọc checksum
    std::string received_checksum(buffer + 8, 4);
    std::string computed_checksum = calculate_checksum(buffer + 12, recv_bytes - 12);

    if (memcmp(received_checksum.data(), computed_checksum.data(), 4) != 0) {
        std::cerr << "[Chunk " << chunk_id << "] Checksum không khớp! Bỏ qua.\n";
        return;
    }

    // Ghi dữ liệu vào file
    file.write(buffer + 12, recv_bytes - 12);
    long downloaded_now = recv_bytes - 12;

    current_offset += downloaded_now;

    std::ostringstream oss;
    oss << "ACK " << received_offset << " " << chunk_id;

    std::string ack_msg = oss.str();

    ssize_t ack_sent = sendto(sock, ack_msg.c_str(), ack_msg.size(), 0,
                            (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ack_sent < 0) {
        std::cerr << "[Error] sendto ACK failed";
    } else {
        std::cout << "[DEBUG] Sent ACK: " << ack_msg << "\n";
    }

    // Hiển thị tiến độ từng chunk
    long chunk_size = end_offset - start_offset;
    double progress = std::min(100.0, ((current_offset - start_offset) * 100.0) / (double)chunk_size);

    std::cout << "\r[Chunk " << chunk_id << "] Progress: " << progress << "%   " << std::flush;

    if (current_offset >= end_offset) {
        std::cout << "\n[Chunk " << chunk_id << "] Hoàn tất.\n";
    }
}


bool file_exists(const std::string &filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

std::string get_unique_filename(const std::string &filename) {
    std::string unique_filename = filename + "_download";
    int index = 1;

    while (file_exists(unique_filename)) {
        unique_filename = filename + "_download_" + std::to_string(index);
        index++;
    }

    return unique_filename;
}

void merge_file(const std::string &filename) {
    std::string merged_filename = get_unique_filename(filename);
    std::ofstream outfile(merged_filename, std::ios::binary);
    
    if (!outfile) {
        std::cerr << "[Error] Không thể tạo file merge: " << merged_filename << std::endl;
        return;
    }

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        std::string part_filename = filename + ".part" + std::to_string(i);
        std::ifstream infile(part_filename, std::ios::binary | std::ios::ate);

        if (!infile) {
            std::cerr << "[Error] Không tìm thấy file " << part_filename << std::endl;
            continue;
        }

        std::streamsize size = infile.tellg();
        infile.seekg(0, std::ios::beg);

        if (size == 0) {
            std::cerr << "[Warning] File " << part_filename << " có kích thước 0!" << std::endl;
            infile.close();
            continue;
        }

        outfile << infile.rdbuf();
        infile.close();

        if (remove(part_filename.c_str()) != 0) {
            std::cerr << "[Error] Không thể xóa file " << part_filename << std::endl;
        }
    }

    outfile.close();
    std::cout << "[Info] File merge hoàn tất: " << merged_filename << std::endl;
}

void download_file(const std::string &filename, long file_size, const char* server_ip) {
    file_total_size = file_size;

    long chunk_size = file_size / NUM_CONNECTIONS;

    struct Chunk {
        int sock;
        std::ofstream file;
        long start_offset;
        long current_offset;
        long end_offset;
        bool done = false;
        sockaddr_in server_addr;
        int id;
    };

    std::vector<Chunk> chunks;

    // Tạo 4 socket + file + trạng thái cho từng chunk
    for (int i = 0; i < NUM_CONNECTIONS; ++i) {
        Chunk chunk;
        chunk.id = i;
        chunk.start_offset = i * chunk_size;
        chunk.end_offset = (i == NUM_CONNECTIONS - 1) ? file_size : chunk.start_offset + chunk_size;
        chunk.current_offset = chunk.start_offset;

        chunk.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (chunk.sock < 0) {
            perror("socket failed");
            exit(1);
        }

        memset(&chunk.server_addr, 0, sizeof(chunk.server_addr));
        chunk.server_addr.sin_family = AF_INET;
        chunk.server_addr.sin_port = htons(PORT);
        inet_pton(AF_INET, server_ip, &chunk.server_addr.sin_addr);

        chunk.file.open(filename + ".part" + std::to_string(i), std::ios::binary);
        if (!chunk.file) {
            std::cerr << "Không mở được file part " << i << "\n";
            exit(1);
        }

        // Gửi yêu cầu đầu tiên
        std::ostringstream req;
        long first_size = std::min((long)PAYLOAD_SIZE, chunk.end_offset - chunk.current_offset);
        req << "DOWNLOAD " << filename << " " << chunk.current_offset << " " << first_size << " " << chunk.id;

        sendto(chunk.sock, req.str().c_str(), req.str().size(), 0,
               (struct sockaddr*)&chunk.server_addr, sizeof(chunk.server_addr));

        chunks.push_back(std::move(chunk));
    }

    // Vòng lặp select
    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;

        int done_count = 0;
        for (auto& chunk : chunks) {
            if (!chunk.done) {
                FD_SET(chunk.sock, &readfds);
                if (chunk.sock > maxfd) maxfd = chunk.sock;
            } else {
                done_count++;
            }
        }

        if (done_count == NUM_CONNECTIONS) break;  // tất cả xong

        int ready = select(maxfd+1, &readfds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            perror("select error");
            exit(1);
        }

        for (auto& chunk : chunks) {
            if (!chunk.done && FD_ISSET(chunk.sock, &readfds)) {
                download_chunk(chunk.sock,
                               chunk.file,
                               chunk.current_offset,
                               chunk.start_offset,
                               chunk.end_offset,
                               filename,
                               chunk.id,
                               chunk.server_addr);

                if (chunk.current_offset >= chunk.end_offset) {
                    chunk.done = true;
                    std::cout << "[Chunk " << chunk.id << "] xong.\n";
                } else {
                    // gửi yêu cầu tiếp theo
                    long next_size = std::min((long)PAYLOAD_SIZE, chunk.end_offset - chunk.current_offset);
                    std::ostringstream req;
                    req << "DOWNLOAD " << filename << " " << chunk.current_offset << " " << next_size << " " << chunk.id;

                    sendto(chunk.sock, req.str().c_str(), req.str().size(), 0,
                           (struct sockaddr*)&chunk.server_addr, sizeof(chunk.server_addr));
                }
            }
        }
    }

    for (auto& chunk : chunks) {
        chunk.file.close();
        close(chunk.sock);
    }

    std::cout << "\n[Debug] Tất cả chunks đã kết thúc, chuẩn bị merge file...\n";
    merge_file(filename);
    total_downloaded = 0;
}

void request_file_list(const char* server_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ERROR] Socket creation failed");
        return;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    std::string request = "LIST";
    sendto(sock, request.c_str(), request.size(), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

    char buffer[4096];
    ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
    if (recv_bytes > 0) {
        buffer[recv_bytes] = '\0';
        std::cout << "[File List]\n" << buffer;
    } else {
        std::cerr << "[ERROR] Failed to receive file list" << std::endl;
    }

    close(sock);
}

long get_file_size(const std::string &filename, const char* server_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ERROR] Socket creation failed");
        return -1;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    std::string request = "SIZE " + filename;
    sendto(sock, request.c_str(), request.size(), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

    char buffer[64];
    ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
    close(sock);

    if (recv_bytes <= 0) {
        std::cerr << "[ERROR] Không thể lấy kích thước file từ server\n";
        return -1;
    }

    buffer[recv_bytes] = '\0';
    return std::stol(buffer); // Chuyển đổi kích thước file từ string sang long
}


std::vector<std::string> readFromLine(size_t skipLines, const std::string& filename) {
    std::vector<std::string> result;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "Không thể mở file: " << filename << std::endl;
        return result;
    }

    std::string line;
    size_t currentLine = 0;

    while (std::getline(file, line)) {
        if (currentLine >= skipLines) {
            result.push_back(line); // Thêm dòng sau khi bỏ qua skipLines dòng đầu tiên
        }
        currentLine++;
    }

    file.close();
    return result;
}

void display_menu() {
    std::cout << "\n===== MENU =====" << std::endl;
    std::cout << "1. Xem danh sách file trên server" << std::endl;
    std::cout << "2. Thêm và tải file ngay lập tức" << std::endl;
    std::cout << "3. Thoát chương trình" << std::endl;
    std::cout << "Lựa chọn của bạn: ";
}

void menu(const char* server_ip) {
    std::string filename = "input.txt";
    size_t processedLines = 0; // Theo dõi số dòng đã xử lý
    int choice;

    do {
        display_menu();
        std::cin >> choice;
        std::cin.ignore(); // Xóa bộ đệm nhập

        switch (choice) {
            case 1:
                request_file_list(server_ip);
                break;
            case 2: {
                while (true) {
                    // Đọc các dòng mới từ file
                    std::vector<std::string> newFiles = readFromLine(processedLines, filename);

                    if (newFiles.empty()) {
                        break;
                    }

                    for (const auto& file : newFiles) {
                        long file_size = get_file_size(file, server_ip);
                        if (file_size > 0) {
                            download_file(file, file_size, server_ip);
                            std::cout << "Đã tải xong file: " << file << "\n";
                        } else {
                            std::cerr << "[ERROR] Không thể tải file: " << file << std::endl;
                        }
                    }

                    processedLines += newFiles.size();
                }

                break;
            }
            case 3:
                std::cout << "Thoát chương trình..." << std::endl;
                break;
            default:
                std::cout << "Lựa chọn không hợp lệ, vui lòng thử lại!" << std::endl;
        }
    } while (choice != 3);
}

// Hàm xử lý tín hiệu Ctrl + C
void signal_handler(int signal) {
    std::cout << "\n[INFO] Nhận tín hiệu Ctrl + C. Đang thoát chương trình...\n";
    exit(0);
}

int main(int argc, char *argv[]) {
    // Đăng ký bắt tín hiệu Ctrl + C
    signal(SIGINT, signal_handler);

    // Kiểm tra tham số dòng lệnh
    if (argc < 2) {
        printf("Cách dùng: %s <Server_IP>\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1]; // Lấy IP từ tham số dòng lệnh
    printf("Server đang chạy trên IP: %s, Port: %d\n", server_ip, PORT);

    menu(server_ip);
    return 0;
}

