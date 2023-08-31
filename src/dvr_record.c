#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "dvr_types.h"
#include "dvr_record.h"
#include "dvr_crypto.h"
#include "dvb_utils.h"
#include "record_device.h"
#include <sys/time.h>
#include <sys/prctl.h>
#include "am_crypt.h"

#include "segment.h"
#include "segment_dataout.h"

#define CHECK_PTS_MAX_COUNT  (20)

//#define DEBUG_PERFORMANCE
#define MAX_DVR_RECORD_SESSION_COUNT 4
#define RECORD_BLOCK_SIZE (256 * 1024)
#define NEW_DEVICE_RECORD_BLOCK_SIZE (1024 * 188)

/**\brief DVR index file type*/
typedef enum {
  DVR_INDEX_TYPE_PCR,                                                   /**< DVR index file use pcr*/
  DVR_INDEX_TYPE_LOCAL_CLOCK,                                           /**< DVR index file use local clock*/
  DVR_INDEX_TYPE_INVALID                                                 /**< DVR index file type invalid type*/
} DVR_IndexType_t;

/**\brief DVR VOD context*/
typedef struct {
  pthread_mutex_t                 mutex;                                /**< VOD mutex lock*/
  pthread_cond_t                  cond;                                 /**< VOD condition*/
  void                            *buffer;                              /**< VOD buffer*/
  uint32_t                        buf_len;                              /**< VOD buffer len*/
} DVR_VodContext_t;

/**\brief DVR record secure mode buffer*/
typedef struct {
  uint32_t addr;                                                        /**< Secure mode record buffer address*/
  uint32_t len;                                                         /**< Secure mode record buffer length*/
} DVR_SecureBuffer_t;

/**\brief DVR record new dmx secure mode buffer*/
typedef struct {
  uint32_t buf_start;                                                        /**< Secure mode record buffer address*/
  uint32_t buf_end;                                                         /**< Secure mode record buffer length*/
  uint32_t data_start;                                                        /**< Secure mode record buffer address*/
  uint32_t data_end;                                                         /**< Secure mode record buffer length*/
} DVR_NewDmxSecureBuffer_t;

/**\brief DVR record context*/
typedef struct {
  pthread_t                       thread;                               /**< DVR thread handle*/
  Record_DeviceHandle_t           dev_handle;                           /**< DVR device handle*/
  Segment_Handle_t                segment_handle;                       /**< DVR segment handle*/
  DVR_RecordState_t               state;                                /**< DVR record state*/
  char                            location[DVR_MAX_LOCATION_SIZE];      /**< DVR record file location*/
  DVR_RecordSegmentStartParams_t  segment_params;                       /**< DVR record start parameters*/
  DVR_RecordSegmentInfo_t         segment_info;                         /**< DVR record current segment info*/
  size_t                          notification_size;                    /**< DVR record notification size*/
  DVR_RecordEventFunction_t       event_notify_fn;                      /**< DVR record event notify function*/
  void                            *event_userdata;                      /**< DVR record event userdata*/
  //DVR_VodContext_t                vod;                                  /**< DVR record vod context*/
  int                             is_vod;                               /**< Indicate current mode is VOD record mode*/
  DVR_CryptoFunction_t            enc_func;                             /**< Encrypt function*/
  void                            *enc_userdata;                        /**< Encrypt userdata*/
  int                             is_secure_mode;                       /**< Record session run in secure pipeline */
  void                            *cryptor;                             /**< Cryptor for encrypted PVR on FTA.*/
  size_t                          last_send_size;                       /**< Last send notify segment size */
  uint32_t                        block_size;                           /**< DVR record block size */
  DVR_Bool_t                      is_new_dmx;                           /**< DVR is used new dmx driver */
  int                             index_type;                           /**< DVR is used pcr or local time */
  DVR_Bool_t                      force_sysclock;                       /**< If ture, force to use system clock as PVR index time source. If false, libdvr can determine index time source based on actual situation*/
  uint64_t                        pts;                                  /**< The newest pcr or local time */
  int                             check_pts_count;                      /**< The check count of pts */
  int                             check_no_pts_count;                   /**< The check count of no pts */
  int                             notification_time;                    /**< DVR record notification time*/
  time_t                          last_send_time;                       /**< Last send notify segment duration */
  loff_t                          guarded_segment_size;                 /**< Guarded segment size in bytes. Libdvr will be forcely stopped to write anymore if current segment reaches this size*/
  size_t                          secbuf_size;                          /**< DVR record secure buffer length*/
  DVR_Bool_t                      discard_coming_data;                  /**< Whether to discard subsequent recording data due to exceeding total size limit too much.*/
  Segment_Ops_t                   segment_ops;
  struct list_head                segment_ctrls;
} DVR_RecordContext_t;

typedef struct {
  struct list_head head;
  unsigned int cmd;
  void *data;
  size_t size;
} DVR_Control_t;

#define SEG_CALL_INIT(_ops) Segment_Ops_t *ops = (_ops)
#define SEG_CALL_RET_VALID(_name, _args, _ret, _def_ret) do {\
    if (ops->segment_##_name)\
      (_ret) = ops->segment_##_name _args;\
    else\
      (_ret) = (_def_ret);\
  } while(0);
#define SEG_CALL_RET(_name, _args, _ret) SEG_CALL_RET_VALID(_name, _args, _ret, _ret)
#define SEG_CALL(_name, _args) do {\
    if (ops->segment_##_name)\
        ops->segment_##_name _args;\
  } while(0)
#define SEG_CALL_IS_VALID(_name) (!!ops->segment_##_name)

extern ssize_t record_device_read_ext(Record_DeviceHandle_t handle, size_t *buf, size_t *len);

static DVR_RecordContext_t record_ctx[MAX_DVR_RECORD_SESSION_COUNT] = {
  {
    .state = DVR_RECORD_STATE_CLOSED
  },
  {
    .state = DVR_RECORD_STATE_CLOSED
  }
};


