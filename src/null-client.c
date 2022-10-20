#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>

int main() {
	int fd;
	struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = "connect.sock",
    };
	int ret;
	int ok = 1;
	int len;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		ok = 0;
	}

	if (ok) {
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
			perror("connect");
			ok = 0;
		}
	}

    size_t total_gb = 32;
    size_t total_bytes = total_gb * 1024 * 1024 * 1024;
    size_t total_sent_bytes = 0;
    size_t buffer_size = 512 * 1024 * 1024;
    char *buffer = malloc(buffer_size);
    struct timespec start, end;

    for(size_t i = 0; i < buffer_size; i++) buffer[i] = 0;

    FILE *source_file = fopen("/tmp/null-client.data", "w+");

    if (source_file == NULL) {
        perror("open");

        return 1;
    }

    int source_fd = fileno(source_file);
    int error = ftruncate(source_fd, buffer_size);

    if (error != 0) {
        perror("truncate");

        return 1;
    }

    printf("warming up kernel cache\n");

    while (total_sent_bytes < (buffer_size * 8)) {
        off_t offset = 0;

		if ((len = sendfile(fd, source_fd, &offset, buffer_size)) < 0) {
			perror("send");
			ok = 0;
		}

        total_sent_bytes += len;
    }

    total_sent_bytes = 0;

    printf("starting transmission\n");

    clock_gettime(CLOCK_REALTIME, &start);

    while (total_sent_bytes < total_bytes) {
        off_t offset = 0;

		if ((len = sendfile(fd, source_fd, &offset, buffer_size)) < 0) {
			perror("send");
			ok = 0;
		}

        total_sent_bytes += len;
    }

	if (ok) {
        clock_gettime(CLOCK_REALTIME, &end);

        double total_sent_gb = total_sent_bytes / (1024 * 1024 * 1024);
        double ns = (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
        double seconds = ns / 1000000000;
        double gb_per_seconds = total_sent_gb / seconds;

        printf("%.2f GB/s %.2f sent in %.2f seconds\n", gb_per_seconds, total_sent_gb, seconds);
	}

	if (fd >= 0) {
		close(fd);
	}

	return 0;
}