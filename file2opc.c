/** \file
 *  OPC image packet reader.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <termios.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <getopt.h>
#include "util.h"

#define FALSE 0
#define TRUE 1

#define HOST_NAME_MAX 255

typedef struct
{
	uint8_t channel;
	uint8_t command;
	uint8_t len_hi;
	uint8_t len_lo;
} opc_cmd_t;

static int
tcp_socket(
  const char* host,
	const int port
)
{
	const int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port)
	};
  inet_pton(AF_INET, host, &addr.sin_addr.s_addr);

	if (sock < 0)
		return -1;
	if (connect(sock, (const struct sockaddr*) &addr, sizeof(addr)) < 0)
		return -1;

	return sock;
}


int
main(
	int argc,
	char ** argv
)
{
	int port = 7890;
	int frame_rate = 30;

	extern char *optarg;
	int opt;

	int fd = 0, fout = 0;
	int loop = TRUE;
  char host[HOST_NAME_MAX+1];
  strncpy(host, "127.0.0.1", HOST_NAME_MAX);

	fprintf(stderr, "OpenPixelControl File Reader\nMads Christensen (c) 2016, www.madschristensen.info\n\n");
	
	while ((opt = getopt(argc, argv, "h:p:r:l:f:")) != -1)
	{
		switch (opt)
		{
    case 'h':
      strncpy(host, optarg, HOST_NAME_MAX);
      break;

		case 'p':
			port = atoi(optarg);
			break;
		
		case 'r': 
			printf("Reading OPC data from file: %s\n", optarg);
			fd = open(optarg, O_RDONLY);
			break;

		case 'l':
			loop = atoi(optarg);
			break;
			
		case 'f':
			frame_rate = atoi(optarg);
			break;

		default:
			fprintf(stderr, "Usage: %s -r <input file> [-h <ipaddr>] [-p <port>] [-f <frame rate>] [-l(oop) 0|1]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

  if (fd == 0)
  	die("Input file must be specified\n");

	const int sock = tcp_socket(host, port);
	if (sock < 0)
		die("Connect on port %d failed: %s\n", port, strerror(errno));

  printf("OPC host: %s:%d\n", host, port);
	printf("Frame rate: %d fps\n", frame_rate);
  printf("Loop: %s\n", loop ? "yes" : "no");

	struct timeval t;
	gettimeofday(&t, NULL);
	const unsigned report_interval = 10;
	unsigned last_report = t.tv_sec;
	unsigned long delta_sum = 0;
	unsigned int frames = 0;
  unsigned int framesTotal = 0;
  ssize_t bytesTotal = 0;

	uint8_t buf[65536];
  int firstPacket = TRUE;

	while(1)
	{
		opc_cmd_t cmd;
		ssize_t rlen = read(fd, &cmd, sizeof(cmd));
    bytesTotal += rlen;

    if (firstPacket)
    {
      firstPacket = FALSE;
      printf("OPC header: channel=%d command=%d size=%d\n", cmd.channel, cmd.command, cmd.len_hi << 8 | cmd.len_lo);
    }
		
		if (rlen == 0)
		{
      printf("Bytes read: %u\n", bytesTotal);
      printf("Frames read: %u\n", framesTotal);
      framesTotal = 0;
      bytesTotal = 0;

			if (loop) {
				printf("Looping file\n");
				lseek(fd, 0, SEEK_SET); // seek to beginning of file
				rlen = read(fd, &cmd, sizeof(cmd)); // read cmd header again
        bytesTotal += rlen;
			} else {
				printf("Closing file\n");
				close(fd);
				fd = 0;
				break;
			}
		}

		// start timing
		struct timeval start_tv, stop_tv, delta_tv;
		gettimeofday(&start_tv, NULL);

		const size_t cmd_len = cmd.len_hi << 8 | cmd.len_lo;
		size_t offset = 0;

		while (offset < cmd_len)
		{
			rlen = read(fd, buf + offset, cmd_len - offset);
			if (rlen < 0)
				die("Read failed: %s\n", strerror(errno));
			if (rlen == 0)
				break;
			offset += rlen;

      bytesTotal += rlen;
		}
			
		if (cmd.command != 0)
			continue;

		write(sock, &cmd, sizeof(cmd));
		write(sock, buf, sizeof(uint8_t)*cmd_len); 

		gettimeofday(&stop_tv, NULL);
		timersub(&stop_tv, &start_tv, &delta_tv);

		// wait for next frame
		int usec = 1000000/frame_rate - delta_tv.tv_usec - 180; // 180 is a magic number 
		if (usec > 0)
			usleep(usec);
	
    framesTotal++;		
		frames++;
		delta_sum += delta_tv.tv_usec;

		if (stop_tv.tv_sec - last_report < report_interval)
			continue;

		last_report = stop_tv.tv_sec;

		const unsigned delta_avg = delta_sum / frames;
		printf("%u bytes, %u frames, %u usec avg, actual %.2f fps (over %u frames)\n",
      bytesTotal,
      framesTotal,
			delta_avg,
			frames * 1.0 / report_interval,
			frames
		);

		frames = delta_sum = 0;
	}

	return 0;
}