static int record_set_segment_ops(DVR_RecordContext_t *p_ctx, int flags)
{
  Segment_Ops_t *ops = &p_ctx->segment_ops;

  memset(ops, 0, sizeof(Segment_Ops_t));

  if (flags & DVR_RECORD_FLAG_DATAOUT) {
    DVR_INFO("%s segment mode: dataout", __func__);
    #define _SET(_op)\
      ops->segment_##_op = segment_dataout_##_op
    _SET(open);
    _SET(close);
    _SET(ioctl);
    _SET(write);
    _SET(update_pts);
    _SET(update_pts_force);
    _SET(tell_position);
    _SET(tell_total_time);
    _SET(store_info);
    _SET(store_allInfo);
    #undef _SET
  } else {
    #define _SET(_op)\
      ops->segment_##_op = segment_##_op
    _SET(open);
    _SET(close);
    _SET(read);
    _SET(write);
    _SET(update_pts);
    _SET(update_pts_force);
    _SET(seek);
    _SET(tell_position);
    _SET(tell_position_time);
    _SET(tell_current_time);
    _SET(tell_total_time);
    _SET(store_info);
    _SET(store_allInfo);
    _SET(load_info);
    _SET(load_allInfo);
    _SET(delete);
    _SET(ongoing);
    _SET(get_cur_segment_size);
    _SET(get_cur_segment_id);
    #undef _SET
  }
  return DVR_SUCCESS;
}

static int record_is_valid_pid(DVR_RecordContext_t *p_ctx, int pid)
{
  int i;

  for (i = 0; i < p_ctx->segment_info.nb_pids; i++) {
    if (pid == p_ctx->segment_info.pids[i].pid)
      return 1;
  }
  return 0;
}

static int record_save_pcr(DVR_RecordContext_t *p_ctx, uint8_t *buf, loff_t pos)
{
  uint8_t *p = buf;
  int len;
  uint8_t afc;
  uint64_t pcr = 0;
  int has_pcr = 0;
  int pid;
  int adp_field_len;

  SEG_CALL_INIT(&p_ctx->segment_ops);

  pid = ((p[1] & 0x1f) << 8) | p[2];
  if (pid == 0x1fff || !record_is_valid_pid(p_ctx, pid))
    return has_pcr;

  //scramble = p[3] >> 6;
  //cc = p[3] & 0x0f;
  afc = (p[3] >> 4) & 0x03;

  p += 4;
  len = 184;

  if (afc & 2) {
    adp_field_len = p[0];
    /* Skip adaptation len */
    p++;
    len--;
    /* Parse pcr field, see 13818 spec table I-2-6,adaptation_field */
    if (p[0] & 0x10 && len >= 6 && adp_field_len >= 6) {
    /* get pcr value,pcr is 33bit value */
    pcr = (((uint64_t)(p[1])) << 25)
        | (((uint64_t)p[2]) << 17)
        | (((uint64_t)(p[3])) << 9)
        | (((uint64_t)p[4]) << 1)
        | ((((uint64_t)p[5]) & 0x80) >> 7);
      has_pcr = 1;
    }

    len -= adp_field_len;

    if (len < 0) {
      DVR_INFO("parser pcr: illegal adaptation field length");
      return 0;
    }
  }

  if (has_pcr && p_ctx->index_type == DVR_INDEX_TYPE_PCR) {
    //save newest pcr
    if (p_ctx->pts == pcr/90 &&
      p_ctx->check_pts_count < CHECK_PTS_MAX_COUNT) {
      p_ctx->check_pts_count ++;
    }
    p_ctx->pts = pcr/90;

    SEG_CALL(update_pts, (p_ctx->segment_handle, pcr/90, pos));
  }
  return has_pcr;
}

static int record_do_pcr_index(DVR_RecordContext_t *p_ctx, uint8_t *buf, int len)
{
  uint8_t *p = buf;
  int left = len;
  loff_t pos;
  int has_pcr = 0;

  SEG_CALL_INIT(&p_ctx->segment_ops);

  SEG_CALL_RET_VALID(tell_position, (p_ctx->segment_handle), pos, -1);

  if (pos == -1)
      return has_pcr;

  if (pos >= len) {
    pos = pos - len;
  }
  while (left >= 188) {
    if (*p == 0x47) {
      has_pcr |= record_save_pcr(p_ctx, p, pos);
      p += 188;
      left -= 188;
      pos += 188;
    } else {
      p++;
      left --;
      pos++;
    }
  }
  return has_pcr;
}

static int get_diff_time(struct timeval start_tv, struct timeval end_tv)
{
  return end_tv.tv_sec * 1000 + end_tv.tv_usec / 1000 - start_tv.tv_sec * 1000 - start_tv.tv_usec / 1000;
}

