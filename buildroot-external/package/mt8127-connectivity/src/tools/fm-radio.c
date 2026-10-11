// SPDX-License-Identifier: GPL-2.0
/*
 * fm-radio: minimal control of a V4L2 radio receiver.
 *
 *   fm-radio [-d /dev/radio0] status
 *   fm-radio [-d dev] tune <MHz>
 *   fm-radio [-d dev] seek up|down
 *   fm-radio [-d dev] mute on|off
 *   fm-radio [-d dev] volume <0-15>
 *   fm-radio [-d dev] mono on|off
 *   fm-radio [-d dev] rds [seconds]
 *
 * Build: $(CC) -O2 -o fm-radio fm-radio.c
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

static int fd;

static void die(const char *what)
{
	fprintf(stderr, "fm-radio: %s: %s\n", what, strerror(errno));
	exit(1);
}

static int set_ctrl(unsigned int id, int value)
{
	struct v4l2_control c = { .id = id, .value = value };

	if (ioctl(fd, VIDIOC_S_CTRL, &c))
		die("VIDIOC_S_CTRL");
	return 0;
}

static int get_ctrl(unsigned int id)
{
	struct v4l2_control c = { .id = id };

	if (ioctl(fd, VIDIOC_G_CTRL, &c))
		die("VIDIOC_G_CTRL");
	return c.value;
}

static void status(void)
{
	struct v4l2_frequency f = { .tuner = 0 };
	struct v4l2_tuner t = { .index = 0 };

	if (ioctl(fd, VIDIOC_G_FREQUENCY, &f))
		die("VIDIOC_G_FREQUENCY");
	if (ioctl(fd, VIDIOC_G_TUNER, &t))
		die("VIDIOC_G_TUNER");

	printf("frequency %.2f MHz\n", f.frequency / 16000.0);
	printf("signal %u/65535 (%u%%)\n", t.signal, t.signal * 100 / 65535);
	printf("%s, audmode %s\n",
	       (t.rxsubchans & V4L2_TUNER_SUB_STEREO) ? "stereo" : "mono",
	       t.audmode == V4L2_TUNER_MODE_MONO ? "mono" : "stereo");
	printf("mute %d, volume %d\n", get_ctrl(V4L2_CID_AUDIO_MUTE),
	       get_ctrl(V4L2_CID_AUDIO_VOLUME));
}

static void tune(double mhz)
{
	struct v4l2_frequency f = {
		.tuner = 0,
		.type = V4L2_TUNER_RADIO,
		.frequency = (unsigned int)(mhz * 16000.0 + 0.5),
	};

	if (ioctl(fd, VIDIOC_S_FREQUENCY, &f))
		die("VIDIOC_S_FREQUENCY");
}

static void seek(int up)
{
	struct v4l2_hw_freq_seek s = {
		.tuner = 0,
		.type = V4L2_TUNER_RADIO,
		.seek_upward = up,
		.wrap_around = 1,
		.spacing = 100000,
	};

	if (ioctl(fd, VIDIOC_S_HW_FREQ_SEEK, &s)) {
		if (errno == ENODATA) {
			printf("no station found\n");
			return;
		}
		die("VIDIOC_S_HW_FREQ_SEEK");
	}
}

static void mono(int on)
{
	struct v4l2_tuner t = { .index = 0 };

	t.audmode = on ? V4L2_TUNER_MODE_MONO : V4L2_TUNER_MODE_STEREO;
	if (ioctl(fd, VIDIOC_S_TUNER, &t))
		die("VIDIOC_S_TUNER");
}

/* Prints the PI code, programme service name and blocks for some seconds. */
static void rds(int seconds)
{
	static const char *names = "ABCDC";
	char ps[9] = "        ";
	time_t end = time(NULL) + seconds;
	struct v4l2_rds_data d[3 * 4];
	unsigned int blk[4] = { 0 }, have = 0;

	while (time(NULL) < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;
		int i;

		if (poll(&p, 1, 500) <= 0)
			continue;
		n = read(fd, d, sizeof(d));
		if (n < 0)
			die("read");

		for (i = 0; i < n / (ssize_t)sizeof(d[0]); i++) {
			unsigned int id = d[i].block & V4L2_RDS_BLOCK_MSK;
			int err = d[i].block & V4L2_RDS_BLOCK_ERROR;

			printf("%c%s %02x%02x  ", names[id < 5 ? id : 0],
			       err ? "!" : " ", d[i].msb, d[i].lsb);
			if (id == V4L2_RDS_BLOCK_A)
				have = 0;
			if (err || id > 3)
				continue;
			blk[id] = d[i].msb << 8 | d[i].lsb;
			have |= 1 << id;
			if (id == V4L2_RDS_BLOCK_D) {
				printf("\n");
				if (have != 0xf)
					continue;
				/* group 0A/0B: two PS characters per group */
				if ((blk[1] >> 12) == 0) {
					unsigned int pos = (blk[1] & 3) * 2;

					ps[pos] = blk[3] >> 8;
					ps[pos + 1] = blk[3] & 0xff;
				}
				printf("PI %04x PS '%s'\n", blk[0], ps);
			}
		}
		fflush(stdout);
	}
}

static void usage(void)
{
	fprintf(stderr,
		"usage: fm-radio [-d dev] status|tune MHz|seek up|down|mute on|off|\n"
		"                volume 0-15|mono on|off|rds [seconds]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/radio0";
	const char *cmd, *arg;

	if (argc > 2 && !strcmp(argv[1], "-d")) {
		dev = argv[2];
		argc -= 2;
		argv += 2;
	}
	if (argc < 2)
		usage();
	cmd = argv[1];
	arg = argc > 2 ? argv[2] : NULL;

	fd = open(dev, O_RDWR);
	if (fd < 0)
		die(dev);

	if (!strcmp(cmd, "status")) {
		status();
	} else if (!strcmp(cmd, "tune") && arg) {
		tune(atof(arg));
		status();
	} else if (!strcmp(cmd, "seek") && arg) {
		seek(!strcmp(arg, "up"));
		status();
	} else if (!strcmp(cmd, "mute") && arg) {
		set_ctrl(V4L2_CID_AUDIO_MUTE, !strcmp(arg, "on"));
	} else if (!strcmp(cmd, "volume") && arg) {
		set_ctrl(V4L2_CID_AUDIO_VOLUME, atoi(arg));
	} else if (!strcmp(cmd, "mono") && arg) {
		mono(!strcmp(arg, "on"));
	} else if (!strcmp(cmd, "rds")) {
		rds(arg ? atoi(arg) : 10);
	} else {
		usage();
	}

	close(fd);
	return 0;
}
