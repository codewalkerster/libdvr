#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "dvr_types.h"
#include "segment.h"

#define MAX_SEGMENT_FD_COUNT (128)
#define MAX_SEGMENT_PATH_SIZE (DVR_MAX_LOCATION_SIZE + 32)
#define MAX_PTS_THRESHOLD (10*1000)
#define PCR_RECORD_INTERVAL_MS (300)
#define PTS_DISCONTINUED_DEVIATION     (40)
#define PTS_HEAD_DEVIATION     (40)
#define PCR_JUMP_DUR     (5000)

#define IDX_FILE_SYNC_TIME    (10)//10*PCR_RECORD_INTERVAL_MS
#define TS_FILE_SYNC_TIME     (9)//9*PCR_RECORD_INTERVAL_MS


/**\brief Segment context*/
typedef struct {
  int             ts_fd;                              /**< Segment ts file fd*/
  FILE            *index_fp;                          /**< Time index file fd*/
  FILE            *dat_fp;                            /**< Information file fd*/
  FILE            *all_dat_fp;                            /**< Information file fd*/
  FILE            *ongoing_fp;                        /**< Ongoing file fd, used to verify timeshift mode*/
  uint64_t        first_pts;                          /**< First pts value, use for write mode*/
  uint64_t        last_pts;                           /**< Last input pts value, use for write mode*/
  uint64_t        last_record_pts;                    /**< Last record pts value, use for write mode*/
  uint64_t        cur_time;                           /**< Current time save in index file */
  uint64_t        segment_id;                         /**< Current segment ID */
  char            location[MAX_SEGMENT_PATH_SIZE];    /**< Current time save in index file */
  loff_t          first_offset;
  loff_t          last_offset;
  loff_t          last_record_offset;
  float           avg_rate;
  int             time;
  DVR_Bool_t      force_sysclock;                     /**< If ture, force to use system clock as PVR index time source. If false, libdvr can determine index time source based on actual situation*/
 } Segment_Context_t;

/**\brief Segment file type*/
typedef enum {
  SEGMENT_FILE_TYPE_TS,                       /**< Used for store TS data*/
  SEGMENT_FILE_TYPE_INDEX,                    /**< Used for store index data*/
  SEGMENT_FILE_TYPE_DAT,                      /**< Used for store information data, such as duration etc*/
  SEGMENT_FILE_TYPE_ONGOING,                  /**< Used for store information data, such as duration etc*/
  SEGMENT_FILE_TYPE_ALL_DATA,                  /**< Used for store all information data*/
} Segment_FileType_t;

static void segment_get_fname(char fname[MAX_SEGMENT_PATH_SIZE],
    const char location[DVR_MAX_LOCATION_SIZE],
    uint64_t segment_id,
    Segment_FileType_t type)
{
  int offset;

  offset = strlen(location);
  DVR_ASSERT(offset < DVR_MAX_LOCATION_SIZE);

  memset(fname, 0, MAX_SEGMENT_PATH_SIZE);
  strncpy(fname, location, offset);

  if (type != SEGMENT_FILE_TYPE_ALL_DATA) {
    strncpy(fname + offset, "-", 2);
    offset += 1;
    sprintf(fname + offset, "%04llu", segment_id);
    offset += strlen(fname + offset);
  }

  if (type == SEGMENT_FILE_TYPE_TS)
    strncpy(fname + offset, ".ts", 4);
  else if (type == SEGMENT_FILE_TYPE_INDEX)
    strncpy(fname + offset, ".idx", 5);
  else if (type == SEGMENT_FILE_TYPE_DAT)
    strncpy(fname + offset, ".dat", 5);
  else if (type == SEGMENT_FILE_TYPE_ONGOING)
    strncpy(fname + offset, ".going", 7);
  else if (type == SEGMENT_FILE_TYPE_ALL_DATA)
    strncpy(fname + offset, ".dat", 5);

}

static void segment_get_dirname(char dir_name[MAX_SEGMENT_PATH_SIZE],
    const char location[DVR_MAX_LOCATION_SIZE])
{
  char *p;
  int i;
  int found = 0;

  for (i = 0; i < (int)strlen(location); i++) {
    if (location[i] == '/') {
      p = (char *)location + i;
      found = 1;
    }
  }
  if (found)
    memcpy(dir_name, location, p - location);
}