void *record_thread(void *arg)
{
  DVR_RecordContext_t *p_ctx = (DVR_RecordContext_t *)arg;
  ssize_t len;
  uint8_t *buf, *buf_out;
  uint32_t block_size = p_ctx->block_size;
  loff_t pos = 0;
  int ret = DVR_SUCCESS;
  struct timespec start_ts, end_ts, start_no_pcr_ts, end_no_pcr_ts;
  DVR_RecordStatus_t record_status;
  int has_pcr;
  int pcr_rec_len = 0;
  DVR_Bool_t guarded_size_exceeded = DVR_FALSE;

  time_t pre_time = 0;
  #define DVR_STORE_INFO_TIME (400)
  DVR_SecureBuffer_t secure_buf = {0,0};
  DVR_NewDmxSecureBuffer_t new_dmx_secure_buf;
  int first_read = 0;

  SEG_CALL_INIT(&p_ctx->segment_ops);

  prctl(PR_SET_NAME,"DvrRecording");

  // Force to use LOCAL_CLOCK as index type if force_sysclock is on. Please
  // refer to SWPL-75327
  if (p_ctx->force_sysclock)
    p_ctx->index_type = DVR_INDEX_TYPE_LOCAL_CLOCK;
  else
    p_ctx->index_type = DVR_INDEX_TYPE_INVALID;
  buf = (uint8_t *)malloc(block_size);
  if (!buf) {
    DVR_INFO("%s, malloc failed", __func__);
    return NULL;
  }

  if (p_ctx->is_secure_mode) {
    buf_out = (uint8_t *)malloc(p_ctx->secbuf_size + 188);
  } else {
    buf_out = (uint8_t *)malloc(block_size + 188);
  }
  if (!buf_out) {
    DVR_INFO("%s, malloc failed", __func__);
    free(buf);
    return NULL;
  }

  memset(&record_status, 0, sizeof(record_status));
  record_status.state = DVR_RECORD_STATE_STARTED;
  if (p_ctx->event_notify_fn) {
    record_status.info.id = p_ctx->segment_info.id;
    p_ctx->event_notify_fn(DVR_RECORD_EVENT_STATUS, &record_status, p_ctx->event_userdata);
    DVR_INFO("%s line %d notify record status, state:%d id=%lld",
          __func__,__LINE__, record_status.state, p_ctx->segment_info.id);
  }
  DVR_INFO("%s, --secure_mode:%d, block_size:%d, cryptor:%p",
        __func__, p_ctx->is_secure_mode,
        block_size, p_ctx->cryptor);
  clock_gettime(CLOCK_MONOTONIC, &start_ts);
  p_ctx->check_pts_count = 0;
  p_ctx->check_no_pts_count++;
  p_ctx->last_send_size = 0;
  p_ctx->last_send_time = 0;
  struct timeval t1, t2, t3, t4, t5, t6, t7;
  while (p_ctx->state == DVR_RECORD_STATE_STARTED ||
    p_ctx->state == DVR_RECORD_STATE_PAUSE) {

    if (p_ctx->state == DVR_RECORD_STATE_PAUSE) {
      //wait resume record
      usleep(20*1000);
      continue;
    }
    gettimeofday(&t1, NULL);

    /* data from dmx, normal dvr case */
    if (p_ctx->is_secure_mode) {
      if (p_ctx->is_new_dmx) {
        /* We resolve the below invoke for dvbcore to be under safety status */
        memset(&new_dmx_secure_buf, 0, sizeof(new_dmx_secure_buf));
        len = record_device_read(p_ctx->dev_handle, &new_dmx_secure_buf,
            sizeof(new_dmx_secure_buf), 10);

        /* Read data from secure demux TA */
        len = record_device_read_ext(p_ctx->dev_handle, &secure_buf.addr,
            &secure_buf.len);
      } else {
          memset(&secure_buf, 0, sizeof(secure_buf));
          len = record_device_read(p_ctx->dev_handle, &secure_buf,
              sizeof(secure_buf), 1000);
      }
    } else {
      len = record_device_read(p_ctx->dev_handle, buf, block_size, 1000);
    }
    if (len == DVR_FAILURE) {
      //usleep(10*1000);
      //DVR_INFO("%s, start_read error", __func__);
      continue;
    }
    gettimeofday(&t2, NULL);

    guarded_size_exceeded = DVR_FALSE;
    if ( p_ctx->guarded_segment_size > 0 &&
        p_ctx->segment_info.size+len >= p_ctx->guarded_segment_size) {
      guarded_size_exceeded = DVR_TRUE;
    }
    /* Got data from device, record it */
    ret = 0;
    if (guarded_size_exceeded) {
      len = 0;
      ret = 0;
      DVR_ERROR("Skip segment_write due to current segment size %u exceeding"
        " guarded segment size", p_ctx->segment_info.size);
    } else if (p_ctx->discard_coming_data) {
      len = 0;
      ret = 0;
      DVR_ERROR("Skip segment_write due to total size exceeding max size too much");
    } else if (p_ctx->enc_func) {
      /* Encrypt record data */
      DVR_CryptoParams_t crypto_params;

      memset(&crypto_params, 0, sizeof(crypto_params));
      crypto_params.type = DVR_CRYPTO_TYPE_ENCRYPT;
      memcpy(crypto_params.location, p_ctx->location, sizeof(p_ctx->location));
      crypto_params.segment_id = p_ctx->segment_info.id;
      crypto_params.offset = p_ctx->segment_info.size;

      if (p_ctx->is_secure_mode) {
        crypto_params.input_buffer.type = DVR_BUFFER_TYPE_SECURE;
        crypto_params.input_buffer.addr = secure_buf.addr;
        crypto_params.input_buffer.size = secure_buf.len;
        crypto_params.output_buffer.size = p_ctx->secbuf_size + 188;
      } else {
        crypto_params.input_buffer.type = DVR_BUFFER_TYPE_NORMAL;
        crypto_params.input_buffer.addr = (size_t)buf;
        crypto_params.input_buffer.size = len;
        crypto_params.output_buffer.size = block_size + 188;
      }

      crypto_params.output_buffer.type = DVR_BUFFER_TYPE_NORMAL;
      crypto_params.output_buffer.addr = (size_t)buf_out;

      p_ctx->enc_func(&crypto_params, p_ctx->enc_userdata);
      gettimeofday(&t3, NULL);
      /* Out buffer length may not equal in buffer length */
      if (crypto_params.output_size > 0) {
        SEG_CALL_RET(write, (p_ctx->segment_handle, buf_out, crypto_params.output_size), ret);
        len = crypto_params.output_size;
      } else {
        len = 0;
      }
    } else if (p_ctx->cryptor) {
      /* Encrypt with clear key */
      int crypt_len = len;
      am_crypt_des_crypt(p_ctx->cryptor, buf_out, buf, &crypt_len, 0);
      len = crypt_len;
      gettimeofday(&t3, NULL);
      SEG_CALL_RET(write, (p_ctx->segment_handle, buf_out, len), ret);
    } else {
      if (first_read == 0) {
        first_read = 1;
        DVR_INFO("%s：%d,first read ts", __func__,__LINE__);
      }
      gettimeofday(&t3, NULL);
      SEG_CALL_RET(write, (p_ctx->segment_handle, buf, len), ret);
    }
    gettimeofday(&t4, NULL);
    //add DVR_RECORD_EVENT_WRITE_ERROR event if write error
    if (ret == -1 && len > 0 && p_ctx->event_notify_fn) {
      //send write event
       if (p_ctx->event_notify_fn) {
         memset(&record_status, 0, sizeof(record_status));
         DVR_INFO("%s:%d,send event write error", __func__,__LINE__);
         record_status.info.id = p_ctx->segment_info.id;
         p_ctx->event_notify_fn(DVR_RECORD_EVENT_WRITE_ERROR, &record_status, p_ctx->event_userdata);
        }
        DVR_INFO("%s,write error %d", __func__,__LINE__);
      goto end;
    }

    if (len > 0 && SEG_CALL_IS_VALID(tell_position)) {
      /* Do time index */
      uint8_t *index_buf = (p_ctx->enc_func || p_ctx->cryptor)? buf_out : buf;
      SEG_CALL_RET(tell_position, (p_ctx->segment_handle), pos);
      has_pcr = record_do_pcr_index(p_ctx, index_buf, len);
      if (has_pcr == 0 && p_ctx->index_type == DVR_INDEX_TYPE_INVALID) {
        clock_gettime(CLOCK_MONOTONIC, &end_ts);
        if ((end_ts.tv_sec*1000 + end_ts.tv_nsec/1000000) -
            (start_ts.tv_sec*1000 + start_ts.tv_nsec/1000000) > 40) {
          /* PCR interval threshold > 40 ms*/
          DVR_INFO("%s use local clock time index", __func__);
          p_ctx->index_type = DVR_INDEX_TYPE_LOCAL_CLOCK;
        }
      } else if (has_pcr && p_ctx->index_type == DVR_INDEX_TYPE_INVALID){
        DVR_INFO("%s use pcr time index", __func__);
        p_ctx->index_type = DVR_INDEX_TYPE_PCR;
        record_do_pcr_index(p_ctx, index_buf, len);
      }
      gettimeofday(&t5, NULL);
      if (p_ctx->index_type == DVR_INDEX_TYPE_PCR) {
        if (has_pcr == 0) {
          if (p_ctx->check_no_pts_count < 2 * CHECK_PTS_MAX_COUNT) {
            if (p_ctx->check_no_pts_count == 0) {
              clock_gettime(CLOCK_MONOTONIC, &start_no_pcr_ts);
              clock_gettime(CLOCK_MONOTONIC, &start_ts);
            }
            p_ctx->check_no_pts_count++;
          }
        } else {
          clock_gettime(CLOCK_MONOTONIC, &start_no_pcr_ts);
          p_ctx->check_no_pts_count = 0;
        }
      }
      /* Update segment i nfo */
      p_ctx->segment_info.size += len;

      /*Duration need use pcr to calculate, todo...*/
      if (p_ctx->index_type == DVR_INDEX_TYPE_PCR) {
        SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), p_ctx->segment_info.duration);
        if (pre_time == 0)
          pre_time = p_ctx->segment_info.duration;
      } else if (p_ctx->index_type == DVR_INDEX_TYPE_LOCAL_CLOCK) {
        clock_gettime(CLOCK_MONOTONIC, &end_ts);
        p_ctx->segment_info.duration = (end_ts.tv_sec*1000 + end_ts.tv_nsec/1000000) -
          (start_ts.tv_sec*1000 + start_ts.tv_nsec/1000000) + pcr_rec_len;
        if (pre_time == 0)
          pre_time = p_ctx->segment_info.duration;
        SEG_CALL(update_pts, (p_ctx->segment_handle, p_ctx->segment_info.duration, pos));
      } else {
        DVR_INFO("%s can NOT do time index", __func__);
      }
      if (p_ctx->index_type == DVR_INDEX_TYPE_PCR &&
          p_ctx->check_pts_count == CHECK_PTS_MAX_COUNT) {
        DVR_INFO("%s change time from pcr to local time", __func__);
        if (pcr_rec_len == 0) {
          SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), pcr_rec_len);
        }
        p_ctx->index_type = DVR_INDEX_TYPE_LOCAL_CLOCK;
        if (pcr_rec_len == 0) {
          SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), pcr_rec_len);
        }
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
      }

      if (p_ctx->index_type == DVR_INDEX_TYPE_PCR ) {
         clock_gettime(CLOCK_MONOTONIC, &end_no_pcr_ts);
         int diff = (int)(end_no_pcr_ts.tv_sec*1000 + end_no_pcr_ts.tv_nsec/1000000) -
          (int)(start_no_pcr_ts.tv_sec*1000 + start_no_pcr_ts.tv_nsec/1000000);
         if (diff > 3000) {
            DVR_INFO("%s no pcr change time from pcr to local time diff[%d]", __func__, diff);
            if (pcr_rec_len == 0) {
              SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), pcr_rec_len);
            }
            p_ctx->index_type = DVR_INDEX_TYPE_LOCAL_CLOCK;
         }
      }
      p_ctx->segment_info.nb_packets = p_ctx->segment_info.size/188;

      if (p_ctx->segment_info.duration - pre_time > DVR_STORE_INFO_TIME) {
        pre_time = p_ctx->segment_info.duration + DVR_STORE_INFO_TIME;
        time_t duration = p_ctx->segment_info.duration;
        if (p_ctx->index_type == DVR_INDEX_TYPE_LOCAL_CLOCK) {
          SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), p_ctx->segment_info.duration);
        }
        SEG_CALL(store_info, (p_ctx->segment_handle, &p_ctx->segment_info));
        p_ctx->segment_info.duration = duration;
      }
    } else {
      gettimeofday(&t5, NULL);
    }
    gettimeofday(&t6, NULL);
     /*Event notification*/
    DVR_Bool_t condA1 = (p_ctx->notification_size > 0);
    DVR_Bool_t condA2 = ((p_ctx->segment_info.size-p_ctx->last_send_size) >= p_ctx->notification_size);
    DVR_Bool_t condA3 = (p_ctx->notification_time > 0);
    DVR_Bool_t condA4 = ((p_ctx->segment_info.duration-p_ctx->last_send_time) >= p_ctx->notification_time);
    DVR_Bool_t condA5 = (guarded_size_exceeded);
    DVR_Bool_t condA6 = (p_ctx->discard_coming_data);
    DVR_Bool_t condB = (p_ctx->event_notify_fn != NULL);
    DVR_Bool_t condC = (p_ctx->segment_info.duration > 0);
    DVR_Bool_t condD = (p_ctx->state == DVR_RECORD_STATE_STARTED);
    if (((condA1 && condA2) || (condA3 && condA4) || condA5 || condA6)
      && condB && condC && condD) {
      memset(&record_status, 0, sizeof(record_status));
      //clock_gettime(CLOCK_MONOTONIC, &end_ts);
      p_ctx->last_send_size = p_ctx->segment_info.size;
      p_ctx->last_send_time = p_ctx->segment_info.duration;
      record_status.state = p_ctx->state;
      record_status.info.id = p_ctx->segment_info.id;
      if (p_ctx->index_type == DVR_INDEX_TYPE_LOCAL_CLOCK) {
        SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), record_status.info.duration);
      } else
        record_status.info.duration = p_ctx->segment_info.duration;
      record_status.info.size = p_ctx->segment_info.size;
      record_status.info.nb_packets = p_ctx->segment_info.size/188;
      p_ctx->event_notify_fn(DVR_RECORD_EVENT_STATUS, &record_status, p_ctx->event_userdata);
      DVR_INFO("%s notify record status, state:%d, id:%lld, duration:%ld ms, size:%zu loc[%s]",
          __func__, record_status.state,
          record_status.info.id, record_status.info.duration,
          record_status.info.size, p_ctx->location);
    }
    gettimeofday(&t7, NULL);
