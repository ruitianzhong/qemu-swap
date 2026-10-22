#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#define VSOCK_PORT 12345
#define BUFFER_SIZE 256

const int BUF_SIZE = 1024;

void error_exit(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

ssize_t read_response(int sockfd, char* buffer, size_t buf_size) {
  ssize_t total_read = 0;
  char ch;

  while (total_read < buf_size - 1) {
    ssize_t n = read(sockfd, &ch, 1);
    if (n <= 0) {
      if (n == 0) break;  // EOF
      error_exit("read failed");
    }

    buffer[total_read++] = ch;
    if (ch == '\n') break;
  }

  buffer[total_read] = '\0';
  return total_read;
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <IP_ADDRESS> <vm_cid>" << std::endl;
    exit(EXIT_FAILURE);
  }
  const char* ip_str = argv[1];
  int vm_cid = std::stoi(argv[2]);

  int sockfd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (sockfd == -1) {
    error_exit("socket creation failed");
  }

  sockaddr_vm server_addr = {};

  server_addr.svm_family = AF_VSOCK;
  server_addr.svm_cid = vm_cid;
  server_addr.svm_port = VSOCK_PORT;
  std::cout << "Connecting to VM CID " << vm_cid << " on port " << VSOCK_PORT
            << "..." << std::endl;

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) ==
      -1) {
    error_exit("connect failed");
  }
  std::cout << "Connected to VSOCK successfully" << std::endl;

  try {
    ssize_t ip_sent = write(sockfd, ip_str, strlen(ip_str));

    if (ip_sent == -1) {
      throw std::runtime_error("Failed to send IP address");
    }
    write(sockfd, "\n", 1);
    std::cout << "Sent IP: " << ip_str << std::endl;

    char final_ok[BUF_SIZE];
    ssize_t n = read_response(sockfd, final_ok, BUF_SIZE);
    if (n <= 0) {
      printf("%ld\n", n);
      throw std::runtime_error("No final OK response");
    }

    std::string final_response(final_ok);
    final_response.erase(final_response.find_last_not_of("\n\r") + 1);
    if (final_response != "OK") {
      throw std::runtime_error("Unexpected final response: " + final_response);
    }
    std::cout << "Received final OK: " << final_ok;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  close(sockfd);
  std::cout << "Connection closed successfully" << std::endl;

  return EXIT_SUCCESS;
}