int segment_open(Segment_OpenParams_t *params, Segment_Handle_t *p_handle)
{
  Segment_Context_t *p_ctx;
  char ts_fname[MAX_SEGMENT_PATH_SIZE];
  char index_fname[MAX_SEGMENT_PATH_SIZE];
  char dat_fname[MAX_SEGMENT_PATH_SIZE];
  char all_dat_fname[MAX_SEGMENT_PATH_SIZE];
  char dir_name[MAX_SEGMENT_PATH_SIZE];
  char going_name[MAX_SEGMENT_PATH_SIZE];
  int ret = 0;

  DVR_RETURN_IF_FALSE(params);
  DVR_RETURN_IF_FALSE(p_handle);

  //DVR_INFO("%s, location:%s, id:%llu", __func__, params->location, params->segment_id);

  p_ctx = (void*)malloc(sizeof(Segment_Context_t));
  DVR_RETURN_IF_FALSE(p_ctx);
  memset(p_ctx, 0, sizeof(Segment_Context_t));

  memset(ts_fname, 0, sizeof(ts_fname));
  segment_get_fname(ts_fname, params->location, params->segment_id, SEGMENT_FILE_TYPE_TS);

  memset(index_fname, 0, sizeof(index_fname));
  segment_get_fname(index_fname, params->location, params->segment_id, SEGMENT_FILE_TYPE_INDEX);

  memset(dat_fname, 0, sizeof(dat_fname));
  segment_get_fname(dat_fname, params->location, params->segment_id, SEGMENT_FILE_TYPE_DAT);

  memset(all_dat_fname, 0, sizeof(all_dat_fname));
  segment_get_fname(all_dat_fname, params->location, params->segment_id, SEGMENT_FILE_TYPE_ALL_DATA);


  memset(going_name, 0, sizeof(going_name));
  segment_get_fname(going_name, params->location, params->segment_id, SEGMENT_FILE_TYPE_ONGOING);

  memset(dir_name, 0, sizeof(dir_name));
  segment_get_dirname(dir_name, params->location);

  ret = mkdir(dir_name, 0666);
  if (ret == -1) {
    DVR_WARN("mkdir of %s returns %d due to errno:%d,%s",
        dir_name,ret,errno,strerror(errno));
  }
  if (access(dir_name, F_OK) == -1) {
    DVR_ERROR("%s dir %s does not exist", __func__, dir_name);
    free(p_ctx);
    return DVR_FAILURE;
  }

  if (params->mode == SEGMENT_MODE_READ) {
    p_ctx->ts_fd = open(ts_fname, O_RDONLY);
    p_ctx->index_fp = fopen(index_fname, "r");
    p_ctx->dat_fp = fopen(dat_fname, "r");
    p_ctx->ongoing_fp = NULL;
    p_ctx->all_dat_fp = fopen(all_dat_fname, "r");
  } else if (params->mode == SEGMENT_MODE_WRITE) {
    p_ctx->ts_fd = open(ts_fname, O_CREAT | O_RDWR | O_TRUNC, 0644);
    p_ctx->index_fp = fopen(index_fname, "w+");
    p_ctx->dat_fp = fopen(dat_fname, "w+");
    p_ctx->all_dat_fp = fopen(all_dat_fname, "a+");
    DVR_INFO("%s dir %s is opened", __func__, all_dat_fname);
    p_ctx->ongoing_fp = fopen(going_name, "w+");
    p_ctx->first_pts = ULLONG_MAX;
    p_ctx->last_pts = ULLONG_MAX;
    p_ctx->last_record_pts = ULLONG_MAX;
    p_ctx->avg_rate = 0.0;
  } else {
    DVR_INFO("%s, unknown mode use default", __func__);
    p_ctx->ts_fd = open(ts_fname, O_RDONLY);
    p_ctx->index_fp = fopen(index_fname, "r");
    p_ctx->dat_fp = fopen(dat_fname, "r");
    p_ctx->all_dat_fp = fopen(all_dat_fname, "r");
    p_ctx->ongoing_fp = NULL;
  }

  if (p_ctx->ts_fd == -1 || !p_ctx->index_fp || !p_ctx->dat_fp) {
    DVR_INFO("%s open file failed [%s, %s, %s], reason:%s", __func__,
        ts_fname, index_fname, dat_fname, strerror(errno));
    if (p_ctx->ts_fd != -1)
      close(p_ctx->ts_fd);
    if (p_ctx->index_fp)
      fclose(p_ctx->index_fp);
    if (p_ctx->dat_fp)
      fclose(p_ctx->dat_fp);
    if (p_ctx->all_dat_fp)
      fclose(p_ctx->all_dat_fp);
    if (p_ctx->ongoing_fp)
      fclose(p_ctx->ongoing_fp);
    free(p_ctx);
    *p_handle = NULL;
    return DVR_FAILURE;
  }
  p_ctx->segment_id = params->segment_id;
  strncpy(p_ctx->location, params->location, strlen(params->location)+1);
  p_ctx->force_sysclock = params->force_sysclock;

  //DVR_INFO("%s, open file success p_ctx->location [%s]", __func__, p_ctx->location, params->mode);
  *p_handle = (Segment_Handle_t)p_ctx;
  return DVR_SUCCESS;
}