#ifdef DEBUG_PERFORMANCE
    DVR_INFO("record count, read:%dms, encrypt:%dms, write:%dms, index:%dms, store:%dms, notify:%dms total:%dms read len:%zd notify [%d]diff[%d]",
        get_diff_time(t1, t2), get_diff_time(t2, t3), get_diff_time(t3, t4), get_diff_time(t4, t5),
        get_diff_time(t5, t6), get_diff_time(t6, t7), get_diff_time(t1, t5), len,
        p_ctx->notification_time,p_ctx->segment_info.duration -p_ctx->last_send_time);
#endif
    if (len == 0) {
      usleep(20*1000);
    }
  }
end:
  free((void *)buf);
  free((void *)buf_out);
  DVR_INFO("exit %s", __func__);
  return NULL;
}

int dvr_record_open(DVR_RecordHandle_t *p_handle, DVR_RecordOpenParams_t *params)
{
  DVR_RecordContext_t *p_ctx;
  Record_DeviceOpenParams_t dev_open_params;
  int ret = DVR_SUCCESS;
  uint32_t i;

  DVR_RETURN_IF_FALSE(p_handle);
  DVR_RETURN_IF_FALSE(params);

  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (record_ctx[i].state == DVR_RECORD_STATE_CLOSED) {
      break;
    }
  }
  DVR_RETURN_IF_FALSE(i < MAX_DVR_RECORD_SESSION_COUNT);
  DVR_RETURN_IF_FALSE(record_ctx[i].state == DVR_RECORD_STATE_CLOSED);
  p_ctx = &record_ctx[i];
  DVR_INFO("%s , current state:%d, dmx_id:%d, notification_size:%zu, flags:%d, keylen:%d ",
        __func__, p_ctx->state, params->dmx_dev_id,
    params->notification_size,
    params->flags, params->keylen);

  /*Process event params*/
  p_ctx->notification_size = params->notification_size;
  p_ctx->notification_time = params->notification_time;

  p_ctx->event_notify_fn = params->event_fn;
  p_ctx->event_userdata = params->event_userdata;
  p_ctx->last_send_size = 0;
  p_ctx->last_send_time = 0;
  p_ctx->pts = ULLONG_MAX;

  if (params->keylen > 0) {
    p_ctx->cryptor = am_crypt_des_open(params->clearkey,
                params->cleariv,
                params->keylen * 8);
    if (!p_ctx->cryptor)
      DVR_INFO("%s , open des cryptor failed!!!\n", __func__);
  } else {
    p_ctx->cryptor = NULL;
  }

  //check is new driver
  p_ctx->is_new_dmx = dvr_check_dmx_isNew();
  /*Process crypto params, todo*/
  memset((void *)&dev_open_params, 0, sizeof(dev_open_params));
  if (params->data_from_memory) {
    /* data from memory, VOD case */
    p_ctx->is_vod = 1;
  } else {
    p_ctx->is_vod = 0;
    /* data from dmx, normal dvr case */
    dev_open_params.dmx_dev_id = params->dmx_dev_id;
    //set dvr flush size
    dev_open_params.buf_size = (params->flush_size > 0 ? params->flush_size : RECORD_BLOCK_SIZE);
    //set dvbcore ringbuf size
    dev_open_params.ringbuf_size = params->ringbuf_size;

    ret = record_device_open(&p_ctx->dev_handle, &dev_open_params);
    if (ret != DVR_SUCCESS) {
      DVR_INFO("%s, open record devices failed", __func__);
      return DVR_FAILURE;
    }
  }

  p_ctx->block_size = (params->flush_size > 0 ? params->flush_size : RECORD_BLOCK_SIZE);

  p_ctx->enc_func = NULL;
  p_ctx->enc_userdata = NULL;
  p_ctx->is_secure_mode = 0;
  p_ctx->state = DVR_RECORD_STATE_OPENED;
  p_ctx->force_sysclock = params->force_sysclock;
  p_ctx->guarded_segment_size = params->guarded_segment_size;
  if (p_ctx->guarded_segment_size <= 0) {
    DVR_WARN("Odd guarded_segment_size value %lld is given. Change it to"
        " 0 to disable segment guarding mechanism.", p_ctx->guarded_segment_size);
    p_ctx->guarded_segment_size = 0;
  }
  p_ctx->discard_coming_data = DVR_FALSE;
  DVR_INFO("%s, block_size:%d is_new:%d", __func__, p_ctx->block_size, p_ctx->is_new_dmx);

  record_set_segment_ops(p_ctx, params->flags);
  INIT_LIST_HEAD(&p_ctx->segment_ctrls);

  *p_handle = p_ctx;
  return DVR_SUCCESS;
}

