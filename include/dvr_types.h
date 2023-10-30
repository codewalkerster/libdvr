/***************************************************************************
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
/**\file
 * \brief Basic datatypes
 * \author Gong Ke <ke.gong@amlogic.com>
 * \date 2010-05-19: create the document
 ***************************************************************************/

#ifndef _DVR_TYPES_H
#define _DVR_TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <pthread.h>

#include "list.h"
#include <android/log.h>

#ifndef __ANDROID_API__
#include <limits.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*****************************************************************************
* Global Definitions
*****************************************************************************/

/**Maximum PIDs can be recorded.*/
#define DVR_MAX_RECORD_PIDS_COUNT 32
/**Maximum path size.*/
#define DVR_MAX_LOCATION_SIZE     512

/**Log facilities.*/
#define LOG_LV_DEFAULT 1
#define LOG_LV_VERBOSE 2
#define LOG_LV_DEBUG 3
#define LOG_LV_INFO 4
#define LOG_LV_WARN 5
#define LOG_LV_ERROR 6
#define LOG_LV_FATAL 7

#define DVR_LOG_TAG "libdvr"
#define DVR_DEBUG(...) DVR_LOG_PRINT(LOG_LV_DEBUG, DVR_LOG_TAG, __VA_ARGS__)
#define DVR_INFO(...) DVR_LOG_PRINT(LOG_LV_INFO, DVR_LOG_TAG, __VA_ARGS__)
#define DVR_WARN(...) DVR_LOG_PRINT(LOG_LV_WARN, DVR_LOG_TAG, __VA_ARGS__)
#define DVR_ERROR(...) DVR_LOG_PRINT(LOG_LV_ERROR, DVR_LOG_TAG, __VA_ARGS__)
#define DVR_FATAL(...) DVR_LOG_PRINT(LOG_LV_FATAL, DVR_LOG_TAG, __VA_ARGS__)

extern int g_dvr_log_level;

#define DVR_LOG_PRINT(level,tag,...) \
  do { \
    if (level>=g_dvr_log_level) { \
      __android_log_print(level,tag,__VA_ARGS__); \
    } \
  }while(0)