int segment_close(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx;

  p_ctx = (void *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);

  if (p_ctx->ts_fd != -1) {
    close(p_ctx->ts_fd);
  }

  if (p_ctx->index_fp) {
    fclose(p_ctx->index_fp);
  }

  if (p_ctx->dat_fp) {
    fclose(p_ctx->dat_fp);
  }
  if (p_ctx->all_dat_fp) {
    fclose(p_ctx->all_dat_fp);
  }
  if (p_ctx->ongoing_fp != NULL) {
    fclose(p_ctx->ongoing_fp);
    char going_name[MAX_SEGMENT_PATH_SIZE];
    memset(going_name, 0, sizeof(going_name));
    segment_get_fname(going_name, p_ctx->location, p_ctx->segment_id, SEGMENT_FILE_TYPE_ONGOING);
    DVR_INFO("segment close del [%s]", going_name);
    unlink(going_name);
  }

  free(p_ctx);
  return 0;
}

ssize_t segment_read(Segment_Handle_t handle, void *buf, size_t count)
{
  Segment_Context_t *p_ctx;
  ssize_t len;
  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(buf);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd != -1);
  len = read(p_ctx->ts_fd, buf, count);
  return len;
}

ssize_t segment_write(Segment_Handle_t handle, void *buf, size_t count)
{
  Segment_Context_t *p_ctx;
  ssize_t len;
  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(buf);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd != -1);
  len = write(p_ctx->ts_fd, buf, count);
  /*remove the fsync, use /proc to control the data writeback*/
  //if (p_ctx->time % TS_FILE_SYNC_TIME == 0)
  //  fsync(p_ctx->ts_fd);
  return len;
}

int segment_update_pts_force(Segment_Handle_t handle, uint64_t pts, loff_t offset)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  int record_diff = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);

  if (p_ctx->first_pts == ULLONG_MAX) {
    DVR_INFO("%s first pcr:%llu", __func__, pts);
    p_ctx->first_pts = pts;
    p_ctx->first_offset = offset;
  }
  memset(buf, 0, sizeof(buf));
  if (p_ctx->last_pts == ULLONG_MAX) {
    /*Last pts is init value*/
    sprintf(buf, "{time=%llu, offset=%lld}", pts - p_ctx->first_pts, offset);
    p_ctx->cur_time = pts - p_ctx->first_pts;
    DVR_INFO("%s force pcr:%llu -1", __func__, pts);
  } else {
    /*Last pts has valid value*/
    int diff = pts - p_ctx->last_pts;
    if ((diff > MAX_PTS_THRESHOLD) || (diff < 0)) {
      /*Current pts has a transition*/
      DVR_INFO("[%s]force update Current pts has a transition, [%llu, %llu, %llu]",__func__,
          p_ctx->first_pts, p_ctx->last_pts, pts);
      sprintf(buf, "\n{time=%llu, offset=%lld}", p_ctx->cur_time, offset);
    } else {
      /*This is a normal pts, record it*/
      //check if this pcr is transition.if true,add 200ms
      //other case normal.
      p_ctx->cur_time += diff;
      DVR_INFO("%s force pcr:%llu -1 diff [%d]", __func__, pts, diff);
      sprintf(buf, "\n{time=%llu, offset=%lld}", p_ctx->cur_time, offset);
    }
  }

  record_diff = pts - p_ctx->last_record_pts;
  if (strlen(buf) > 0) {
    DVR_INFO("%s force pcr:%llu buf:%s", __func__, pts, buf);
    fputs(buf, p_ctx->index_fp);
    fflush(p_ctx->index_fp);
    //fsync(fileno(p_ctx->index_fp));
    p_ctx->last_record_pts = pts;
  }
  p_ctx->last_pts = pts;
  return DVR_SUCCESS;
}

