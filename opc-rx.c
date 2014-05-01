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

#define FALSE 0
#define TRUE 1

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


int openTty(const char* devName) {
	struct termios tio;

        memset(&tio,0,sizeof(tio));
        tio.c_iflag=0;
        tio.c_oflag=0;
        tio.c_cflag=CS8|CREAD|CLOCAL;           // 8n1, see termios.h for more information
        tio.c_lflag=0;
        tio.c_cc[VMIN]=1;
        tio.c_cc[VTIME]=5;
 
        int tty_fd=open(devName, O_RDWR | O_NONBLOCK);      
        cfsetospeed(&tio,B115200);            // 115200 baud
        cfsetispeed(&tio,B115200);            // 115200 baud
 
        tcsetattr(tty_fd,TCSANOW,&tio);

	return tty_fd;
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

	int fd = 0, fout = 0, tty_fd = 0;
	int fromfile = FALSE;
	int loop = FALSE;

	int lampTest = 0;

	int uartDone = FALSE;
	uint8_t uartBuf[5];
	memset(uartBuf, 0, sizeof(uartBuf));
	uartBuf[0] = 0x01; // SOH
	uartBuf[4] = 0x04; // EOT

	fprintf(stderr, "OpenPixelControl LEDScape Receiver\n\n");
	
	while ((opt = getopt(argc, argv, "p:c:d:w:r:f:t:l:s:")) != -1)
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

		case 'w': 
			printf("Writing OPC data to file: %s\n", optarg);
			fout = open(optarg, O_WRONLY | O_CREAT | O_TRUNC);
			break;
		
		case 'r': 
			printf("Reading OPC data from file: %s\n", optarg);
			fd = open(optarg, O_RDONLY);
			fromfile = TRUE;
			break;

		case 'l':
			loop = TRUE;
			break;
			
		case 'f':
			frame_rate = atoi(optarg);
			break;

		case 't':
			lampTest = atoi(optarg);
			break;

		case 's':
			tty_fd = openTty(optarg);
			if (tty_fd == 0)
				die("Could not open port: '%s'\n", optarg);
			else
				printf("Serial port opened: %s\n", optarg);
			break;

		default:
			fprintf(stderr, "Usage: %s [-p <port>] [-c <led_count> | -d <width>x<height>] [-w <output file>] [-r <input file> [-f <frame rate>][-l(oop)] [-t <lamp test 0-255>]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (fromfile)
		printf("Frame rate: %d fps\n", frame_rate);

	printf("LEDs per strip: %d\nPort: %d\n", led_count, port);

	const int sock = tcp_socket(port);
	if (sock < 0)
		die("Socket port %d failed: %s\n", port, strerror(errno));

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
	//ledscape_set_color(frame, 0, 255, 255, 0, 0);
	//ledscape_set_color(frame, 0, 256, 0, 255, 0);
	//ledscape_set_color(frame, 0, 257, 0, 0, 255);
	ledscape_draw(leds, 0);
	if (fromfile)
		printf("Playing\n");
	else
		printf("Ready\n");

	while (fd || (fd = accept(sock, NULL, NULL)) >= 0)
	{
		if (!fromfile)
			printf("Socket connected\n");

		while(1)
		{
			opc_cmd_t cmd;
			ssize_t rlen = read(fd, &cmd, sizeof(cmd));
			
			if (rlen < 0)
			{
				printf("Closing socket\n");
				close(fd);
				fd = 0;
				break;
				//die("recv failed: %s\n", strerror(errno));
			}
			if (rlen == 0)
			{
				if (fromfile)
				{
					if (loop) {
						printf("looping file\n");
						lseek(fd, 0, SEEK_SET); // seek to beginning of file
						rlen = read(fd, &cmd, sizeof(cmd)); // read cmd header again
					} else {
						printf("closing file\n");
						close(fd);
						fd = 0;
						fromfile = FALSE;
						break;
					}
				}
				else
				{
					printf("Closing socket\n");
					close(fd);
					fd = 0;
					break;
				}
			}

			// start timing
			struct timeval start_tv, stop_tv, delta_tv;
			gettimeofday(&start_tv, NULL);

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

			for (unsigned int i=0; i<cmd_len/3; i++) {
				const uint8_t * const in = &buf[3*i];
			        ledscape_set_color(frame, cmd.channel + i / led_count, i % led_count, 
							in[0], in[1], in[2]);
 			}

//			ledscape_wait(leds);
			ledscape_draw(leds, 0);

			if (fout)
			{
				write(fout, &cmd, sizeof(cmd));
				write(fout, buf, sizeof(uint8_t)*cmd_len); 
			}

			gettimeofday(&stop_tv, NULL);
			timersub(&stop_tv, &start_tv, &delta_tv);

			// wait for next frame if reading from file
			if (fromfile)
				usleep(1000000/frame_rate - delta_tv.tv_usec - 180); // 180 is a magic number 
				
			frames++;
			delta_sum += delta_tv.tv_usec;

			if (tty_fd != 0 && stop_tv.tv_sec % 5 == 0) {
				if (!uartDone) {
					const uint8_t * const in = &buf[3 * (led_count*3+80)]; // 4th strand, about half way in
					uartBuf[1] = in[0];
					uartBuf[2] = in[1];
					uartBuf[3] = in[2];
					write(tty_fd, uartBuf, sizeof(uartBuf));
					uartDone = TRUE;
				}
			}
			else
				uartDone = FALSE;

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

	close(tty_fd);
	return 0;
}
