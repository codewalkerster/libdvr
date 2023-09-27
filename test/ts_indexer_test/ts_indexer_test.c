/**
 * \page ts_indexer_test
 * \section Introduction
 * test code with ts_indexer_xxxxx APIs.
 * It supports:
 * \li parse pts/i-frame from MPEG2/H264/HEVC transport stream
 *
 * \section Usage
 *
 * \li ts: bitstream file path
 * \li vpid: the pid of video bitstream with pts/i-frame
 * \li vfmt: the video format
 * \li apid: the pid of audio bitstream with pts
 *
 *
 * parse pts/i-frame:
 * \code
 *    ts_indexer_test [ts=] [vpid=] [vfmt=] [apid=]
 * \endcode
 *
 * \endsection
 */

#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_indexer.h"

#define INF(fmt, ...) fprintf(stdout, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

enum {
  INDEX_PUSI      = 0x01,
  INDEX_IFRAME    = 0x02,
  INDEX_PTS       = 0x04
};

#define has_pusi(_m_)    ((_m_) & INDEX_PUSI)
#define has_iframe(_m_) ((_m_) & INDEX_IFRAME)
#define has_pts(_m_)    ((_m_) & INDEX_PTS)
#define MAX_CACHE_SIZE  (2*1024*1024)

typedef struct
{
  uint8_t *base_ptr;
  uint8_t *last_pusi_ptr;
  uint64_t last_pusi_offset;
  uint64_t cnt;
  uint64_t last_pts;
  uint32_t flags;
  uint8_t *cache_data;
  uint64_t cache_len;
  FILE *dump_file;
} ts_indexer_ctl;

static ts_indexer_ctl gControl;

int send_data()
{
  ts_indexer_ctl *ctl = &gControl;

  if (!has_pusi(ctl->flags)) {
    ERR("no PUSI, should not send\n");
    return -1;
  }
  //send the cache data
  if (ctl->dump_file) {
    INF("send %#lx bytes, PUSI: %d, IFRAME: %d, PTS: %d\n",
        ctl->cache_len,
        has_pusi(ctl->flags),
        has_iframe(ctl->flags),
        has_pts(ctl->flags));
    fwrite(&ctl->cache_data[0], 1, ctl->cache_len, ctl->dump_file);
  }

  return 0;
}

static void ts_indexer_event_cb(TS_Indexer_t *ts_indexer, TS_Indexer_Event_t *event)
{
  static int cnt = 0;
  int margin_len;
  ts_indexer_ctl *ctl = &gControl;

  if (ts_indexer == NULL || event == NULL)
    return;

  if (!has_iframe(ctl->flags) &&
         !has_pts(ctl->flags) &&
         !has_pusi(ctl->flags) &&
         event->type != TS_INDEXER_EVENT_TYPE_START_INDICATOR) {
    INF("wait the PUSI come %d...\n", event->type);
    return;
  }

  if (ctl->last_pusi_offset <= 0) {
    if (event->type == TS_INDEXER_EVENT_TYPE_START_INDICATOR) {
      ctl->last_pusi_offset = ts_indexer->offset;
      ctl->last_pusi_ptr = ctl->base_ptr + (ts_indexer->offset - ctl->cnt);
      INF("PUSI %d\n", cnt++);
      return;
    } else {
      ERR("unexpected event: %d\n", event->type);
      return;
    }
  }

  switch (event->type) {
    case TS_INDEXER_EVENT_TYPE_MPEG2_I_FRAME:
    case TS_INDEXER_EVENT_TYPE_AVC_I_SLICE:
    case TS_INDEXER_EVENT_TYPE_HEVC_IDR_W_RADL:
      ctl->flags |= INDEX_IFRAME;
      #if 0
      INF("pid: %#x ", event->pid);
      INF("TS_INDEXER_EVENT_TYPE_I_FRAME, offset: %lx, pts: %lx\n",
            event->offset, event->pts);
      #endif
      break;

    case TS_INDEXER_EVENT_TYPE_VIDEO_PTS:
      ctl->flags |= INDEX_PTS;
      #if 0
      INF("PID: %#x ", event->pid);
      INF("TS_INDEXER_EVENT_TYPE_VIDEO_PTS, Offset: %lx, Lastoffset: %lx, Pts: %lx\n",
            event->offset, gControl.last_offset, event->pts);
      gControl.last_offset = ts_indexer->offset;
      #endif
      break;

    case TS_INDEXER_EVENT_TYPE_AUDIO_PTS:
      break;

    case TS_INDEXER_EVENT_TYPE_START_INDICATOR:
      /*
       * it does means this is the first PUSI on current block if there're
       * cache data
       */
      if (ctl->cache_len > 0) {
        // the length from block head to the pusi position
        margin_len = ts_indexer->offset - ctl->cnt;
        memcpy(&ctl->cache_data[ctl->cache_len], ctl->base_ptr, margin_len);
        ctl->cache_len += margin_len;
        ctl->last_pusi_ptr = ctl->base_ptr + margin_len;
      } else {
        // the length between the last PUSI and current PUSI
        margin_len = ts_indexer->offset - ctl->last_pusi_offset;
        memcpy(&ctl->cache_data[0], ctl->last_pusi_ptr, margin_len);
        ctl->cache_len = margin_len;
        ctl->last_pusi_ptr += margin_len;
      }

      send_data();
      ctl->flags = INDEX_PUSI;
      ctl->last_pusi_offset = ts_indexer->offset;
      ctl->cache_len = 0;
      break;
    case TS_INDEXER_EVENT_TYPE_DISCONTINUITY_INDICATOR:
    case TS_INDEXER_EVENT_TYPE_MPEG2_P_FRAME:
    case TS_INDEXER_EVENT_TYPE_MPEG2_B_FRAME:
    case TS_INDEXER_EVENT_TYPE_MPEG2_SEQUENCE:
    case TS_INDEXER_EVENT_TYPE_AVC_P_SLICE:
    case TS_INDEXER_EVENT_TYPE_AVC_B_SLICE:
    case TS_INDEXER_EVENT_TYPE_AVC_SI_SLICE:
    case TS_INDEXER_EVENT_TYPE_AVC_SP_SLICE:
    case TS_INDEXER_EVENT_TYPE_HEVC_SPS:
    case TS_INDEXER_EVENT_TYPE_HEVC_AUD:
    case TS_INDEXER_EVENT_TYPE_HEVC_BLA_W_LP:
    case TS_INDEXER_EVENT_TYPE_HEVC_BLA_W_RADL:
    case TS_INDEXER_EVENT_TYPE_HEVC_IDR_N_LP:
    case TS_INDEXER_EVENT_TYPE_HEVC_TRAIL_CRA:
      //INF("type: %d, offset: %lx\n", event->type, event->offset);
      break;

    default:
      break;
  }

  return;
}