int segment_update_pts(Segment_Handle_t handle, uint64_t pts, loff_t offset)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  int record_diff = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);

  if (p_ctx->first_pts == ULLONG_MAX) {
    DVR_INFO("%s first pcr:%llu", __func__, pts);
    p_ctx->first_pts = pts;
    //p_ctx->cur_time = p_ctx->cur_time + PTS_HEAD_DEVIATION;
  }
  memset(buf, 0, sizeof(buf));
  if (p_ctx->last_pts == ULLONG_MAX) {
    /*Last pts is init value*/
    sprintf(buf, "{time=%llu, offset=%lld}", pts - p_ctx->first_pts, offset);
    p_ctx->cur_time = pts - p_ctx->first_pts;
  } else {
    if (!p_ctx->force_sysclock) {
      /* if force_sysclock is off, we follow old manner. Please refer to
       * SWPL-75327*/
      /*Last pts has valid value*/
      int diff = pts - p_ctx->last_pts;
      if ((diff > MAX_PTS_THRESHOLD) || (diff < 0)) {
        /*Current pts has a transition*/
        DVR_INFO("Current pts has a transition, [%llu, %llu, %llu]",
            p_ctx->first_pts, p_ctx->last_pts, pts);
        p_ctx->last_record_pts = pts;
        //p_ctx->cur_time = p_ctx->cur_time + PTS_DISCONTINUED_DEVIATION;
      } else {
        /*This is a normal pts, record it*/
        loff_t off_diff = offset - p_ctx->last_offset;
        float rate = (float) (off_diff) / (float)(diff);
        if (p_ctx->avg_rate == 0.0) {
              p_ctx->avg_rate = (float) offset / (float)(p_ctx->cur_time + diff);
        }
        if (diff >= PCR_JUMP_DUR) {
          DVR_INFO("PTS TRANSITION ERROR last pcr[%llu]pts[%llu]pcr diff[%d]off[%llu]off_diff[%llu]rate[%f]avg rate[%f]4*rate[%d]av_diff[%d]",p_ctx->last_pts, pts, diff, offset,off_diff, rate, p_ctx->avg_rate, (int)(rate * 4), (int)(off_diff / p_ctx->avg_rate));
          if (p_ctx->avg_rate != 0 && (int)(p_ctx->avg_rate) >= (int)(rate * 4)) {
            diff = off_diff / p_ctx->avg_rate;
            p_ctx->cur_time += diff;
          } else {
            p_ctx->cur_time += diff;
          }
        } else {
           p_ctx->cur_time += diff;
        }
      }
    } else {
      /* if force_sysclock is on, we simply calculate cur_time based on system
       * time. Please refer to SWPL-75327*/
      p_ctx->cur_time = pts - p_ctx->first_pts;
    }
    sprintf(buf, "\n{time=%llu, offset=%lld}", p_ctx->cur_time, offset);
  }

  record_diff = pts - p_ctx->last_record_pts;
  if (strlen(buf) > 0 &&
      (record_diff > PCR_RECORD_INTERVAL_MS || p_ctx->last_record_pts == ULLONG_MAX)){
    fputs(buf, p_ctx->index_fp);
    fflush(p_ctx->index_fp);
    p_ctx->time++;
    //flush idx file 3s
    //if ((p_ctx->time > 0 && p_ctx->time % IDX_FILE_SYNC_TIME == 0))
    //  fsync(fileno(p_ctx->index_fp));
    if (p_ctx->time > IDX_FILE_SYNC_TIME)
      p_ctx->time = 0;
    p_ctx->last_record_pts = pts;
    p_ctx->last_record_offset = offset;
    if (p_ctx->cur_time > 0)
      p_ctx->avg_rate = (float) offset / (float)p_ctx->cur_time;
  }
  p_ctx->last_pts = pts;
  p_ctx->last_offset = offset;
  return DVR_SUCCESS;
}

loff_t segment_seek(Segment_Handle_t handle, uint64_t time, int block_size)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  char value[256];
  uint64_t pts = 0L;
  loff_t offset = 0;
  char *p1, *p2;
  int ret = 0;

  DVR_INFO("into seek time=%llu, offset=%lld time--%llu\n", pts, offset, time);

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd != -1);

  if (time == 0) {
    offset = 0;
    DVR_INFO("seek time=%llu, offset=%lld time--%llu\n", pts, offset, time);
    DVR_RETURN_IF_FALSE(lseek(p_ctx->ts_fd, offset, SEEK_SET) != -1);
    return offset;
  }

  memset(buf, 0, sizeof(buf));
  ret = fseek(p_ctx->index_fp, 0, SEEK_SET);
  DVR_RETURN_IF_FALSE(ret != -1);
  int line = 0;
  while (fgets(buf, sizeof(buf), p_ctx->index_fp) != NULL) {
    line++;
    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "time="))) {
      p1 += 5;
      if ((p2 = strstr(buf, ","))) {
        memcpy(value, p1, p2 - p1);
      }
      pts = strtoull(value, NULL, 10);
    }

    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "offset="))) {
      p1 += 7;
      if ((p2 = strstr(buf, "}"))) {
        memcpy(value, p1, p2 - p1);
      }
      offset = strtoull(value, NULL, 10);
    }
    if (0)
    {
      DVR_INFO("seek buf[%s]", buf);
      DVR_INFO("seek time=%llu, offset=%lld\n", pts, offset);
    }
    memset(buf, 0, sizeof(buf));
    if (time <= pts) {
      if (block_size > 0) {
        offset = offset - offset%block_size;
      }
      //DVR_INFO("seek time=%llu, offset=%lld time--%llu line %d\n", pts, offset, time, line);
      DVR_RETURN_IF_FALSE(lseek(p_ctx->ts_fd, offset, SEEK_SET) != -1);
      return offset;
    }
  }
  if (time > pts) {
    if (block_size > 0) {
      offset = offset - offset%block_size;
    }
    DVR_INFO("seek time=%llu, offset=%lld time--%llu line %d end\n", pts, offset, time, line);
    DVR_RETURN_IF_FALSE(lseek(p_ctx->ts_fd, offset, SEEK_SET) != -1);
    return offset;
  }
  DVR_INFO("seek error line [%d]", line);
  return DVR_FAILURE;
}

