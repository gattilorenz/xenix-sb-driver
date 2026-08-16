/*
 * play_wav.c - play a .wav file through /dev/sbdsp
 *
 * The DSP only does 8-bit unsigned PCM (see ch.6/9 of the driver
 * writer's guide and sb.c's DSPCMD_READ/WRITE), 4kHz-23kHz on output,
 * so this converts whatever the WAV file actually contains:
 *   - 8-bit or 16-bit PCM, mono or stereo -> 8-bit unsigned mono
 *   - sample rate is clamped to the board's supported range
 *
 * Only uncompressed PCM (WAVE_FORMAT_PCM) WAV files are supported.
 *
 * Compile:
 *     cc -o play_wav play_wav.c
 * Run:
 *     ./play_wav tada.wav
 */

#include <sys/types.h>
#include <sys/sb.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define CHUNK_SIZE 8192		/* read/write chunk, well under DSP_BUF_SIZE */

static unsigned char raw[CHUNK_SIZE];	/* bytes as read from the file */
static unsigned char pcm[CHUNK_SIZE];	/* converted 8-bit unsigned mono */

/* WAV integers are little-endian; read them out of a byte buffer
 * explicitly rather than overlaying a struct, to sidestep any
 * compiler-specific padding/alignment on the char+int mix in a
 * RIFF header. */
static long
get_le32(p)
unsigned char *p;
{
	return ((long) p[0]) | ((long) p[1] << 8)
		| ((long) p[2] << 16) | ((long) p[3] << 24);
}

static int
get_le16(p)
unsigned char *p;
{
	return ((int) p[0]) | ((int) p[1] << 8);
}

main(argc, argv)
int argc;
char **argv;
{
	int fdin, dspfd;
	unsigned char hdr[12];
	unsigned char chdr[8];
	unsigned char fmtbuf[16];
	char id[5];
	long chunk_size;
	int have_fmt;
	int channels, bits;
	long rate, speed, data_remaining;
	int in_frame_bytes, frames_per_read, read_bytes;
	int n, i, j, outbytes, want;

	if (argc != 2) {
		fprintf(stderr, "usage: %s file.wav\n", argv[0]);
		exit(1);
	}

	fdin = open(argv[1], O_RDONLY);
	if (fdin == -1) {
		perror(argv[1]);
		exit(1);
	}

	if (read(fdin, hdr, 12) != 12
	    || strncmp((char *) hdr, "RIFF", 4) != 0
	    || strncmp((char *) hdr + 8, "WAVE", 4) != 0) {
		fprintf(stderr, "%s: not a RIFF/WAVE file\n", argv[1]);
		exit(1);
	}

	have_fmt = 0;
	channels = 0;
	bits = 0;
	rate = 0;

	/* walk chunks until fmt has been seen and data has been found */
	for (;;) {
		if (read(fdin, chdr, 8) != 8) {
			fprintf(stderr,
				"%s: unexpected EOF looking for data chunk\n",
				argv[1]);
			exit(1);
		}
		memcpy(id, chdr, 4);
		id[4] = '\0';
		chunk_size = get_le32(chdr + 4);

		if (strcmp(id, "fmt ") == 0) {
			if (chunk_size < 16 || read(fdin, fmtbuf, 16) != 16) {
				fprintf(stderr, "%s: bad fmt chunk\n", argv[1]);
				exit(1);
			}
			if (get_le16(fmtbuf) != 1) {
				fprintf(stderr,
					"%s: only uncompressed PCM is supported\n",
					argv[1]);
				exit(1);
			}
			channels = get_le16(fmtbuf + 2);
			rate = get_le32(fmtbuf + 4);
			bits = get_le16(fmtbuf + 14);
			have_fmt = 1;

			/* skip any extra fmt bytes, and the pad byte if
			 * this chunk's size was odd (RIFF chunks are
			 * word-aligned) */
			if (chunk_size > 16)
				lseek(fdin, chunk_size - 16, 1);
			if (chunk_size & 1)
				lseek(fdin, 1L, 1);
			continue;
		}

		if (strcmp(id, "data") == 0) {
			if (!have_fmt) {
				fprintf(stderr,
					"%s: data chunk appeared before fmt chunk\n",
					argv[1]);
				exit(1);
			}
			data_remaining = chunk_size;
			break;
		}

		/* skip any other chunk (LIST, fact, etc.) */
		lseek(fdin, chunk_size + (chunk_size & 1), 1);
	}

	if (channels != 1 && channels != 2) {
		fprintf(stderr, "%s: unsupported channel count %d\n",
			argv[1], channels);
		exit(1);
	}
	if (bits != 8 && bits != 16) {
		fprintf(stderr, "%s: unsupported sample width %d bits\n",
			argv[1], bits);
		exit(1);
	}

	speed = rate;
	if (speed < 4000)
		speed = 4000;
	if (speed > 23000)
		speed = 23000;
	if (speed != rate)
		fprintf(stderr,
			"warning: file rate %ld Hz clamped to %ld Hz (board limit)\n",
			rate, speed);

	printf("%s: %ld Hz, %d channel%s, %d-bit -> 8-bit unsigned mono @ %ld Hz\n",
		argv[1], rate, channels, channels == 1 ? "" : "s", bits, speed);

	dspfd = open("/dev/sbdsp", O_WRONLY);
	if (dspfd == -1) {
		perror("/dev/sbdsp");
		exit(1);
	}

	if (ioctl(dspfd, DSP_IOCTL_SPEED, speed) == -1) {
		perror("ioctl DSP_IOCTL_SPEED");
		exit(1);
	}

	in_frame_bytes = channels * (bits / 8);
	frames_per_read = CHUNK_SIZE / in_frame_bytes;
	read_bytes = frames_per_read * in_frame_bytes;

	while (data_remaining > 0) {
		want = read_bytes;
		if ((long) want > data_remaining)
			want = (int) data_remaining;
		want -= want % in_frame_bytes;
		if (want == 0)
			break;

		n = read(fdin, raw, want);
		if (n <= 0)
			break;
		data_remaining -= n;

		outbytes = 0;
		for (i = 0; i + in_frame_bytes <= n; i += in_frame_bytes) {
			long acc;
			int samp, lo, hi;

			acc = 0;
			for (j = 0; j < channels; j++) {
				if (bits == 8) {
					/* WAV 8-bit PCM is already unsigned;
					 * shift to signed for mixing */
					samp = raw[i + j] - 128;
				} else {
					lo = raw[i + j * 2];
					hi = raw[i + j * 2 + 1];
					samp = (short) ((hi << 8) | lo);
					samp >>= 8;	/* 16-bit -> 8-bit signed range */
				}
				acc += samp;
			}
			acc /= channels;
			if (acc > 127)
				acc = 127;
			if (acc < -128)
				acc = -128;
			pcm[outbytes++] = (unsigned char) (acc + 128);
		}

		if (write(dspfd, pcm, outbytes) != outbytes) {
			perror("write /dev/sbdsp");
			break;
		}
	}

	if (ioctl(dspfd, DSP_IOCTL_FLUSH, 0) == -1)
		perror("ioctl DSP_IOCTL_FLUSH");

	close(dspfd);
	close(fdin);
	printf("done\n");
	exit(0);
}
