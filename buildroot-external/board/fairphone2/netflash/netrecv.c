// SPDX-License-Identifier: GPL-2.0
/*
 * Accept one TCP connection, or read stdin when the port is "-", and write
 * everything received to a file or block device (then fsync it), or to stdout
 * with "-". Reports the byte count and the elapsed time on stderr.
 * Usage: netrecv <port|-> <output|->
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE (4 << 20)

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	int one = 1, rcvbuf = 4 << 20;
	long long total = 0;
	int ls, s, out;
	double t0;
	char *buf;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <port> <output>\n", argv[0]);
		return 2;
	}

	buf = malloc(BUF_SIZE);
	out = strcmp(argv[2], "-") ? open(argv[2], O_WRONLY) : STDOUT_FILENO;
	if (!buf || out < 0) {
		perror(argv[2]);
		return 1;
	}

	if (!strcmp(argv[1], "-")) {
		s = STDIN_FILENO;
	} else {
		ls = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		setsockopt(ls, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
		addr.sin_port = htons(atoi(argv[1]));
		if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 1)) {
			perror("listen");
			return 1;
		}

		s = accept(ls, NULL, NULL);
		if (s < 0) {
			perror("accept");
			return 1;
		}
	}
	t0 = now();

	for (;;) {
		ssize_t n = read(s, buf, BUF_SIZE);
		ssize_t off = 0;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			perror("read");
			return 1;
		}
		if (n == 0)
			break;
		while (off < n) {
			ssize_t w = write(out, buf + off, n - off);

			if (w < 0) {
				perror("write");
				return 1;
			}
			off += w;
		}
		total += n;
	}

	if (out != STDOUT_FILENO && fsync(out)) {
		perror("fsync");
		return 1;
	}
	fprintf(stderr, "%lld bytes received in %.1f s\n", total, now() - t0);
	close(s);
	close(out);

	return 0;
}