loff_t segment_tell_position(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx;
  loff_t pos;
  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd != -1);
  pos = lseek(p_ctx->ts_fd, 0, SEEK_CUR);
  return pos;
}

loff_t segment_tell_position_time(Segment_Handle_t handle, loff_t position)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  char value[256];
  uint64_t ret = 0L;
  uint64_t pts = 0L;
  uint64_t pts_p = 0L;
  loff_t offset = 0;
  loff_t offset_p = 0;
  char *p1, *p2;
  int ret2 = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd);

  memset(buf, 0, sizeof(buf));
  ret2 = fseek(p_ctx->index_fp, 0, SEEK_SET);
  DVR_RETURN_IF_FALSE(ret2 != -1);
  DVR_RETURN_IF_FALSE(position != -1);

  while (fgets(buf, sizeof(buf), p_ctx->index_fp) != NULL) {
    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "time="))) {
      p1 += 5;
      if ((p2 = strstr(buf, ","))) {
        memcpy(value, p1, p2 - p1);
      }
      pts = strtoull(value, NULL, 10);
    }

    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "offset="))) {
      p1 += 7;
      if ((p2 = strstr(buf, "}"))) {
        memcpy(value, p1, p2 - p1);
      }
      offset = strtoull(value, NULL, 10);
    }

    memset(buf, 0, sizeof(buf));
    //DVR_INFO("tell cur time=%llu, offset=%lld, position=%lld\n", pts, offset, position);
    if (position <= offset
        &&position >= offset_p
        && offset - offset_p > 0) {
      // Tainted data issue originating from fgets seem false positive, so we
      // just suppress it here.
      // coverity[tainted_data]
      ret = pts_p + (pts - pts_p) * (position - offset_p) / (offset - offset_p);
      //DVR_INFO("tell cur time=%llu, pts_p = %llu, offset=%lld, position=%lld offset_p+%lld\n", pts, pts_p, offset, position, offset_p);
      return ret;
    }
    offset_p = offset;
    pts_p = pts;
  }
  //DVR_INFO("tell cur time=%llu, offset=%lld, position=%lld\n", pts, offset, position);
  return pts;
}


loff_t segment_tell_current_time(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  char value[256];
  uint64_t pts = 0L;
  loff_t offset = 0, position = 0;
  char *p1, *p2;
  int ret = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd);

  memset(buf, 0, sizeof(buf));
  ret = fseek(p_ctx->index_fp, 0, SEEK_SET);
  DVR_RETURN_IF_FALSE(ret != -1);
  position = lseek(p_ctx->ts_fd, 0, SEEK_CUR);
  DVR_RETURN_IF_FALSE(position != -1);

  while (fgets(buf, sizeof(buf), p_ctx->index_fp) != NULL) {
    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "time="))) {
      p1 += 5;
      if ((p2 = strstr(buf, ","))) {
        memcpy(value, p1, p2 - p1);
      }
      pts = strtoull(value, NULL, 10);
    }

    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "offset="))) {
      p1 += 7;
      if ((p2 = strstr(buf, "}"))) {
        memcpy(value, p1, p2 - p1);
      }
      offset = strtoull(value, NULL, 10);
    }

    memset(buf, 0, sizeof(buf));
    //DVR_INFO("tell cur time=%llu, offset=%lld, position=%lld\n", pts, offset, position);
    if (position <= offset) {
      return pts;
    }
  }
  //DVR_INFO("tell cur time=%llu, offset=%lld, position=%lld\n", pts, offset, position);
  return pts;
}

loff_t segment_tell_total_time(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  char last_buf[256];
  char value[256];
  uint64_t pts = ULLONG_MAX;
  loff_t offset = 0, position = 0;
  char *p1, *p2;
  int line = 0;
  int ret = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd);

  memset(buf, 0, sizeof(buf));
  memset(last_buf, 0, sizeof(last_buf));
  position = lseek(p_ctx->ts_fd, 0, SEEK_CUR);
  DVR_RETURN_IF_FALSE(position != -1);

  // if unable to seek from end, it is necessary to seek to file beginning position.
  if (fseek(p_ctx->index_fp, -1000L, SEEK_END) == -1) {
    ret = fseek(p_ctx->index_fp, 0L, SEEK_SET);
    DVR_RETURN_IF_FALSE(ret != -1);
  }
  /* Save last line buffer */
  while (fgets(buf, sizeof(buf), p_ctx->index_fp) != NULL) {
    if (strlen(buf) <= 0) {
      DVR_INFO("read index buf is len 0");
      continue;
    }
    memset(last_buf, 0, sizeof(last_buf));
    memcpy(last_buf, buf, strlen(buf));
    memset(buf, 0, sizeof(buf));
    line++;
  }

  /* Extract time value */
  memset(value, 0, sizeof(value));
  if ((p1 = strstr(last_buf, "time="))) {
    p1 += 5;
    if ((p2 = strstr(last_buf, ","))) {
      memcpy(value, p1, p2 - p1);
    }
    pts = strtoull(value, NULL, 10);
  }

  memset(value, 0, sizeof(value));
  if ((p1 = strstr(last_buf, "offset="))) {
    p1 += 7;
    if ((p2 = strstr(last_buf, "}"))) {
      memcpy(value, p1, p2 - p1);
    }
    offset = strtoull(value, NULL, 10);
  }
  //if (line < 2)
  //DVR_INFO("totle time=%llu, offset=%lld, position=%lld, line:%d\n", pts, offset, position, line);
  return (pts == ULLONG_MAX ? DVR_FAILURE : pts);
}

