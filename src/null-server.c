#include <stdlib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h>
#include <stdbool.h>

int main() {
    int server_fd, socket_fd;
    int opt = 1;
    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
        .sun_path = "connect.sock"
    };
    int addrlen = sizeof(address);
 
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
 
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
 
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    while (true) {
        if ((socket_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }

        printf("Connection opened\n");

        size_t size = 64 * 1024 * 1024;
        void *buffer = malloc(size);

        while (true) {
            size_t received = read(socket_fd, buffer, size);

            if (received < 0) {
                perror("read");
                exit(EXIT_FAILURE);
            }

            if (received == 0) {
                printf("Connection closed\n");
                break;
            }
        }
    }
 
    close(socket_fd);
    shutdown(server_fd, SHUT_RDWR);

    return 0;
}
