/** \file
 *  E1.31 packet receiver.
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

#define FALSE 0
#define TRUE 1



int
main(
	int argc,
	char ** argv
)
{
	int port = 5568;
	int led_count = 64;
	int frame_rate = 30;

	extern char *optarg;
	int opt;

	int fd = 0, fout = 0;
	int fromfile = FALSE;
	int lampTest = 0;

	fprintf(stderr, "E1.31 LEDScape Receiver\n\n");
	
	while ((opt = getopt(argc, argv, "p:c:d:w:r:f:t:")) != -1)
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
				printf("Invalid argument for -d; expected NxN; actual: %s\n", optarg);
				exit(EXIT_FAILURE);
			}
			break;
			}

/*
		case 'w': 
			printf("Writing OPC data to file: %s\n", optarg);
			fout = open(optarg, O_WRONLY | O_CREAT | O_TRUNC);
			break;
		
		case 'r': 
			printf("Reading OPC data from file: %s\n", optarg);
			fd = open(optarg, O_RDONLY);
			fromfile = TRUE;
			break;
			
		case 'f':
			frame_rate = atoi(optarg);
			break;
*/
		case 't':
			lampTest = atoi(optarg);
			break;

		default:
			fprintf(stderr, "Usage: %s [-p <port>] [-c <led_count> | -d <width>x<height>] [-w <output file>] [-r <input file> [-f <frame rate>]] [-t <lamp test 0-255>]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (fromfile)
		printf("Frame rate: %d fps\n", frame_rate);

	printf("LEDs per strip: %d\nPort: %d\n", led_count, port);

	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(port),
	};

	if (sock < 0)
		die("socket failed: %s\n", strerror(errno));

	if (bind(sock, (const struct sockaddr*) &addr, sizeof(addr)) < 0)
		die("bind port %d failed: %s\n", port, strerror(errno));

	const size_t image_size = led_count * 3;

	// largest possible UDP packet
	uint8_t buf[65536];
	if (sizeof(buf) < image_size + 1)
		die("%u too large for UDP\n", image_size);

	ledscape_t * const leds = ledscape_init(led_count);

	struct timeval t;
	gettimeofday(&t, NULL);
	const unsigned report_interval = 10;
	unsigned last_report = t.tv_sec;
	unsigned long delta_sum = 0;
	unsigned frames = 0;

	ledscape_frame_t * const frame = ledscape_frame(leds, 0);

	// initial value (perhaps lamp test was specified)
	memset(frame, lampTest, led_count * LEDSCAPE_NUM_STRIPS * 4);
	//ledscape_set_color(frame, 0, 0, 255, 0, 0);
	//ledscape_set_color(frame, 1, 0, 0, 255, 0);
	//ledscape_set_color(frame, 2, 0, 0, 0, 255);
	ledscape_draw(leds, 0);
	if (fromfile)
		printf("Playing\n");
	else
		printf("Ready\n");

	while (fd || (fd = accept(sock, NULL, NULL)) >= 0)
	{
		while(1)
		{
			const ssize_t rc = recv(sock, buf, sizeof(buf), 0);
			if (rc < 0) {
				printf("recv failed: %s\n", strerror(errno));
				continue;
			}
			
			if (buf[8] != 'E' || buf[9] != '1' || buf[11] != '1' || buf[12] != '7')
			{
			  printf("not an E1.17 packet\n");
			  continue;
			}
			
			// start timing
			struct timeval start_tv, stop_tv, delta_tv;
			gettimeofday(&start_tv, NULL);

			const int universe = buf[113] << 8 | buf[114];
			const int data_len = (buf[123] << 8 | buf[124]) - 1;
			warn("received %zu bytes for universe: %d\n", data_len, universe);
				
//printf("Ch %d: %db\n", cmd.channel, cmd_len);
			uint8_t* data = buf+126;
			for (unsigned int i=0; i<data_len; i++) {
				const uint8_t * const in = &data[3 * i];
			        ledscape_set_color(frame, universe + i / led_count, i % led_count, 
							in[0], in[1], in[2]);
 			}

			ledscape_draw(leds, 0);

/*
			if (fout)
			{
				write(fout, &cmd, sizeof(cmd));
				write(fout, buf, sizeof(uint8_t)*cmd_len); 
			}
*/
			gettimeofday(&stop_tv, NULL);
			timersub(&stop_tv, &start_tv, &delta_tv);

			// wait for next frame if reading from file
			if (fromfile)
				usleep(1000000/frame_rate - delta_tv.tv_usec - 180); // 180 is a magic number 
				
			frames++;
			delta_sum += delta_tv.tv_usec;
			if (stop_tv.tv_sec - last_report < report_interval)
				continue;

			last_report = stop_tv.tv_sec;

			const unsigned delta_avg = delta_sum / frames;
			printf("%u usec avg, actual %.2f fps (over %u frames)\n",
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