int dvr_record_close(DVR_RecordHandle_t handle)
{
  DVR_RecordContext_t *p_ctx;
  int ret = DVR_SUCCESS;
  uint32_t i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  DVR_INFO("%s , current state:%d", __func__, p_ctx->state);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);
  if (p_ctx->cryptor) {
    am_crypt_des_close(p_ctx->cryptor);
    p_ctx->cryptor = NULL;
  }
  if (p_ctx->is_vod) {
    ret = DVR_SUCCESS;
  } else {
    ret = record_device_close(p_ctx->dev_handle);
    if (ret != DVR_SUCCESS) {
      DVR_INFO("%s, failed", __func__);
    }
  }

  if (!list_empty(&p_ctx->segment_ctrls)) {
    DVR_Control_t *pc, *pc_tmp;
    list_for_each_entry_safe(pc, pc_tmp, &p_ctx->segment_ctrls, head) {
      list_del(&pc->head);
      if (pc->data)
        free(pc->data);
      free(pc);
    }
  }

  memset(p_ctx, 0, sizeof(DVR_RecordContext_t));
  p_ctx->state = DVR_RECORD_STATE_CLOSED;
  return ret;
}

int dvr_record_pause(DVR_RecordHandle_t handle)
{
  DVR_RecordContext_t *p_ctx;
  int ret = DVR_SUCCESS;
  uint32_t i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  DVR_INFO("%s , current state:%d", __func__, p_ctx->state);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);

  if (p_ctx->is_vod) {
    ret = DVR_SUCCESS;
  }
  //set pause state,will not store ts into segment
  p_ctx->state = DVR_RECORD_STATE_PAUSE;
  return ret;
}

