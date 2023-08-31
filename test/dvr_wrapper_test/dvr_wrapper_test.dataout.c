/**
 * \page dvb_wrapper_test
 * \section Introduction
 * test code with dvb_wrapper_xxxx APIs.
 * It supports:
 * \li Record
 *
 * \section Usage
 *
 * Help msg will be shown if the test runs without parameters.\n
 * There are some general concepts for the parameters:
 * \li assign value "1" to the parameter to enable a feature specified
 * \li millisecond is used as time unit
 * \li no limit to the parameter order
 *
 * For recording:
 * \code
 *   dvr_wrapper_test.dataout [tsin=n] [dataout=<stdout>]
 * \endcode
 *
 * \section ops Operations in the test:
 * \li quit\n
 *             quit the test
 * \endsection
 */


#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>

#include "dvr_segment.h"
#include "dvr_wrapper.h"
#include "dvb_utils.h"
#include "segment_dataout.h"


/****************************************************************************
 * Macro definitions
 ***************************************************************************/
#define DMX_DEV_DVR 1

#define REC_EVT(fmt, ...)   fprintf(stderr, "recorder:" fmt, ##__VA_ARGS__)
#define INF(fmt, ...)       fprintf(stderr, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...)       fprintf(stderr, "error:" fmt, ##__VA_ARGS__)
#define RESULT(fmt, ...)    fprintf(stderr, fmt, ##__VA_ARGS__)

static int ts_src=0;
static FILE *dataout = NULL;
static int flush_size = 10* 188 * 1024;
static   DVR_WrapperRecord_t recorder;


static void display_usage(void)
{
    INF( "==================\n");
    INF( "*quit\n");
    INF( "==================\n");
}

int start_test(void)
{
    DVR_Bool_t go = DVR_TRUE;
    char buf[256];

    display_usage();

    while (go) {
        if (fgets(buf, sizeof(buf), stdin)) {

            if (!strncmp(buf, "quit", 4)) {
                go = DVR_FALSE;
                continue;
            }
            else if (!strncmp(buf, "recpause", 8)) {
                INF("rec paused.\n");
            }
            else if (!strncmp(buf, "recresume", 9)) {
                INF("rec resumed.\n");
            }
            else {
                ERR("Unknown command: %s\n", buf);
                display_usage();
            }
        }
    }

    if (dataout && dataout != stdout) {
        fclose(dataout);
        dataout = NULL;
    }

    return 0;
}

static void log_rec_evt(DVR_WrapperRecordStatus_t *status, void *user)
{
  char *state[] = {
    /*DVR_RECORD_STATE_OPENED*/ "open",        /**< Record state is opened*/
    /*DVR_RECORD_STATE_STARTED*/"start",       /**< Record state is started*/
    /*DVR_RECORD_STATE_STOPPED*/"stop",        /**< Record state is stopped*/
    /*DVR_RECORD_STATE_CLOSED*/ "close",       /**< Record state is closed*/
  };
  REC_EVT("[%s] state[%s(0x%x)] time/size/pkts[%lu/%llu/%u]\n",
    (char *)user,
    state[status->state],
    status->state,
    status->info.time,
    status->info.size,
    status->info.pkts
    );
}

static DVR_Result_t RecEventHandler(DVR_RecordEvent_t event, void *params, void *userdata)
{
   if (userdata != NULL)
   {
      DVR_WrapperRecordStatus_t *status = (DVR_WrapperRecordStatus_t *)params;

      switch (event)
      {
         case DVR_RECORD_EVENT_STATUS:
            log_rec_evt(status, userdata);
         break;
         default:
            REC_EVT("Unhandled recording event 0x%x from (%s)\n", event, (char *)userdata);
         break;
      }
   }
   return DVR_SUCCESS;
}

static int data_cb(unsigned char *buf, size_t size, void *priv)
{
    INF("data(%d) priv(%s): %02x %02x %02x %02x\n", size, (char *)priv, buf[0], buf[1], buf[2], buf[3]);

    if (dataout)
        fwrite(buf, size, 1, dataout);

    return size;
}