/* Should consider the case of cut power, todo... */
int segment_store_info(Segment_Handle_t handle, Segment_StoreInfo_t *p_info)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  uint32_t i;
  int ret = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->dat_fp);
  DVR_RETURN_IF_FALSE(p_info);
  // seek to 0 to rewrite info
  ret = fseek(p_ctx->dat_fp, 0, SEEK_SET);
  DVR_RETURN_IF_FALSE(ret != -1);

  /*Save segment id*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "id=%lld\n", p_info->id);
  fputs(buf, p_ctx->dat_fp);

  /*Save number of pids*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "nb_pids=%d\n", p_info->nb_pids);
  fputs(buf, p_ctx->dat_fp);

  /*Save pid information*/
  for (i = 0; i < p_info->nb_pids; i++) {
    memset(buf, 0, sizeof(buf));
    sprintf(buf, "{pid=%d, type=%d}\n", p_info->pids[i].pid, p_info->pids[i].type);
    fputs(buf, p_ctx->dat_fp);
  }

  /*Save segment duration*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "duration=%ld\n", p_info->duration);
  fputs(buf, p_ctx->dat_fp);

  /*Save segment size*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "size=%zu\n", p_info->size);
  fputs(buf, p_ctx->dat_fp);

  /*Save number of packets*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "nb_packets=%d\n", p_info->nb_packets);
  fputs(buf, p_ctx->dat_fp);

  fflush(p_ctx->dat_fp);
  //fsync(fileno(p_ctx->dat_fp));
  return DVR_SUCCESS;
}

/* Should consider the case of cut power, todo... */
int segment_store_allInfo(Segment_Handle_t handle, Segment_StoreInfo_t *p_info)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  uint32_t i;
  int ret = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->all_dat_fp);
  DVR_RETURN_IF_FALSE(p_info);

  //seek to end to append info
  ret = fseek(p_ctx->all_dat_fp, 0, SEEK_END);
  DVR_RETURN_IF_FALSE(ret != -1);

  /*Save segment id*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "id=%lld\n", p_info->id);
  fputs(buf, p_ctx->all_dat_fp);

  /*Save number of pids*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "nb_pids=%d\n", p_info->nb_pids);
  fputs(buf, p_ctx->all_dat_fp);

  /*Save pid information*/
  for (i = 0; i < p_info->nb_pids; i++) {
    memset(buf, 0, sizeof(buf));
    sprintf(buf, "{pid=%d, type=%d}\n", p_info->pids[i].pid, p_info->pids[i].type);
    fputs(buf, p_ctx->all_dat_fp);
  }

  /*Save segment duration*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "duration=%ld\n", p_info->duration);
  fputs(buf, p_ctx->all_dat_fp);

  /*Save segment size*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "size=%zu\n", p_info->size);
  fputs(buf, p_ctx->all_dat_fp);

  /*Save number of packets*/
  memset(buf, 0, sizeof(buf));
  sprintf(buf, "nb_packets=%d\n", p_info->nb_packets);
  fputs(buf, p_ctx->all_dat_fp);

  fflush(p_ctx->all_dat_fp);
  //fsync(fileno(p_ctx->all_dat_fp));
  return DVR_SUCCESS;
}