int dvr_record_resume(DVR_RecordHandle_t handle)
{
  DVR_RecordContext_t *p_ctx;
  int ret = DVR_SUCCESS;
  uint32_t i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  DVR_INFO("%s , current state:%d", __func__, p_ctx->state);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);

  if (p_ctx->is_vod) {
    ret = DVR_SUCCESS;
  }
  //set stated state,will resume store ts into segment
  p_ctx->state = DVR_RECORD_STATE_STARTED;
  return ret;
}

#if 0
int dvr_record_register_encryption(DVR_RecordHandle_t handle,
    DVR_CryptoFunction_t cb,
    DVR_CryptoParams_t params,
    void *userdata)
{
  return DVR_SUCCESS;
}
#endif

int dvr_record_start_segment(DVR_RecordHandle_t handle, DVR_RecordStartParams_t *params)
{
  DVR_RecordContext_t *p_ctx;
  Segment_OpenParams_t open_params;
  int ret = DVR_SUCCESS;
  uint32_t i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  SEG_CALL_INIT(&p_ctx->segment_ops);

  DVR_INFO("%s , current state:%d pids:%d params->location:%s", __func__, p_ctx->state, params->segment.nb_pids, params->location);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_STARTED);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);
  DVR_RETURN_IF_FALSE(params);
  DVR_RETURN_IF_FALSE(strlen((const char *)params->location) < DVR_MAX_LOCATION_SIZE);

  if (SEG_CALL_IS_VALID(open)) {
    memset(&open_params, 0, sizeof(open_params));

    memcpy(open_params.location, params->location, sizeof(params->location));
    open_params.segment_id = params->segment.segment_id;
    open_params.mode = SEGMENT_MODE_WRITE;
    open_params.force_sysclock = p_ctx->force_sysclock;

    SEG_CALL_RET(open, (&open_params, &p_ctx->segment_handle), ret);
    DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

    if (SEG_CALL_IS_VALID(ioctl)) {
      DVR_Control_t *pc;
      list_for_each_entry(pc, &p_ctx->segment_ctrls, head) {
        SEG_CALL_RET(ioctl, (p_ctx->segment_handle, pc->cmd, pc->data, pc->size), ret);
        DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
      }
    }
  }

  /*process params*/
  {
    memcpy(p_ctx->location, params->location, sizeof(params->location));
    //need all params??
    memcpy(&p_ctx->segment_params, &params->segment, sizeof(params->segment));
    /*save current segment info*/
    memset(&p_ctx->segment_info, 0, sizeof(p_ctx->segment_info));
    p_ctx->segment_info.id = params->segment.segment_id;
    p_ctx->segment_info.nb_pids = params->segment.nb_pids;
    memcpy(p_ctx->segment_info.pids, params->segment.pids, params->segment.nb_pids*sizeof(DVR_StreamPid_t));
  }

  if (!p_ctx->is_vod) {
    /* normal dvr case */
    for (i = 0; i < params->segment.nb_pids; i++) {
      ret = record_device_add_pid(p_ctx->dev_handle, params->segment.pids[i].pid);
      DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
    }
    ret = record_device_start(p_ctx->dev_handle);
    DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
  }

  SEG_CALL_RET(store_info, (p_ctx->segment_handle, &p_ctx->segment_info), ret);
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  p_ctx->state = DVR_RECORD_STATE_STARTED;
  if (!p_ctx->is_vod)
    pthread_create(&p_ctx->thread, NULL, record_thread, p_ctx);

  return DVR_SUCCESS;
}