static int start_recording()
{
    DVR_WrapperRecordOpenParams_t rec_open_params;
    DVR_WrapperRecordStartParams_t rec_start_params;
    DVR_WrapperPidsInfo_t *pids_info;
    char cmd[256];
    int error = 0;

    sprintf(cmd, "echo ts%d > /sys/class/stb/demux%d_source", ts_src, DMX_DEV_DVR);
    //system(cmd);

    dvb_set_demux_source(DMX_DEV_DVR, ts_src);

    memset(&rec_open_params, 0, sizeof(DVR_WrapperRecordOpenParams_t));

    rec_open_params.dmx_dev_id = DMX_DEV_DVR;
    rec_open_params.segment_size = 0;/*infinite*/
    rec_open_params.max_size = 0;/*infinite*/
    rec_open_params.max_time = 0;/*infinite*/
    rec_open_params.event_fn = RecEventHandler;
    rec_open_params.event_userdata = "rec0";
    rec_open_params.flags = DVR_RECORD_FLAG_DATAOUT;
    rec_open_params.force_sysclock = DVR_TRUE;
    rec_open_params.flush_size = flush_size;
    snprintf(rec_open_params.location, DVR_MAX_LOCATION_SIZE, "%s", "dataout");

    error = dvr_wrapper_open_record(&recorder, &rec_open_params);
    if (error) {
      ERR( "recorder open fail = (0x%x)\n", error);
      return -1;
    }

    INF( "Starting %s recording %p [%ld secs/%llu bytes]\n",
       "dataout",
       recorder,
       rec_open_params.max_time,
       rec_open_params.max_size);

    Segment_DataoutCallback_t dataout = { data_cb, "datacb_priv"};
    error = dvr_wrapper_ioctl_record(recorder, SEGMENT_DATAOUT_CMD_SET_CALLBACK, &dataout, sizeof(dataout));
    if (error) {
        INF("Set dataout callback fail = (%#x)", error);
        dvr_wrapper_close_record(recorder);
        return -1;
    }

    memset(&rec_start_params, 0, sizeof(rec_start_params));

    pids_info = &rec_start_params.pids_info;
    pids_info->nb_pids = 1;
    pids_info->pids[0].pid = 0x2000;
    pids_info->pids[0].type = DVR_STREAM_TYPE_OTHER << 24;
    error = dvr_wrapper_start_record(recorder, &rec_start_params);
    if (error)
    {
      ERR( "recorder start fail = (0x%x)\n", error);
      dvr_wrapper_close_record(recorder);
      return -1;
    }

    return 0;
}

static void usage(int argc, char *argv[])
{
  INF( "Usage: record      : %s [tsin=n] [dataout=<stdout>]\n", argv[0]);
}


static void exit_helper() { if (dataout && dataout != stdout) fclose(dataout); }
void sig_handler(int signo)
{
  if (signo == SIGTERM)
    exit_helper();
  exit(0);
}

int main(int argc, char **argv)
{
    int error;
    int i;
    int helper = 0;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "tsin", 4))
            sscanf(argv[i], "tsin=%i", &ts_src);
        else if (!strncmp(argv[i], "dataout=stdout", 14))
            dataout = stdout;
        else if (!strncmp(argv[i], "flush=", 6))
            sscanf(argv[i], "flush=%i", &flush_size);
        else if (!strncmp(argv[i], "help", 4)) {
            usage(argc, argv);
            exit(0);
        }
    }

    if (argc == 1) {
      usage(argc, argv);
      exit(0);
    }

    if (helper) {
        atexit(&exit_helper);
        signal(SIGTERM, &sig_handler);
        signal(SIGINT, &sig_handler);
    }

    error = start_recording();
    if (error != 0) {
      ERR("start_recording failed with return value %d",error);
    }

    //command loop
    start_test();

    error = dvr_wrapper_stop_record(recorder);
    INF("stop record = (0x%x)\n", error);
    error = dvr_wrapper_close_record(recorder);
    INF("close record = (0x%x)\n", error);

    return 0;
}