/* Should consider the case of cut power, todo... */
int segment_load_info(Segment_Handle_t handle, Segment_StoreInfo_t *p_info)
{
  Segment_Context_t *p_ctx;
  uint32_t i;
  char buf[256];
  char value[256];
  char *p1, *p2;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_info);

  /*Load segment id*/
  p1 = fgets(buf, sizeof(buf), p_ctx->dat_fp);
  DVR_RETURN_IF_FALSE(p1);
  p1 = strstr(buf, "id=");
  DVR_RETURN_IF_FALSE(p1);
  p_info->id = strtoull(p1 + 3, NULL, 10);

  /*Save number of pids*/
  p1 = fgets(buf, sizeof(buf), p_ctx->dat_fp);
  DVR_RETURN_IF_FALSE(p1);
  p1 = strstr(buf, "nb_pids=");
  DVR_RETURN_IF_FALSE(p1);
  p_info->nb_pids = strtoul(p1 + 8, NULL, 10);

  /*Save pid information*/

  // Tainted data issue originating from fgets seem false positive, so we
  // just suppress it here.
  // coverity[tainted_data]
  for (i = 0; i < p_info->nb_pids; i++) {
    p1 = fgets(buf, sizeof(buf), p_ctx->dat_fp);
    DVR_RETURN_IF_FALSE(p1);
    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "pid="))) {
      DVR_RETURN_IF_FALSE(p1);
      p1 += 4;
      if ((p2 = strstr(buf, ","))) {
        DVR_RETURN_IF_FALSE(p2);
        memcpy(value, p1, p2 - p1);
      }
      p_info->pids[i].pid = strtoull(value, NULL, 10);
    }

    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "type="))) {
      DVR_RETURN_IF_FALSE(p1);
      p1 += 5;
      if ((p2 = strstr(buf, "}"))) {
        DVR_RETURN_IF_FALSE(p2);
        memcpy(value, p1, p2 - p1);
      }
      p_info->pids[i].type = strtoull(value, NULL, 10);
    }
  }

  /*Save segment duration*/
  p1 = fgets(buf, sizeof(buf), p_ctx->dat_fp);
  DVR_RETURN_IF_FALSE(p1);
  p1 = strstr(buf, "duration=");
  DVR_RETURN_IF_FALSE(p1);
  p_info->duration = strtoull(p1 + 9, NULL, 10);
  //DVR_INFO("load info p_info->duration:%lld", p_info->duration);

  /*Save segment size*/
  p1 = fgets(buf, sizeof(buf), p_ctx->dat_fp);
  DVR_RETURN_IF_FALSE(p1);
  p1 = strstr(buf, "size=");
  DVR_RETURN_IF_FALSE(p1);
  p_info->size = strtoull(p1 + 5, NULL, 10);

  /*Save number of packets*/
  p1 = fgets(buf, sizeof(buf), p_ctx->dat_fp);
  DVR_RETURN_IF_FALSE(p1);
  p1 = strstr(buf, "nb_packets=");
  DVR_RETURN_IF_FALSE(p1);
  p_info->nb_packets = strtoull(p1 + 11, NULL, 10);

  return DVR_SUCCESS;
}

/* Should consider the case of cut power, todo... */
int segment_load_allInfo(Segment_Handle_t handle, struct list_head *list)
{
  Segment_Context_t *p_ctx;
  uint32_t i;
  char buf[256];
  char value[256];
  char *p1, *p2;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(list);
  if (p_ctx->all_dat_fp == NULL) {
    DVR_INFO("all dat file not open\n");
    return DVR_FAILURE;
  }
  //first get
  p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
  DVR_RETURN_IF_FALSE(p1);

  do {

    DVR_RecordSegmentInfo_t *p_info;

    p_info = malloc(sizeof(DVR_RecordSegmentInfo_t));
    DVR_RETURN_IF_FALSE(p_info);
    memset(p_info, 0, sizeof(DVR_RecordSegmentInfo_t));

    list_add_tail(p_info, list);

    /*Load segment id*/
    DVR_RETURN_IF_FALSE(p1);
    p1 = strstr(buf, "id=");
    DVR_RETURN_IF_FALSE(p1);
    p_info->id = strtoull(p1 + 3, NULL, 10);

    /*Save number of pids*/
    p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
    DVR_RETURN_IF_FALSE(p1);
    p1 = strstr(buf, "nb_pids=");
    DVR_RETURN_IF_FALSE(p1);
    p_info->nb_pids = strtoull(p1 + 8, NULL, 10);

    /*Save pid information*/
    // Tainted data issue originating from fgets seem false positive, so we
    // just suppress it here.
    // coverity[tainted_data]
    for (i = 0; i < p_info->nb_pids; i++) {
      p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
      DVR_RETURN_IF_FALSE(p1);
      memset(value, 0, sizeof(value));
      if ((p1 = strstr(buf, "pid="))) {
        DVR_RETURN_IF_FALSE(p1);
        p1 += 4;
        if ((p2 = strstr(buf, ","))) {
          DVR_RETURN_IF_FALSE(p2);
          memcpy(value, p1, p2 - p1);
        }
        p_info->pids[i].pid = strtoull(value, NULL, 10);
      }

      memset(value, 0, sizeof(value));
      if ((p1 = strstr(buf, "type="))) {
        DVR_RETURN_IF_FALSE(p1);
        p1 += 5;
        if ((p2 = strstr(buf, "}"))) {
          DVR_RETURN_IF_FALSE(p2);
          memcpy(value, p1, p2 - p1);
        }
        p_info->pids[i].type = strtoull(value, NULL, 10);
      }
    }

    /*Save segment duration*/
    p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
    DVR_RETURN_IF_FALSE(p1);
    p1 = strstr(buf, "duration=");
    DVR_RETURN_IF_FALSE(p1);
    p_info->duration = strtoull(p1 + 9, NULL, 10);
    //DVR_INFO("load info p_info->duration:%lld", p_info->duration);

    /*Save segment size*/
    p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
    DVR_RETURN_IF_FALSE(p1);
    p1 = strstr(buf, "size=");
    DVR_RETURN_IF_FALSE(p1);
    p_info->size = strtoull(p1 + 5, NULL, 10);

    /*Save number of packets*/
    p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
    DVR_RETURN_IF_FALSE(p1);
    p1 = strstr(buf, "nb_packets=");
    DVR_RETURN_IF_FALSE(p1);
    p_info->nb_packets = strtoull(p1 + 11, NULL, 10);
    //if reach end,exit loop
    p1 = fgets(buf, sizeof(buf), p_ctx->all_dat_fp);
  } while (p1);

  return DVR_SUCCESS;
}