int dvr_record_next_segment(DVR_RecordHandle_t handle, DVR_RecordStartParams_t *params, DVR_RecordSegmentInfo_t *p_info)
{
  DVR_RecordContext_t *p_ctx;
  Segment_OpenParams_t open_params;
  int ret = DVR_SUCCESS;
  uint32_t i;
  loff_t pos;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  DVR_INFO("%s , current state:%d p_ctx->location:%s", __func__, p_ctx->state, p_ctx->location);
  DVR_RETURN_IF_FALSE(p_ctx->state == DVR_RECORD_STATE_STARTED);
  //DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);
  DVR_RETURN_IF_FALSE(params);
  DVR_RETURN_IF_FALSE(p_info);
  DVR_RETURN_IF_FALSE(!p_ctx->is_vod);

  SEG_CALL_INIT(&p_ctx->segment_ops);

  /*Stop the on going record segment*/
  //ret = record_device_stop(p_ctx->dev_handle);
  //DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
  p_ctx->state = DVR_RECORD_STATE_STOPPED;
  pthread_join(p_ctx->thread, NULL);

  //add index file store
  if (SEG_CALL_IS_VALID(update_pts_force)) {
    SEG_CALL_RET_VALID(tell_position, (p_ctx->segment_handle), pos, -1);
    if (pos != -1) {
      SEG_CALL(update_pts_force, (p_ctx->segment_handle, p_ctx->segment_info.duration, pos));
    }
  }

  SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), p_ctx->segment_info.duration);

  /*Update segment info*/
  memcpy(p_info, &p_ctx->segment_info, sizeof(p_ctx->segment_info));

  SEG_CALL_RET(store_info, (p_ctx->segment_handle, p_info), ret);
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  SEG_CALL(store_allInfo, (p_ctx->segment_handle, p_info));

  DVR_INFO("%s dump segment info, id:%lld, nb_pids:%d, duration:%ld ms, size:%zu, nb_packets:%d params->segment.nb_pids:%d",
      __func__, p_info->id, p_info->nb_pids, p_info->duration, p_info->size, p_info->nb_packets, params->segment.nb_pids);

  /*Close current segment*/
  SEG_CALL_RET(close, (p_ctx->segment_handle), ret);
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  p_ctx->last_send_size = 0;
  p_ctx->last_send_time = 0;

  /*Open the new record segment*/
  if (SEG_CALL_IS_VALID(open)) {
    memset(&open_params, 0, sizeof(open_params));
    memcpy(open_params.location, p_ctx->location, sizeof(p_ctx->location));
    open_params.segment_id = params->segment.segment_id;
    open_params.mode = SEGMENT_MODE_WRITE;
    open_params.force_sysclock = p_ctx->force_sysclock;
    DVR_INFO("%s: p_ctx->location:%s  params->location:%s", __func__, p_ctx->location,params->location);
    SEG_CALL_RET(open, (&open_params, &p_ctx->segment_handle), ret);
    DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
  }

  /*process params*/
  {
    //need all params??
    memcpy(&p_ctx->segment_params, &params->segment, sizeof(params->segment));
    /*save current segment info*/
    memset(&p_ctx->segment_info, 0, sizeof(p_ctx->segment_info));
    p_ctx->segment_info.id = params->segment.segment_id;
    memcpy(p_ctx->segment_info.pids, params->segment.pids, params->segment.nb_pids*sizeof(DVR_StreamPid_t));
  }

  p_ctx->segment_info.nb_pids = 0;
  for (i = 0; i < params->segment.nb_pids; i++) {
    switch (params->segment.pid_action[i]) {
      case DVR_RECORD_PID_CREATE:
        DVR_INFO("%s create pid:%d", __func__, params->segment.pids[i].pid);
        ret = record_device_add_pid(p_ctx->dev_handle, params->segment.pids[i].pid);
        p_ctx->segment_info.nb_pids++;
        DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
        break;
      case DVR_RECORD_PID_KEEP:
        DVR_INFO("%s keep pid:%d", __func__, params->segment.pids[i].pid);
        p_ctx->segment_info.nb_pids++;
        break;
      case DVR_RECORD_PID_CLOSE:
        DVR_INFO("%s close pid:%d", __func__, params->segment.pids[i].pid);
        ret = record_device_remove_pid(p_ctx->dev_handle, params->segment.pids[i].pid);
        DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
        break;
      default:
        DVR_INFO("%s wrong action pid:%d", __func__, params->segment.pids[i].pid);
        return DVR_FAILURE;
    }
  }

  //ret = record_device_start(p_ctx->dev_handle);
  //DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  /*Update segment info*/
  SEG_CALL_RET(store_info, (p_ctx->segment_handle, &p_ctx->segment_info), ret);
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  if (p_ctx->pts != ULLONG_MAX) {
    SEG_CALL(update_pts, (p_ctx->segment_handle, p_ctx->pts, 0));
  }

  p_ctx->state = DVR_RECORD_STATE_STARTED;
  pthread_create(&p_ctx->thread, NULL, record_thread, p_ctx);
  return DVR_SUCCESS;
}

int dvr_record_stop_segment(DVR_RecordHandle_t handle, DVR_RecordSegmentInfo_t *p_info)
{
  DVR_RecordContext_t *p_ctx;
  int ret = DVR_SUCCESS;
  uint32_t i;
  loff_t pos = 0;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  if (p_ctx->segment_handle == NULL) {
    // It seems this stop function has been called twice on the same recording,
    // so just return success.
    return DVR_SUCCESS;
  }

  DVR_INFO("%s , current state:%d p_ctx->location:%s", __func__, p_ctx->state, p_ctx->location);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_STOPPED);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);
  DVR_RETURN_IF_FALSE(p_info);/*should support NULL*/

  SEG_CALL_INIT(&p_ctx->segment_ops);

  p_ctx->state = DVR_RECORD_STATE_STOPPED;
  if (p_ctx->is_vod) {
    p_ctx->segment_info.duration = 10*1000; //debug, should delete it
  } else {
    pthread_join(p_ctx->thread, NULL);
    ret = record_device_stop(p_ctx->dev_handle);
    //DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);
    if (ret != DVR_SUCCESS)
      goto end;
    //p_ctx->state = DVR_RECORD_STATE_STOPPED;
  }

  //add index file store
  if (SEG_CALL_IS_VALID(update_pts_force)) {
    SEG_CALL_RET_VALID(tell_position, (p_ctx->segment_handle), pos, -1);
    if (pos != -1) {
      SEG_CALL(update_pts_force, (p_ctx->segment_handle, p_ctx->segment_info.duration, pos));
    }
  }

  SEG_CALL_RET(tell_total_time, (p_ctx->segment_handle), p_ctx->segment_info.duration);

  /*Update segment info*/
  memcpy(p_info, &p_ctx->segment_info, sizeof(p_ctx->segment_info));

  SEG_CALL_RET(store_info, (p_ctx->segment_handle, p_info), ret);
  if (ret != DVR_SUCCESS)
    goto end;

  SEG_CALL(store_allInfo, (p_ctx->segment_handle, p_info));

  DVR_INFO("%s dump segment info, id:%lld, nb_pids:%d, duration:%ld ms, size:%zu, nb_packets:%d",
      __func__, p_info->id, p_info->nb_pids, p_info->duration, p_info->size, p_info->nb_packets);