/**Abort the program if assertion is false.*/
#define DVR_ASSERT(expr) \
  do {\
    if (!(expr)) {\
      DVR_FATAL("at %s:%d, \'"#expr"\' is false then asserts", __func__, __LINE__);\
      assert(expr);\
    }\
  } while (0)

/**Return DVR_FAILURE is the expression is false.*/
#define DVR_RETURN_IF_FALSE(expr)\
  do {\
    if (!(expr)) {\
      DVR_ERROR("at %s:%d, \'"#expr"\' is false then returns failure", __func__, __LINE__);\
      return DVR_FAILURE;\
    }\
  } while (0);

/**Return DVR_FAILURE and unlock the mutex when the expression is false.*/
#define DVR_RETURN_IF_FALSE_WITH_UNLOCK(expr, lock)\
  do {\
    if (!(expr)) {\
      DVR_ERROR("at %s:%d, \'"#expr"\' is false then unlocks and returns failure", __func__, __LINE__);\
      pthread_mutex_unlock(lock);\
      return DVR_FAILURE;\
    }\
  } while (0);

/**\brief Boolean value*/
typedef uint8_t        DVR_Bool_t;

#ifndef DVR_TRUE
/**\brief Boolean value: true*/
#define DVR_TRUE        (1)
#endif

#ifndef DVR_FALSE
/**\brief Boolean value: false*/
#define DVR_FALSE       (0)
#endif

#ifndef DVR_UNUSED
#define DVR_UNUSED(x) (void)(x)
#endif

/**Function result*/
typedef enum {
  DVR_FAILURE = -1, /**< Generic error.*/
  DVR_SUCCESS = 0   /**< Success*/
} DVR_Result_t;

/**PID*/
typedef uint16_t DVR_Pid_t;

/**Invalid PID value.*/
#define DVR_INVALID_PID 0xffff

/**PTS.*/
typedef uint64_t DVR_Pts_t;

/**Stream type.*/
typedef enum {
  DVR_STREAM_TYPE_VIDEO,     /**< Video stream.*/
  DVR_STREAM_TYPE_AUDIO,     /**< Audio stream.*/
  DVR_STREAM_TYPE_AD,        /**< AD stream.*/
  DVR_STREAM_TYPE_SUBTITLE,  /**< Subtitle stream.*/
  DVR_STREAM_TYPE_TELETEXT,  /**< Teletext stream.*/
  DVR_STREAM_TYPE_ECM,       /**< ECM stream.*/
  DVR_STREAM_TYPE_EMM,       /**< EMM stream.*/
  DVR_STREAM_TYPE_OTHER,     /**< other stream.*/
} DVR_StreamType_t;

/**Video format.*/
typedef enum {
  DVR_VIDEO_FORMAT_MPEG1, /**< MPEG1 video.*/
  DVR_VIDEO_FORMAT_MPEG2, /**< MPEG2 video.*/
  DVR_VIDEO_FORMAT_H264,  /**< H264.*/
  DVR_VIDEO_FORMAT_HEVC ,  /**< HEVC.*/
  DVR_VIDEO_FORMAT_VP9  /**< VP9.*/
} DVR_VideoFormat_t;

/**Audio format.*/
typedef enum {
  DVR_AUDIO_FORMAT_MPEG, /**< MPEG audio*/
  DVR_AUDIO_FORMAT_AC3,  /**< AC3 audio.*/
  DVR_AUDIO_FORMAT_EAC3, /**< EAC3 audio.*/
  DVR_AUDIO_FORMAT_DTS  , /**< DTS audio.*/
  DVR_AUDIO_FORMAT_AAC, /**< AAC audio.*/
  DVR_AUDIO_FORMAT_HEAAC, /**<HE AAC audio.*/
  DVR_AUDIO_FORMAT_LATM, /**<LATM audio.*/
  DVR_AUDIO_FORMAT_PCM, /**<PCM audio.*/
  DVR_AUDIO_FORMAT_AC4 /**<PCM audio.*/
} DVR_AudioFormat_t;

/**Buffer type.*/
typedef enum {
  DVR_BUFFER_TYPE_NORMAL, /**< Normal buffer.*/
  DVR_BUFFER_TYPE_SECURE  /**< Secure buffer.*/
} DVR_BufferType_t;

/**Stream's PID.*/
typedef struct {
  DVR_StreamType_t type;   /**< Stream type.*/
  DVR_Pid_t        pid;    /**< PID.*/
} DVR_StreamPid_t;

/**Stream information.*/
typedef struct {
  DVR_StreamType_t type;   /**< Stream type.*/
  DVR_Pid_t        pid;    /**< PID.*/
  int              format; /**< Codec format.*/
} DVR_StreamInfo_t;

/**Stream buffer.*/
typedef struct {
  DVR_BufferType_t type; /**< Buffer type.*/
  size_t           addr; /**< Start address of the buffer.*/
  size_t           size; /**< Size of the buffer.*/
} DVR_Buffer_t;

/**DVR error reason.*/
typedef enum
{
  DVR_ERROR_REASON_GENERIC,         /**< GENERIC error.*/
  DVR_ERROR_REASON_READ,            /**< Disk read error.*/
  DVR_ERROR_REASON_WRITE,           /**< Disk write error.*/
  DVR_ERROR_REASON_DISK_FULL,       /**< Disk is    full.*/
} DVR_Error_Reason_t;


/**\brief Segment store information*/
typedef struct {
  struct  list_head   head;         /**< Segment node.*/
  uint64_t            id;                                         /**< DVR segment id*/
  uint32_t            nb_pids;                                    /**< DVR segment number of pids*/
  DVR_StreamPid_t     pids[DVR_MAX_RECORD_PIDS_COUNT];            /**< DVR pids information*/
  time_t              duration;                                   /**< DVR segment time duration, unit on ms*/
  size_t              size;                                       /**< DVR segment size*/
  uint32_t            nb_packets;                                 /**< DVR segment number of ts packets*/
} Segment_StoreInfo_t;

/**\brief DVR record segment information*/
typedef Segment_StoreInfo_t DVR_RecordSegmentInfo_t;


#ifdef __cplusplus
}
#endif

#endif

