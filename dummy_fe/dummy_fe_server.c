/**
 * \page dummy_fe_server
 * \section Introduction
 * local ts server based on dummy frontend
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <expat.h>

#include <linux/dvb/frontend.h>
#include <getopt.h>
#include "dummy_fe_wrapper.h"
#include "cJSON.h"
#include "dmx.h"

#define INF(fmt, ...) fprintf(stdout, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

#define DIFF_THRESHOLD 20*1000*1000
#define MAX_PCR_PIDS 100
#define DUMMY_MAX_TP_NUM 16
#define DUMMY_BLOCK_SIZE (188*1024)

typedef struct {
	int delivery_system;
	int freqM;
	char ts_file[512];
} Dummy_TPInfo_t;

typedef struct {
	int id;
	int inject_fd;
	int filter_fd;
	Dummy_TPInfo_t tp_list[DUMMY_MAX_TP_NUM];
	int running;
} Dummy_FrontendInfo_t;

typedef struct {
	uint16_t pcr_pid;
	uint64_t last_pcr;
	uint64_t start_time;
} PidPCR;

static int tp_cnt = 0;
static PidPCR pcr_pids[MAX_PCR_PIDS];
static int pcr_pid_count = 0;

static int running = 1;
static int dummy_fe_fd = -1;
static pthread_t fe_thread_handle = (pthread_t) -1;
static Dummy_FrontendInfo_t dummy_fe_info;

static int dummy_dvr_device_open(int dvr_dev_id, int rw)
{
	int fd;
	int flags = 0;
	char dev_name[32];

	memset(dev_name, 0, sizeof(dev_name));
	snprintf(dev_name, sizeof(dev_name), "/dev/dvb0.dvr%d", dvr_dev_id);
	if (rw)
		flags = O_RDONLY;
	else
		flags = O_WRONLY;

	fd = open(dev_name, flags);
	if (fd == -1) {
		ERR("%s cannot open \"%s\" (%s)\n", __func__, dev_name, strerror(errno));
		return fd;
	}

	INF("%s open %s succeed, fd: %d, rw: %d\n", __func__, dev_name, fd, rw);
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK, 0) < 0) {
		ERR("%s set nonblock flag failed \"%s\"\n", __func__, strerror(errno));
	}

	return fd;
}

static int dummy_set_demux_source(int dmx_id)
{
	int r = 0;
	char node[32] = {0};

	snprintf(node, sizeof(node), "/dev/dvb0.demux%d", dmx_id);
	int fd = open(node, O_RDWR);
	if (fd < 0) {
		ERR("%s open demux%d failed!\n", __func__, dmx_id);
		return -1;
	}

	struct dmx_sct_filter_params filter_param;
	memset(&filter_param, 0, sizeof(filter_param));
	filter_param.pid = 0;
	filter_param.filter.filter[0] = 0;
	filter_param.filter.mask[0] = 0xff;
	filter_param.flags = 1;
	r = ioctl(fd, DMX_SET_FILTER, &filter_param);
	r |= ioctl(fd, DMX_SET_BUFFER_SIZE, 32 * 1024);
	r |= ioctl(fd, DMX_START);
	if (r) {
		ERR("%s create filter failed: %d\n", __func__, r);
		return -1;
	}

	int source = DMA_0 + dmx_id;
	int input = INPUT_LOCAL;

	if (ioctl(fd, DMX_SET_INPUT, input) == -1) {
		ERR("%s set input failed. error: %d\n", __func__, errno);
		r = -1;
	}

	if (ioctl(fd, DMX_SET_HW_SOURCE, source) == -1) {
		ERR("%s set hw source failed. error: %d\n", __func__, errno);
		r = -1;
	}

	dummy_fe_info.filter_fd = fd;

	INF("%s set dmx%d to local mode and source is DMA_%d\n", __func__, dmx_id, dmx_id);
	return r;
}

static int dummy_inject_data(uint8_t *buf, int len)
{
	int ret;

	if (dummy_fe_info.inject_fd < 0) {
		ERR("invalid inject fd\n");
		return -1;
	}

	ret = write(dummy_fe_info.inject_fd, buf, len);
	if (ret <= 0) {
		ERR("failed to inject, inject fd: %d, err: %d\n",
				dummy_fe_info.inject_fd, errno);
		return -1;
	}
	//INF("inject len: %#x, actual injected len: %#x\n", len, ret);
	return ret;
}

enum DUMMY_FE_FILE_TYPE {
	DUMMY_FE_FILE_XML,
	DUMMY_FE_FILE_JSON,
	DUMMY_FE_FILE_INVALID,
};

static enum DUMMY_FE_FILE_TYPE check_file_type(FILE *fp)
{
	#define BUFFER_SIZE 1024
	enum DUMMY_FE_FILE_TYPE type = DUMMY_FE_FILE_INVALID;
	char buf[BUFFER_SIZE];

	size_t bytes_read = fread(buf, 1, BUFFER_SIZE, fp);
	if (bytes_read == 0) {
		return type;
	}

	int i = 0;
	while (i < bytes_read && isspace((unsigned char) buf[i])) {
		i++;
	}

	if (buf[i] == '<') {
		type = DUMMY_FE_FILE_XML;
	} else if (buf[i] == '{' || buf[i] == '[') {
		type = DUMMY_FE_FILE_JSON;
	} else {
		ERR("invalid config file\n");
	}

	INF("config file type: %d\n", type);
	return type;
}

static void start_element(void *data, const char *element, const char **attr) {
	Dummy_FrontendInfo_t *fe = &dummy_fe_info;

	INF("Start element: %s\n", element);
	if (strcmp(element, "freq")) {
		INF("not freq\n");
		return;
	}

	for (int i = 0; attr[i]; i+=2) {
		INF(" Attribute: %s = %s\n", attr[i], attr[i+1]);
		if (!strcmp(attr[i], "modulation")) {
			if (!strcmp(attr[i+1], "DVBC")) {
				fe->tp_list[tp_cnt].delivery_system = SYS_DVBC_ANNEX_A;
			} else if (!strcmp(attr[i+1], "DVBT")) {
				fe->tp_list[tp_cnt].delivery_system = SYS_DVBT;
			} else if (!strcmp(attr[i+1], "DVBT2")) {
				fe->tp_list[tp_cnt].delivery_system = SYS_DVBT2;
			} else if (!strcmp(attr[i+1], "DVBS")) {
				fe->tp_list[tp_cnt].delivery_system = SYS_DVBS;
			} else if (!strcmp(attr[i+1], "DVBS2")) {
				fe->tp_list[tp_cnt].delivery_system = SYS_DVBS2;
			} else {
				ERR("unsupported delivery system, %s\n", attr[i+1]);
			}
		} else if (!strcmp(attr[i], "frequency")) {
			fe->tp_list[tp_cnt].freqM = atoi(attr[i+1]) / 1000 / 1000;
		} else if (!strcmp(attr[i], "filename")) {
			strcpy(&fe->tp_list[tp_cnt].ts_file[0], attr[i+1]);
		} else {
			INF(" Ignor attribute: %s = %s\n", attr[i], attr[i+1]);
		}
	}

	tp_cnt++;
}

static void end_element(void *data, const char *element) {
	INF("End element: %s\n", element);
}

static void handle_data(void *data, const char *content, int length) {
	INF("Character data: %.*s\n", length, content);
}

static int dummy_fe_xml_parse(FILE *fp)
{
	char buffer[128];
	int done;
	int len;

	fseek(fp, 0, SEEK_SET);

	// Create XML Parser
	XML_Parser parser = XML_ParserCreate(NULL);

	// Set callback
	XML_SetElementHandler(parser, start_element, end_element);
	XML_SetCharacterDataHandler(parser, handle_data);

	// Read a block and parser
	do {
		len = fread(buffer, 1, sizeof(buffer), fp);
		if (ferror(fp)) {
			ERR("read file error\n");
			break;
		}

		done = feof(fp);

		// Parser data block
		if (XML_Parse(parser, buffer, len, done) == XML_STATUS_ERROR) {
			ERR("xml parser error: %s at line %lu\n",
				XML_ErrorString(XML_GetErrorCode(parser)),
				XML_GetCurrentLineNumber(parser));
			break;
		}
	} while (!done);

	// Free XML Parser
	XML_ParserFree(parser);

	INF("TP list:\n");
	Dummy_FrontendInfo_t *fe = &dummy_fe_info;
	for (int i = 0; i < tp_cnt; i++) {
		INF("freq: %d, delivery_system: %d, stream: %s\n",
			fe->tp_list[i].freqM,
			fe->tp_list[i].delivery_system,
			fe->tp_list[i].ts_file);
	}

	return 0;
}

static int dummy_fe_json_parse(FILE *fp)
{
	uint8_t *buf = NULL;
	int len;

	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = malloc(len);
	if (buf) {
		if (fread(buf, 1, len, fp) != len)
			return -1;
	}

	cJSON *json = cJSON_Parse((const char *)buf);
	free(buf);
	if (!json) {
		ERR("fe parse failed, (%s)\n", cJSON_GetErrorPtr());
		return -1;
	}

	// parse json object
	cJSON *frontend = cJSON_GetObjectItem(json, "frontend");
	cJSON *tp_list = cJSON_GetObjectItem(frontend, "tp_list");
	int array_size = cJSON_GetArraySize(tp_list);
	Dummy_FrontendInfo_t *fe = &dummy_fe_info;


	INF("TP list:\n");
	for (int i = 0; i < array_size && i < DUMMY_MAX_TP_NUM; i++) {
		cJSON *tp = cJSON_GetArrayItem(tp_list, i);

		cJSON *delivery_system = cJSON_GetObjectItem(tp, "delivery_system");
		char *delivery = delivery_system->valuestring;
		if (!strcmp(delivery, "DVBC")) {
			fe->tp_list[i].delivery_system = SYS_DVBC_ANNEX_A;
		} else if (!strcmp(delivery, "DVBT")) {
			fe->tp_list[i].delivery_system = SYS_DVBT;
		} else if (!strcmp(delivery, "DVBT2")) {
			fe->tp_list[i].delivery_system = SYS_DVBT2;
		} else if (!strcmp(delivery, "DVBS")) {
			fe->tp_list[i].delivery_system = SYS_DVBS;
		} else if (!strcmp(delivery, "DVBS2")) {
			fe->tp_list[i].delivery_system = SYS_DVBS2;
		} else {
			ERR("unsupported delivery system, %s\n", delivery_system->valuestring);
			continue;
		}

		INF("delivery: %s, value: %d\n", delivery, fe->tp_list[i].delivery_system);
		fe->tp_list[i].freqM = cJSON_GetObjectItem(tp, "freqM")->valueint;
		strcpy(&fe->tp_list[i].ts_file[0], cJSON_GetObjectItem(tp, "stream")->valuestring);
		INF("TP[%d] freq: %d, stream: %s\n", i, fe->tp_list[i].freqM, &fe->tp_list[i].ts_file[0]);
	}

	cJSON_Delete(json);

	return 0;
}

static char* dummy_fe_get_stream(int delivery_system, int freqM)
{
	int i;
	Dummy_FrontendInfo_t *fe = &dummy_fe_info;

	if (freqM <= 0)
		return NULL;

	for (i = 0; i < DUMMY_MAX_TP_NUM; i++) {
		if (fe->tp_list[i].delivery_system == delivery_system &&
			fe->tp_list[i].freqM == freqM) {
			return &fe->tp_list[i].ts_file[0];
		}
	}

	ERR("%s failed, delivery_system: %d, freqM: %d\n",
			__func__, delivery_system, freqM);

	return NULL;
}

// Get current system time in microseconds
uint64_t get_current_time_us()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000 + tv.tv_usec;
}

// Extract PCR values from TS packet
uint64_t extract_pcr(const uint8_t *packet)
{
	uint64_t pcr_base = ((uint64_t)(packet[6] & 0xFF) << 25) |
						((uint64_t)(packet[7] & 0xFF) << 17) |
						((uint64_t)(packet[8] & 0xFF) << 9) |
						((uint64_t)(packet[9] & 0xFF) << 1) |
						((uint64_t)(packet[10] & 0x80) >> 7);
	return pcr_base;  // Ignor PCR extension
}

// Record the PID which contains PCR
void record_pcr_pid(uint16_t pid)
{
	for (int i = 0; i < pcr_pid_count; i++) {
		if (pcr_pids[i].pcr_pid == pid) {
			return;
		}
	}

	if (pcr_pid_count < MAX_PCR_PIDS) {
		pcr_pids[pcr_pid_count].pcr_pid = pid;
		pcr_pids[pcr_pid_count].last_pcr = 0;
		pcr_pid_count ++;
		INF("%s record pid: %#x, cnt: %d\n", __func__, pid, pcr_pid_count);
	}
}

// Check if the TS packet contains PCR and record its PID
void check_and_record_pcr_pid(const uint8_t *packet)
{
	if (packet[3] & 0x20) {	// Adaptation field control
		int adp_field_len = packet[4];
		if (adp_field_len > 0 && (packet[5] & 0x10)) {	// PCR flag
			uint16_t pid = ((packet[1] & 0x1f) << 8) | packet[2];
			record_pcr_pid(pid);
		}
	}
}

void *dummy_inject_thread(void *arg)
{
	char *stream = (char *)arg;
	uint8_t *buf;

	INF("inject %s\n", stream);
	FILE *file = fopen(stream, "rb");
	if (!file) {
		ERR("Error opening input file %s\n", stream);
		return NULL;
	}

	buf = (uint8_t *)malloc(DUMMY_BLOCK_SIZE);
	if (!buf) {
		ERR("no heap memory\n");
		return NULL;
	}

	while (dummy_fe_info.running) {
		uint64_t start_time;
		int data_size = DUMMY_BLOCK_SIZE;

		pcr_pid_count = 0;
		memset(&pcr_pids[0], 0, sizeof(PidPCR) * MAX_PCR_PIDS);

		INF("loop\n");
		start_time = get_current_time_us();
		fseek(file, 0, SEEK_SET);
		while (fread(buf, 1, data_size, file) == data_size) {
			int left = data_size;
			uint8_t *packet = buf;
			//INF("%s %#x bytes read\n", __func__, data_size);
			if (!dummy_fe_info.running)
				break;

			while (left >= 188) {
				if (packet[0] != 0x47) {
					ERR("invalid sync byte\n");
					packet++;
					left--;
					continue;
				}

				// Check and record pids that contain PCR
				check_and_record_pcr_pid(packet);

				// Process TS packets containing PCR
				if (packet[3] & 0x20) { // adaptation field control
					int adp_field_len = packet[4];
					//INF("%s adp_field_len: %d, packet5: %#x\n",
								//__func__, adp_field_len, packet[5]);
					if (adp_field_len > 0 && (packet[5] & 0x10)) { // PCR flag
						uint16_t pid = ((packet[1] & 0x1f) << 8) | packet[2];
						uint64_t current_pcr = extract_pcr(packet);

						for (int i = 0; i < pcr_pid_count; i++) {
							if (pcr_pids[i].pcr_pid ==  pid) {
								uint64_t last_pcr = pcr_pids[i].last_pcr;
								if (pcr_pids[i].start_time == 0)
									pcr_pids[i].start_time = start_time;
								if (last_pcr != 0) {
									uint64_t pcr_diff = current_pcr - last_pcr;

									// PCR is a 90MHz clock, 1 PCR tick = 1/90000000 second
									uint64_t pcr_time_us = pcr_diff / 90 * 1000;
									//INF("pid: %#x, pcr_time_us: %lld\n", pid, pcr_time_us);

									uint64_t current_time = get_current_time_us();
									int elapsed_time_us = current_time - pcr_pids[i].start_time;

									//INF("pid: %#x, current_time: %llu, start_time: %llu, elapsed_time_us: %d\n",
											//pid, current_time, pcr_pids[i].start_time, elapsed_time_us);
									if (elapsed_time_us > 0 &&
											elapsed_time_us < pcr_time_us &&
											(pcr_time_us - elapsed_time_us) < DIFF_THRESHOLD) {
										INF("usleep %lld\n", pcr_time_us - elapsed_time_us);
										usleep(pcr_time_us - elapsed_time_us);
										pcr_pids[i].start_time = get_current_time_us();
										//INF("PCR: %llx, Last_PCR: %llx\n", current_pcr, pcr_pids[i].last_pcr);
										pcr_pids[i].last_pcr = current_pcr;
									}
								} else {
									pcr_pids[i].last_pcr = current_pcr;
								}
								break;
							}
						}
					}
				}
				packet += 188;
				left -= 188;
			}

			dummy_inject_data(buf, data_size);
		}
	}

	if (file)
		fclose(file);
	if (buf)
		free(buf);

	INF("exit %s\n", __func__);
	return NULL;
}

static void dummy_fe_term(void)
{

	dummy_fe_info.running = 0;

	if (!pthread_equal(fe_thread_handle, (pthread_t) -1) && running) {
		running = 0;
		pthread_join(fe_thread_handle, NULL);
	}

	if (dummy_fe_fd != -1)
		close(dummy_fe_fd);

	if (dummy_fe_info.inject_fd >= 0)
		close(dummy_fe_info.inject_fd);

	if (dummy_fe_info.filter_fd >= 0)
		close(dummy_fe_info.filter_fd);
	exit(0);
}

static void handle_signal(int signal)
{
	dummy_fe_term();
}

static void init_signal_handler(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = handle_signal;
	sigaction(SIGINT, &act, NULL);
}

static struct option long_options[] = {
	{"help", no_argument, 0, 'h'},
	{"config", required_argument, 0, 'f'},
	{"device", required_argument, 0, 'd'},
	{0, 0, 0, 0}
};

static void usage(char *argv[])
{
	INF("Usage: %s [option]\n", argv[0]);
	INF("	-h --help		Show this help message\n");
	INF("	-f --config		Specify configure file\n");
	INF("	-d --device		Specify FE device\n");
	INF("	-i --dmxdevice	Specify Demux device\n");
}

int main(int argc, char **argv)
{
	int ret;
	int opt;
	int opt_index = 0;
	int fe_id = -1;
	int dmx_id = -1;
	char fe_file_path[512];

	init_signal_handler();
	memset(&fe_file_path[0], 0, sizeof(fe_file_path));
	while ((opt = getopt_long(argc, argv, "hf:d:i:", long_options, &opt_index)) != -1) {
		switch (opt) {
			case 'h':
				usage(argv);
				exit(0);
			case 'f':
				strcpy(&fe_file_path[0], optarg);
				INF("FE config file: %s\n", optarg);
				break;
			case 'd':
				fe_id = strtol(optarg, NULL, 10);
				INF("FE id: %d\n", fe_id);
				break;
			case 'i':
				dmx_id = strtol(optarg, NULL, 10);
				INF("Demux id: %d\n", dmx_id);
				break;
			default:
				usage(argv);
				abort();
		}
	}

	if (fe_id == -1 || dmx_id == -1) {
		usage(argv);
		exit(0);
	}

	FILE *fe_fp = fopen(fe_file_path, "rb");
	if (fe_fp == NULL) {
		ERR("open %s failed!\n", fe_file_path);
		return -1;
	}

	// parse and save frontend info from the frontend file
	memset(&dummy_fe_info, 0, sizeof(Dummy_FrontendInfo_t));
	dummy_fe_info.id = fe_id;
	dummy_fe_info.inject_fd = -1;
	dummy_fe_info.filter_fd = -1;

	// check file type. xml or json ?
	enum DUMMY_FE_FILE_TYPE type = check_file_type(fe_fp);

	if (type == DUMMY_FE_FILE_XML) {
		ret = dummy_fe_xml_parse(fe_fp);
	} else if (type == DUMMY_FE_FILE_JSON) {
		ret = dummy_fe_json_parse(fe_fp);
	} else {
		ret= -1;
	}

	fclose(fe_fp);
	if (ret) {
		ERR("fe parse type:%d failed\n", type);
		return -1;
	}

	dummy_fe_info.inject_fd = dummy_dvr_device_open(dmx_id, 0);
	if (dummy_fe_info.inject_fd < 0) {
		ERR("open dvr device failed\n");
		return -1;
	}

	// Set demux source to DMA0 + dmx_id for ts inject
	dummy_set_demux_source(dmx_id);

	// open dummy fe device
	char *dev_name = "/dev/dummy_fe";
	dummy_fe_fd = open(dev_name, O_RDWR);
	if (dummy_fe_fd == -1) {
		ERR("cannot open \"%s\" (%s)", dev_name, strerror(errno));
		return -1;
	}

	struct dummy_fe_property prop;
	memset(&prop, 0, sizeof(struct dummy_fe_property));

	prop.cmd = DUMMY_FE_ID;
	prop.data = dummy_fe_info.id;
	INF("set prop, cmd: %d, data: %d\n", prop.cmd, prop.data);
	ret = ioctl(dummy_fe_fd, DUMMY_FE_SET_PROPERTY, &prop);
	if (ret) {
		INF("set fe id[%d] failed, ret: %#x, %s\n", prop.data, ret, strerror(errno));
		return -1;
	}

	dummy_fe_info.running = 1;

	char *stream;
	struct pollfd poll_fd;
	poll_fd.fd = dummy_fe_fd;
	poll_fd.events = POLLIN | POLLERR;
	while (running) {
		ret = poll(&poll_fd, 1, 100);
		if (ret < 0) {
			INF("poll failed, (%s)", strerror(errno));
			continue;
		}

		if (!(poll_fd.revents & POLLIN))
			continue;
		INF("poll revents: %#x\n", poll_fd.revents);

		prop.cmd = DUMMY_FE_STATE;
		ret = ioctl(dummy_fe_fd, DUMMY_FE_GET_PROPERTY, &prop);
		if (ret) {
			INF("get state failed, ret: %#x\n", ret);
			continue;
		}
		INF("dummy fe state: %d\n", prop.data);
		if (prop.data != DUMMY_FE_LOCK) {
			continue;
		}

		prop.cmd = DUMMY_FE_DELIVERY_SYSTEM;
		ret = ioctl(dummy_fe_fd, DUMMY_FE_GET_PROPERTY, &prop);
		if (ret) {
			INF("get delivery system failed, ret: %#x\n", ret);
			continue;
		}
		int delivery_system = prop.data;
		INF("dummy fe delivery system: %d\n", delivery_system);

		prop.cmd = DUMMY_FE_FREQUENCY;
		ret = ioctl(dummy_fe_fd, DUMMY_FE_GET_PROPERTY, &prop);
		if (ret) {
			INF("get frequency failed, ret: %#x\n", ret);
			continue;
		}
		int freqM = prop.data / 1000 / 1000;
		INF("dummy fe frequency: %d\n", freqM);

		// get ts stream from the dummy frontend information according to
		// the delivery system and frequency
		stream = dummy_fe_get_stream(delivery_system, freqM);
		if (stream == NULL) {
			ERR("cannot find delivery:%d, freqM:%d from FE config file\n",
					delivery_system, freqM);
			prop.cmd = DUMMY_FE_STATE;
			prop.data = DUMMY_FE_UNLOCKED;
			ret = ioctl(dummy_fe_fd, DUMMY_FE_SET_PROPERTY, &prop);
			if (ret) {
				ERR("set property failed, ret: %d\n", ret);
			}
			continue;
		} else {
			prop.cmd = DUMMY_FE_STATE;
			prop.data = DUMMY_FE_LOCKED;
			ret = ioctl(dummy_fe_fd, DUMMY_FE_SET_PROPERTY, &prop);
			if (ret) {
				ERR("set property failed, ret: %d\n", ret);
			}
		}

		if (!pthread_equal(fe_thread_handle, (pthread_t) -1) && dummy_fe_info.running) {
			dummy_fe_info.running = 0;
			pthread_join(fe_thread_handle, NULL);
			fe_thread_handle = (pthread_t) -1;
		}

		// create thread to read TS file and inject to hardware demux
		dummy_fe_info.running = 1;
		pthread_create(&fe_thread_handle, NULL, dummy_inject_thread, stream);
	}

	dummy_fe_term();

	return 0;
}
