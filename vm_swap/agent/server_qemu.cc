// clang-format off
// To avoid reordering the header
#include <sys/socket.h>
#include <linux/vm_sockets.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <regex>
// clang-format on
// VSOCK PORT
#define VSOCK_PORT 12345

#define BUF_SIZE 1024

bool is_valid_ip(const std::string& ip) {
  std::regex ip_regex(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$)");
  return std::regex_match(ip, ip_regex);
}

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

void handle_client(int client_fd) {
  char buffer[BUF_SIZE];
  ssize_t n;

  ssize_t recv_len = read_response(client_fd, buffer, BUF_SIZE);

  if (recv_len > 0) {
    buffer[recv_len] = '\0';
    std::string ip_str(buffer);

    ip_str.erase(std::remove(ip_str.begin(), ip_str.end(), '\n'), ip_str.end());
    ip_str.erase(std::remove(ip_str.begin(), ip_str.end(), '\r'), ip_str.end());

    if (!is_valid_ip(ip_str)) {
      std::cout << "Illegal ipv4" << std::endl;
      goto end;
    }
    std::string nic = "ens3";
    auto cmd0 = "ip addr flush dev " + nic +
                " && "
                "ip addr add " +
                ip_str + "/30" + " dev " + nic +
                " && "
                "ip link set dev " +
                nic + " up";

    std::cout << cmd0 << std::endl;

    int result = system(cmd0.c_str());

    if (result != 0) {
      printf("NON ZERO return");
      goto end;
    }

    std::string response = "OK\n";
    int ret = write(client_fd, response.c_str(), response.length());
  } else if (recv_len < 0) {
    perror("n<0");
  } else {
    std::cout << "No data" << std::endl;
  }
end:
  close(client_fd);

  exit(0);
}

int main() {
  int listen_fd, client_fd;
  struct sockaddr_vm listen_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  // 1. create vsock
  listen_fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (listen_fd == -1) {
    perror("create vsocke failed");
    exit(EXIT_FAILURE);
  }

  memset(&listen_addr, 0, sizeof(listen_addr));
  listen_addr.svm_family = AF_VSOCK;
  listen_addr.svm_port = VSOCK_PORT;
  listen_addr.svm_cid = VMADDR_CID_ANY;

  if (bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) ==
      -1) {
    perror("bind error");
    close(listen_fd);
    exit(EXIT_FAILURE);
  }

  if (listen(listen_fd, 10) == -1) {
    perror("listen failed");
    close(listen_fd);
    exit(EXIT_FAILURE);
  }

  client_fd =
      accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
  if (client_fd == -1) {
    perror("client id");
    exit(EXIT_FAILURE);
  }

  std::cout << "\nNew connection: CID=" << client_addr.svm_cid
            << ", Port=" << client_addr.svm_port << std::endl;

  handle_client(client_fd);

  return 0;
}