static char *help_vfmt =
  "\n\t0:TS_INDEXER_VIDEO_FORMAT_MPEG2" /**< MPEG2 video.*/
  "\n\t1:TS_INDEXER_VIDEO_FORMAT_H264" /**< H264.*/
  "\n\t2:TS_INDEXER_VIDEO_FORMAT_HEVC"; /**<HEVC.*/

static void usage(int argc, char *argv[])
{
  INF("Usage: %s [ts=] [vpid=] [vfmt=] [apid=]\n", argv[0]);
  INF("Usage: %s\n", help_vfmt);
}

int main(int argc, char **argv)
{
  int i;
  char file[512];
  int vpid = 0x1fff;
  int apid = 0x1fff;
  int vfmt = -1;
  TS_Indexer_t ts_indexer;
  int block_size = 188*1024;
  ts_indexer_ctl *ctl = &gControl;

  memset(&file[0], 0, sizeof(file));
  for (i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "ts=", 3))
      sscanf(argv[i], "ts=%s", &file[0]);
    else if (!strncmp(argv[i], "vpid=", 5))
      sscanf(argv[i], "vpid=%i", &vpid);
    else if (!strncmp(argv[i], "vfmt=", 5))
      sscanf(argv[i], "vfmt=%i", &vfmt);
    else if (!strncmp(argv[i], "apid=", 5))
      sscanf(argv[i], "apid=%i", &apid);
    else if (!strncmp(argv[i], "help", 4)) {
      usage(argc, argv);
      exit(0);
    }
  }

  if (argc == 1 ||
    (vpid == 0x1fff && apid == 0x1fff))
  {
    usage(argc, argv);
    exit(0);
  }

  memset(&ts_indexer, 0, sizeof(TS_Indexer_t));
  ts_indexer_init(&ts_indexer);
  ts_indexer_set_video_pid(&ts_indexer, vpid);
  ts_indexer_set_audio_pid(&ts_indexer, apid);
  ts_indexer_set_video_format(&ts_indexer, vfmt);
  ts_indexer_set_event_callback(&ts_indexer, ts_indexer_event_cb);

  FILE *f = fopen(file, "rb");
  if (f == NULL) {
    ERR("open %s failed!\n", file);
    return -1;
  }

  uint8_t *data = malloc(block_size);
  if (data == NULL) {
    ERR("no heap memory!\n");
    return -1;
  }

  size_t len = 0;
  INF("vpid: %#x, vfmt: %d, apid:%#x\n", vpid, vfmt, apid);
  memset(ctl, 0, sizeof(ts_indexer_ctl));
  ctl->last_pts = -1;
  ctl->last_pusi_offset = 0;
  ctl->cache_data = malloc(MAX_CACHE_SIZE);
  if (ctl->cache_data == NULL) {
    ERR("malloc cache failed\n");
    return -1;
  }
  ctl->dump_file = fopen("./dump.ts", "wb+");
  if (ctl->dump_file == NULL) {
    ERR("open dump file failed\n");
    return -1;
  }

  while (1) {
    len = fread(data, 1, block_size, f);
    if (len <= 0)
      break;

    ctl->base_ptr = &data[0];
    ts_indexer_parse(&ts_indexer, data, len);

    int offset = ctl->cache_len;
    /*
     * cache the data from PUSI position to the end of the block if current
     * block has PUSI.
     * cache the whole block data if current block has no PUSI.
     */

    if (ctl->cache_len == 0 && ctl->last_pusi_ptr) {
      int pusi_to_end_len = ((ctl->base_ptr + len) - ctl->last_pusi_ptr);
      if (pusi_to_end_len > 0)
        memcpy(&ctl->cache_data[offset], ctl->last_pusi_ptr, pusi_to_end_len);
      ctl->cache_len += pusi_to_end_len;
    } else {
      memcpy(&ctl->cache_data[offset], ctl->base_ptr, len);
      ctl->cache_len += len;
    }
    ctl->cnt += len;
    ctl->last_pusi_ptr = NULL;
  }
  if (ctl->cache_data) {
    free(ctl->cache_data);
  }
  fflush(ctl->dump_file);
  fclose(ctl->dump_file);

  return 0;
}