int segment_delete(const char *location, uint64_t segment_id)
{
  char fname[MAX_SEGMENT_PATH_SIZE];
  int ret;

  DVR_RETURN_IF_FALSE(location);

  /*delete ts file*/
  memset(fname, 0, sizeof(fname));
  segment_get_fname(fname, location, segment_id, SEGMENT_FILE_TYPE_TS);
  ret = unlink(fname);
  DVR_ERROR("%s, [%s] return:%s", __func__, fname, strerror(errno));
  DVR_RETURN_IF_FALSE(ret == 0);

  /*delete index file*/
  memset(fname, 0, sizeof(fname));
  segment_get_fname(fname, location, segment_id, SEGMENT_FILE_TYPE_INDEX);
  unlink(fname);
  DVR_ERROR("%s, [%s] return:%s", __func__, fname, strerror(errno));
  DVR_RETURN_IF_FALSE(ret == 0);

  /*delete store information file*/
  memset(fname, 0, sizeof(fname));
  segment_get_fname(fname, location, segment_id, SEGMENT_FILE_TYPE_DAT);
  unlink(fname);
  DVR_ERROR("%s, [%s] return:%s", __func__, fname, strerror(errno));
  DVR_RETURN_IF_FALSE(ret == 0);

  return DVR_SUCCESS;
}

int segment_ongoing(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx;
  p_ctx = (Segment_Context_t *)handle;
  struct stat mstat;

  char going_name[MAX_SEGMENT_PATH_SIZE];
  memset(going_name, 0, sizeof(going_name));
  segment_get_fname(going_name, p_ctx->location, p_ctx->segment_id, SEGMENT_FILE_TYPE_ONGOING);
  int ret = stat(going_name, &mstat);
  DVR_INFO("segment check ongoing  [%s] ret [%d]", going_name, ret);
  if (ret != 0) {
    return DVR_FAILURE;
  }
  return DVR_SUCCESS;
}

loff_t segment_dump_pts(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx;
  char buf[256];
  char value[256];
  uint64_t pts = 0;
  loff_t offset = 0;
  char *p1, *p2;
  int ret = 0;

  p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->index_fp);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd != -1);

  memset(buf, 0, sizeof(buf));
  ret = fseek(p_ctx->index_fp, 0, SEEK_SET);
  DVR_RETURN_IF_FALSE(ret != -1);

  printf("start gets pts\n");
  while (fgets(buf, sizeof(buf), p_ctx->index_fp) != NULL) {
    printf("buf[%s]\n", buf);
    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "time="))) {
      p1 += 5;
      if ((p2 = strstr(buf, ","))) {
        memcpy(value, p1, p2 - p1);
      }
      pts = strtoull(value, NULL, 10);
    }

    memset(value, 0, sizeof(value));
    if ((p1 = strstr(buf, "offset="))) {
      p1 += 7;
      if ((p2 = strstr(buf, "}"))) {
        memcpy(value, p1, p2 - p1);
      }
      offset = strtoull(value, NULL, 10);
    }

    memset(buf, 0, sizeof(buf));
    printf("pts=%llu, offset=%lld\n", pts, offset);
  }

  return 0;
}

off_t segment_get_cur_segment_size(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  DVR_RETURN_IF_FALSE(p_ctx->ts_fd != -1);
  struct stat sb;
  int ret=fstat(p_ctx->ts_fd,&sb);
  if (ret<0) {
    return -1;
  }
  return sb.st_size;
}

uint64_t segment_get_cur_segment_id(Segment_Handle_t handle)
{
  Segment_Context_t *p_ctx = (Segment_Context_t *)handle;
  DVR_RETURN_IF_FALSE(p_ctx);
  return p_ctx->segment_id;
}

