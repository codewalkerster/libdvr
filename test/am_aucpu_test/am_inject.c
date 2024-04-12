#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
/***************************************************************************
 *  Copyright C 2009 by Amlogic, Inc. All Rights Reserved.
 */
/**\file
 * \brief DVR测试程序
 *
 * \author Xia Lei Peng <leipeng.xia@amlogic.com>
 * \date 2010-12-10: create the document
 ***************************************************************************/

#define AM_DEBUG_LEVEL 1

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <am_debug.h>
#include <stdio.h>
#include "am_dmx.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

/****************************************************************************
 * Macro definitions
 ***************************************************************************/
#ifdef CHIP_8226H
#define DVR_DEV_COUNT      (2)
#elif defined(CHIP_8226M) || defined(CHIP_8626X)
#define DVR_DEV_COUNT      (3)
#else
#define DVR_DEV_COUNT      (2)
#endif

#undef DVR_DEV_COUNT
#define DVR_DEV_COUNT      (2)

#define FEND_DEV_NO 0
#define AV_DEV_NO 0
#define PLAY_DMX_DEV_NO 1

#define DMX_DEV_NO 0
#define DVR_DEV_NO 0
#define ASYNC_FIFO 0

typedef struct
{
	int id;
	int ofd;
	pthread_t thread;
	int running;
	int fd;
}DVRData;

static DVRData data_threads[DVR_DEV_COUNT];

static int dvr_data_write(int fd, uint8_t *buf, int size)
{
	int ret;
	int left = size;
	uint8_t *p = buf;

	while (left > 0)
	{
//		printf("write start\n");
		ret = write(fd, p, left);
//		printf("write end\n");
		if (ret == -1)
		{
			if (errno != EINTR)
			{
				printf("Write DVR data failed: %s", strerror(errno));
				break;
			}
			ret = 0;
		} else {
//			printf("%s write cnt:%d\n",__FUNCTION__,ret);
		}

		left -= ret;
		p += ret;
	}

	return (size - left);
}

static void handle_signal(int signal)
{
	int i;
	UNUSED(signal);

	exit(0);
}

static void init_signal_handler()
{
	struct sigaction act;
	act.sa_handler = handle_signal;
	sigaction(SIGINT, &act, NULL);
}

static int inject_running=0;
static int inject_loop=0;
static void* inject_entry(void *arg)
{
	DVRData *dvr_data = (DVRData *)arg;
	int sock = dvr_data->ofd;
	int fd = dvr_data->fd;
	uint8_t buf[50*188];
	int len, left=0, send, ret;
	int cnt=50;
	struct timeval start_tv;
	struct timeval now_tv;
	unsigned int diff_time = 0;
	unsigned int total_len = 0;

	gettimeofday(&start_tv, NULL);
	printf("inject thread start");
	while (inject_running) {
		len = sizeof(buf) - left;
		ret = read(sock, buf+left, len);
		if (ret > 0) {
//			printf("recv %d bytes\n", ret);
/*			if (!cnt){
				cnt=50;
				printf("recv %d\n", ret);
			}
			cnt--; */
			left += ret;
			total_len += ret;
		} else {
			if (inject_loop && ((ret == 0) || (errno == EAGAIN))) {
				printf("loop\n");
				lseek(sock, 0, SEEK_SET);
				left=0;
				continue;
			} else {
				fprintf(stderr, "read file failed [%d:%s] ret=%d left=%d\n", errno, strerror(errno), ret, left);
				break;
			}
		}
		if (left) {
#if 0
			gettimeofday(&now_tv, NULL);
			if (now_tv.tv_usec < start_tv.tv_usec) {
				diff_time = (now_tv.tv_sec - 1 - start_tv.tv_sec) * 1000 + (now_tv.tv_usec + 1*1000*1000 - start_tv.tv_usec) / 1000;
			} else {
				diff_time = (now_tv.tv_sec - start_tv.tv_sec) * 1000 + (now_tv.tv_usec - start_tv.tv_usec) / 1000;
			}

			if ( diff_time != 0 && total_len/1024/1024*8*1000/diff_time > 4) {
				usleep(20*1000);
			}
#endif
			usleep(50*1000);
			send = left;
			send = dvr_data_write(fd, buf, send);
//			printf("write %d bytes\n", send);
			if (send) {
				left -= send;
				if (left)
					memmove(buf, buf+send, left);
				//AM_DEBUG(1, "inject %d bytes", send);
			}
		}
	}
	printf("inject thread end");
	inject_running = -1;
	return NULL;
}

#ifdef LINUX
#define DVR_DEVICE "/dev/dvb/adapter0/dvr"
#else
#define DVR_DEVICE "/dev/dvb0.dvr"
#endif

int inject_file(int dvr_no, char *inject_name)
{
	int loop = 0;
	int ofd;
	char dev_name[32];
	int fd;
	int ret;

	pthread_t th;

	printf("inject file name=%s to dvr%d\n", inject_name, dvr_no);

	init_signal_handler();
	snprintf(dev_name, sizeof(dev_name), DVR_DEVICE"%d", dvr_no);

	fd = open(dev_name, O_WRONLY);
	if (fd == -1)
	{
		printf("cannot open \"%s\" (%s)", dev_name, strerror(errno));
		return -1;
	}
//	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK, 0);

	ret = ioctl(fd, DMX_SET_INPUT, INPUT_LOCAL);
	printf("DMX_SET_INPUT ret:%d, %s\n", ret, strerror(errno));

	data_threads[0].id =0;
	data_threads[0].fd = fd;

	data_threads[0].running = 0;

	ofd = open(inject_name, O_RDWR, S_IRUSR);
	if (ofd == -1) {
		printf("cannot open \"%s\" (%s)", inject_name, strerror(errno));
		return -1;
	}
	data_threads[0].ofd = ofd;

	inject_loop = loop;
	inject_running = 1;
	pthread_create(&th, NULL, inject_entry, (void*)(long)&data_threads[0]);

	return 0;
}
int inject_file_and_rec_close(void) {

	inject_running = 0;

	do {}while((inject_running != -1));

	close(data_threads[0].fd);
	close(data_threads[0].ofd);
	return 0;
}