end:
  SEG_CALL_RET(close, (p_ctx->segment_handle), ret);
  p_ctx->segment_handle = NULL;
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  return DVR_SUCCESS;
}

int dvr_record_resume_segment(DVR_RecordHandle_t handle, DVR_RecordStartParams_t *params, uint64_t *p_resume_size)
{
  DVR_RecordContext_t *p_ctx;
  uint32_t i;
  int ret = DVR_SUCCESS;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);
  DVR_RETURN_IF_FALSE(params);
  DVR_RETURN_IF_FALSE(p_resume_size);

  DVR_INFO("%s , current state:%d, resume size:%lld", __func__, p_ctx->state, *p_resume_size);
  ret = dvr_record_start_segment(handle, params);
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  p_ctx->segment_info.size = *p_resume_size;

  return DVR_SUCCESS;
}

int dvr_record_get_status(DVR_RecordHandle_t handle, DVR_RecordStatus_t *p_status)
{
  DVR_RecordContext_t *p_ctx;
  int i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);
  DVR_RETURN_IF_FALSE(p_status);

  //lock
  p_status->state = p_ctx->state;
  p_status->info.id = p_ctx->segment_info.id;
  p_status->info.duration = p_ctx->segment_info.duration;
  p_status->info.size = p_ctx->segment_info.size;
  p_status->info.nb_packets = p_ctx->segment_info.size/188;

  return DVR_SUCCESS;
}

int dvr_record_write(DVR_RecordHandle_t handle, void *buffer, uint32_t len)
{
  DVR_RecordContext_t *p_ctx;
  uint32_t i;
  int ret = DVR_SUCCESS;
  int has_pcr;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);
  DVR_RETURN_IF_FALSE(buffer);
  DVR_RETURN_IF_FALSE(len);

  SEG_CALL_INIT(&p_ctx->segment_ops);

  has_pcr = record_do_pcr_index(p_ctx, buffer, len);
  if (has_pcr == 0) {
    /* Pull VOD record should use PCR time index */
    DVR_INFO("%s has no pcr, can NOT do time index", __func__);
  }
  SEG_CALL_RET(write, (p_ctx->segment_handle, buffer, len), ret);
  if (ret != len) {
    DVR_INFO("%s write error ret:%d len:%d", __func__, ret, len);
  }
  p_ctx->segment_info.size += len;
  p_ctx->segment_info.nb_packets = p_ctx->segment_info.size/188;

  return DVR_SUCCESS;
}

int dvr_record_set_encrypt_callback(DVR_RecordHandle_t handle, DVR_CryptoFunction_t func, void *userdata)
{
  DVR_RecordContext_t *p_ctx;
  uint32_t i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);
  DVR_RETURN_IF_FALSE(func);

  DVR_INFO("%s , current state:%d", __func__, p_ctx->state);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_STARTED);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);

  p_ctx->enc_func = func;
  p_ctx->enc_userdata = userdata;
  return DVR_SUCCESS;
}

int dvr_record_set_secure_buffer(DVR_RecordHandle_t handle, uint8_t *p_secure_buf, uint32_t len)
{
  DVR_RecordContext_t *p_ctx;
  uint32_t i;
  int ret = DVR_SUCCESS;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);
  DVR_RETURN_IF_FALSE(p_secure_buf);
  DVR_RETURN_IF_FALSE(len);

  DVR_INFO("%s , current state:%d", __func__, p_ctx->state);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_STARTED);
  DVR_RETURN_IF_FALSE(p_ctx->state != DVR_RECORD_STATE_CLOSED);

  ret = record_device_set_secure_buffer(p_ctx->dev_handle, p_secure_buf, len);
  DVR_RETURN_IF_FALSE(ret == DVR_SUCCESS);

  p_ctx->is_secure_mode = 1;
  p_ctx->secbuf_size = len;
  return ret;
}

int dvr_record_is_secure_mode(DVR_RecordHandle_t handle)
{
  DVR_RecordContext_t *p_ctx;
  uint32_t i;
  int ret = DVR_SUCCESS;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }

  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  if (p_ctx->is_secure_mode == 1)
    ret = 1;
  else
    ret = 0;
  return ret;
}

int dvr_record_discard_coming_data(DVR_RecordHandle_t handle, DVR_Bool_t discard)
{
  DVR_RecordContext_t *p_ctx;
  int i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  if (p_ctx->discard_coming_data != discard) {
    p_ctx->discard_coming_data = discard;
    if (discard) {
      DVR_WARN("%s, start discarding coming data. discard:%d",__func__,discard);
    } else {
      DVR_WARN("%s, finish discarding coming data. discard:%d",__func__,discard);
    }
  }

  return DVR_TRUE;
}

int dvr_record_ioctl(DVR_RecordHandle_t handle, unsigned int cmd, void *data, size_t size)
{
  DVR_RecordContext_t *p_ctx;
  int ret = DVR_FAILURE;
  int i;

  p_ctx = (DVR_RecordContext_t *)handle;
  for (i = 0; i < MAX_DVR_RECORD_SESSION_COUNT; i++) {
    if (p_ctx == &record_ctx[i])
      break;
  }
  DVR_RETURN_IF_FALSE(p_ctx == &record_ctx[i]);

  SEG_CALL_INIT(&p_ctx->segment_ops);

  if (SEG_CALL_IS_VALID(ioctl)) {
    if (p_ctx->segment_handle) {
      SEG_CALL_RET(ioctl, (p_ctx->segment_handle, cmd, data, size), ret);
    } else {
      DVR_Control_t *ctrl = (DVR_Control_t *)calloc(1, sizeof(DVR_Control_t));
      if (ctrl) {
        ctrl->cmd = cmd;
        if (size) {
          void *pdata = malloc(size);
          if (pdata) {
            memcpy(pdata, data, size);
            ctrl->data = pdata;
            ctrl->size = size;
          } else {
            free(ctrl);
            ctrl = NULL;
          }
        }
      }
      if (ctrl) {
        list_add_tail(&ctrl->head, &p_ctx->segment_ctrls);
        ret = DVR_SUCCESS;
      }
    }
  }

  return ret;
}
