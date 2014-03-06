/** \file
 *  OPC image packet receiver.
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
#include <netinet/in.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include "util.h"
#include "ledscape.h"

typedef struct
{
	uint8_t channel;
	uint8_t command;
	uint8_t len_hi;
	uint8_t len_lo;
} opc_cmd_t;

static int
tcp_socket(
	const int port
)
{
	const int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (sock < 0)
		return -1;
	if (bind(sock, (const struct sockaddr*) &addr, sizeof(addr)) < 0)
		return -1;
	if (listen(sock, 5) == -1)
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
	int led_count = 64;
	int frame_rate = 30;

	extern char *optarg;
	int opt;

	int fd = 0, fout = 0;
	int fromfile = FALSE;
	
	while ((opt = getopt(argc, argv, "p:c:d:")) != -1)
	{
		switch (opt)
		{
		case 'p':
			port = atoi(optarg);
			break;
		case 'c':
			led_count = atoi(optarg);
			break;
		case 'd': {
			int width=0, height=0;

			if (sscanf(optarg,"%dx%d", &width, &height) == 2) {
				led_count = width * height;
			} else {
				printf("Invalid argument for -d; expected NxN; actual: %s", optarg);
				exit(EXIT_FAILURE);
			}
			break;

		case 'w': 
			printf("Writing OPC data to file: %s", optarg);
			fout = open(optarg, O_WRONLY | O_CREAT | O_TRUNC);
			break;
		
		case 'r': 
			printf("Reading OPC data from file: %s", optarg);
			fd = open(optarg, O_RDONLY);
			fromfile = TRUE;
			break;
			
		case 'f':
			frame_rate = atoi(optarg);
			break;
		
		}
		default:
			fprintf(stderr, "Usage: %s [-p <port>] [-c <led_count> | -d <width>x<height>] [-w <output file>] [-r <input file> [-f <frame rate>]] \n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (fromfile)
		printf("Frame rate: %d fps", frame_rate);

	const int sock = tcp_socket(port);
	if (sock < 0)
		die("socket port %d failed: %s\n", port, strerror(errno));

	const size_t image_size = led_count * 3;

	// largest possible UDP packet
	uint8_t buf[65536];
	if (sizeof(buf) < image_size + 1)
		die("%u too large for UDP\n", image_size);

	fprintf(stderr, "OpenPixelControl LEDScape Receiver started on TCP port %d for %d pixels.\n", port, led_count);

	ledscape_t * const leds = ledscape_init(led_count);

	const unsigned report_interval = 10;
	unsigned last_report = 0;
	unsigned long delta_sum = 0;
	unsigned frames = 0;

	ledscape_frame_t * const frame = ledscape_frame(leds, 0);

	memset(frame, 0, led_count * LEDSCAPE_NUM_STRIPS * 4);
	//ledscape_set_color(frame, 0, 0, 255, 0, 0);
	//ledscape_set_color(frame, 1, 0, 0, 255, 0);
	//ledscape_set_color(frame, 2, 0, 0, 0, 255);
	ledscape_draw(leds, 0);
	printf("ready\n");

	while (fd || (fd = accept(sock, NULL, NULL)) >= 0)
	{
		while(1)
		{
			opc_cmd_t cmd;
			ssize_t rlen = read(fd, &cmd, sizeof(cmd));
			
			if (rlen < 0)
				die("recv failed: %s\n", strerror(errno));
			if (rlen == 0)
			{
				close(fd);
				break;
			}

			const size_t cmd_len = cmd.len_hi << 8 | cmd.len_lo;
//			warn("received %zu bytes: %d %zu\n", rlen, cmd.command, cmd_len);
			size_t offset = 0;

			while (offset < cmd_len)
			{
				rlen = read(fd, buf + offset, cmd_len - offset);
				if (rlen < 0)
					die("recv failed: %s\n", strerror(errno));
				if (rlen == 0)
					break;
				offset += rlen;
			}
				
			if (cmd.command != 0)
				continue;

//printf("Ch %d: %db\n", cmd.channel, cmd_len);

			struct timeval start_tv, stop_tv, delta_tv;
			gettimeofday(&start_tv, NULL);

//			const unsigned frame_num = 0;

			for (unsigned int i=0; i<cmd_len; i++) {
				//uint8_t * const out = (void*) &frame[i + led_count*cmd.channel];
				const uint8_t * const in = &buf[3 * i];
				//out[0] = in[0];
				//out[1] = in[1];
				//out[2] = in[2];
        ledscape_set_color(
          frame,
          cmd.channel + i / led_count,
          i % led_count,
          
         in[0],
         in[1],
         in[2]
        );
 			}

			ledscape_draw(leds, 0);

			if (fromfile)
				usleep(1000000/frame_rate); // Not counting delay in this loop
				
			if (fout)
			{
				write(cmd, sizeof(cmd), 1, fpOut);
				write(cmd, sizeof(char), uint8_t, cmd_len, fpOut); 
			}

			gettimeofday(&stop_tv, NULL);
			timersub(&stop_tv, &start_tv, &delta_tv);

			frames++;
			delta_sum += delta_tv.tv_usec;
			if (stop_tv.tv_sec - last_report < report_interval)
				continue;
			last_report = stop_tv.tv_sec;

			const unsigned delta_avg = delta_sum / frames;
			printf("\n%6u usec avg, actual %.2f fps (over %u frames)\n",
				delta_avg,
//				report_interval * 1.0e6 / delta_avg,
				frames * 1.0 / report_interval,
				frames
			);

			frames = delta_sum = 0;
		}
	}

	return 0;
}
