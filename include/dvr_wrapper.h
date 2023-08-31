/**
 * \mainpage Amlogic DVR library
 *
 * \section Introduction
 * "libdvr" is a library provides basic DVR functions used by Amlogic platform.
 * It supports:
 * \li Record
 * \li Playback
 * \li Index file generated
 * \li Segment split
 * \li Encrypt and decrypt
 *
 * \file
 * \brief libdvr wrapper layer
 *
 * Wrapper layer is upper layer of libdvr.
 * It is on top of dvr_record and dvr_playback.
 * It supports:
 * \li Separate record segments automatically.
 * \li Load segments automatically.
 */

#ifndef DVR_WRAPPER_H_
#define DVR_WRAPPER_H_

#include "dvr_types.h"
#include "dvr_crypto.h"
#include "dvr_playback.h"
#include "dvr_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/**Record wrapper handle.*/
typedef void* DVR_WrapperRecord_t;
/**Playback wrapper handle.*/
typedef void* DVR_WrapperPlayback_t;

typedef void* Playback_DeviceHandle_t;

typedef struct {
  time_t              time;       /**< time duration, unit on ms*/
  loff_t              size;       /**< size*/
  uint32_t            pkts;       /**< number of ts packets*/
} DVR_WrapperInfo_t;

typedef struct {
  uint32_t              nb_pids;                         /**< Number of PIDs.*/
  DVR_StreamPid_t       pids[DVR_MAX_RECORD_PIDS_COUNT]; /**< PIDs to be recorded.*/
} DVR_WrapperPidsInfo_t;

typedef struct {
  DVR_PlaybackPlayState_t state;      /**< DVR playback state*/
  DVR_WrapperInfo_t info_cur;         /**< DVR playback current information*/
  DVR_WrapperInfo_t info_full;        /**< DVR playback total(non-obsolete) information*/
  DVR_PlaybackPids_t pids;            /**< DVR playback pids information*/
  float speed;                        /**< DVR playback current speed*/
  DVR_PlaybackSegmentFlag_t flags;    /**< DVR playback flags*/
  DVR_WrapperInfo_t info_obsolete;    /**< DVR playback obsolete information, take into account for timeshift*/
  DVR_WrapperInfo_t disguised_info_obsolete;    /**< DVR playback disguised obsolete information, take into account for timeshift*/
} DVR_WrapperPlaybackStatus_t;

typedef struct {
  DVR_RecordState_t state;            /**< DVR record state*/
  DVR_WrapperInfo_t info;             /**< DVR record information*/
  DVR_WrapperPidsInfo_t pids;         /**< DVR record pids info*/
  DVR_WrapperInfo_t info_obsolete;    /**< DVR record obsolete information, take into account for timeshift*/
} DVR_WrapperRecordStatus_t;

/**Record wrapper open parameters.*/
typedef struct {
  int                   fend_dev_id;                     /**< Frontend device's index.*/
  int                   dmx_dev_id;                      /**< Demux device's index.*/
  char                  location[DVR_MAX_LOCATION_SIZE]; /**< Location of the record file.*/
  DVR_Bool_t            is_timeshift;                    /**< The record file is used by timeshift.*/
  loff_t                segment_size;                    /**< Segment file's size.*/
  loff_t                max_size;                        /**< Maximum record file size in bytes(for timeshift).*/
  time_t                max_time;                        /**< Maximum record time in milliseconds(for timeshift).*/
  DVR_RecordFlag_t      flags;                           /**< Flags.*/
  DVR_CryptoPeriod_t    crypto_period;                   /**< Crypto period.*/
  DVR_CryptoFunction_t  crypto_fn;                       /**< Crypto callback function.*/
  void                  *crypto_data;                    /**< User data of crypto function.*/
  uint8_t               *clearkey;                       /**< key for encrypted PVR on FTA.*/
  uint8_t               *cleariv;                        /**< iv for encrypted PVR on FTA.*/
  uint32_t              keylen;                          /**< key/iv length.*/
  DVR_RecordEventFunction_t   event_fn;                  /**< DVR record event callback function.*/
  void                        *event_userdata;           /**< DVR event userdata.*/
  int                   flush_size;                      /**< DVR flush size.*/
  int                   ringbuf_size;                    /**< DVR ringbuf size.*/
  DVR_Bool_t            force_sysclock;                  /**< If ture, force to use system clock as PVR index time source. If false, libdvr can determine index time source based on actual situation*/
} DVR_WrapperRecordOpenParams_t;

typedef struct {
  DVR_WrapperPidsInfo_t pids_info;
  int                   save_rec_file;                    /**< save rec file with same location,default is 0.*/
} DVR_WrapperRecordStartParams_t;

typedef struct {
  uint32_t              nb_pids;                               /**< Number of PID actions.*/
  DVR_StreamPid_t       pids[DVR_MAX_RECORD_PIDS_COUNT];       /**< PIDs.*/
  DVR_RecordPidAction_t pid_action[DVR_MAX_RECORD_PIDS_COUNT]; /**< Actions.*/
} DVR_WrapperUpdatePidsParams_t;

/**Playback wrapper open parameters.*/
typedef struct {
  int                     dmx_dev_id;                      /**< playback used dmx device index*/
  char                    location[DVR_MAX_LOCATION_SIZE]; /**< Location of the record file.*/
  int                     block_size;                      /**< playback inject block size*/
  DVR_Bool_t              is_timeshift;                    /**< 0:playback mode, 1 : is timeshift mode*/
  Playback_DeviceHandle_t playback_handle;                 /**< Playback device handle.*/
  DVR_CryptoFunction_t    crypto_fn;                       /**< Crypto function.*/
  void                    *crypto_data;                    /**< Crypto function's user data.*/
  uint8_t                 *clearkey;                       /**< key for encrypted PVR on FTA.*/
  uint8_t                 *cleariv;                        /**< iv for encrypted PVR on FTA.*/
  uint32_t                keylen;                          /**< key/iv length.*/
  DVR_PlaybackEventFunction_t  event_fn;                   /**< playback event callback function*/
  void                        *event_userdata;             /**< event userdata*/
  DVR_Bool_t              is_notify_time;                  /**< 0:not notify time, 1 : notify*/
  DVR_PlaybackVendor_t    vendor;                          /**< vendor type*/
  DVR_Bool_t              control_speed_enable;            /**< 1: system clock, 0: libdvr can determine index time source based on actual situation*/
} DVR_WrapperPlaybackOpenParams_t;

/**
 * Open a new record wrapper.
 * \param[out] rec Return the new record handle.
 * \param params Record open parameters.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_open_record (DVR_WrapperRecord_t *rec, DVR_WrapperRecordOpenParams_t *params);

/**
 * Close an unused record wrapper.
 * \param rec The record handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_close_record (DVR_WrapperRecord_t rec);

/**
 * Start recording.
 * \param rec The record handle.
 * \param params Record start parameters.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_start_record (DVR_WrapperRecord_t rec, DVR_WrapperRecordStartParams_t *params);

/**
 * Stop recording..
 * \param rec The record handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_stop_record (DVR_WrapperRecord_t rec);

/**
 * Pause recording..
 * \param rec The record handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_pause_record (DVR_WrapperRecord_t rec);

/**
 * Resume recording..
 * \param rec The record handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_resume_record (DVR_WrapperRecord_t rec);

/**
 * Update the recording PIDs.
 * \param rec The record handle.
 * \param params The new PIDs.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_update_record_pids (DVR_WrapperRecord_t rec, DVR_WrapperUpdatePidsParams_t *params);

/**
 * Get the current recording status.
 * \param rec The record handle.
 * \param status The recording status returned.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_get_record_status (DVR_WrapperRecord_t rec, DVR_WrapperRecordStatus_t *status);

/**
 * check record mode is secure or free.
 * \param rec The record handle.
 * \retval 1:secure 0: free mode.
 * \return secure or free.
 */
int dvr_wrapper_record_is_secure_mode(DVR_WrapperRecord_t rec);

/**
 * Set record secure buffer.
 * \param rec The record handle.
 * \param p_secure_buf record p_secure_buf addr.
 * \param len record p_secure_buf len.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_record_secure_buffer (DVR_WrapperRecord_t rec,  uint8_t *p_secure_buf, uint32_t len);

/**
 * Set record decrypt callback.
 * \param rec The record handle.
 * \param func record dec cb.
 * \param userdata cb user data.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_record_decrypt_callback (DVR_WrapperRecord_t rec,  DVR_CryptoFunction_t func, void *userdata);

/**
 * Open a new playback wrapper handle.
 * \param[out] playback Return the new playback handle.
 * \param params Playback handle open parameters.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_open_playback (DVR_WrapperPlayback_t *playback, DVR_WrapperPlaybackOpenParams_t *params);

/**
 * Close a unused playback handle.
 * \param playback The playback handle to be closed.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_close_playback (DVR_WrapperPlayback_t playback);

/**
 * Start playback.
 * \param playback The playback handle.
 * \param flags Playback flags.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_start_playback (DVR_WrapperPlayback_t playback, DVR_PlaybackFlag_t flags, DVR_PlaybackPids_t *p_pids);

/**
 * Stop playback.
 * \param playback The playback handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_stop_playback (DVR_WrapperPlayback_t playback);

/**
 * Pause the playback.
 * \param playback The playback handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_pause_playback (DVR_WrapperPlayback_t playback);

/**
 * resume the playback.
 * \param playback The playback handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_resume_playback (DVR_WrapperPlayback_t playback);

/**
 * Set the playback speed.
 * \param playback The playback handle.
 * \param speed The new speed.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_playback_speed (DVR_WrapperPlayback_t playback, float speed);

/**
 * set the current playback limit info.
 * \param playback The playback handle.
 * \param time The rec time in milliseconds.
 * \param limit The rec limit time in milliseconds.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_setlimit_playback (DVR_WrapperPlayback_t playback, uint64_t time, int32_t limit);

/**
 * Seek the current playback position.
 * \param playback The playback handle.
 * \param time_offset The current time in milliseconds.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_seek_playback (DVR_WrapperPlayback_t playback, uint32_t time_offset);

/**
 * Get the current playback status.
 * \param playback The playback handle.
 * \param status The playback status returned.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_get_playback_status (DVR_WrapperPlayback_t playback, DVR_WrapperPlaybackStatus_t *status);

/**
 * Update playback.
 * \param playback The playback handle.
 * \param flags Playback flags.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_update_playback (DVR_WrapperPlayback_t playback, DVR_PlaybackPids_t *p_pids);

/**
 * only Update playback info.
 * \param playback The playback handle.
 * \param flags Playback flags.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_only_update_playback (DVR_WrapperPlayback_t playback, DVR_PlaybackPids_t *p_pids);

/**
 * Set playback secure buffer.
 * \param playback The playback handle.
 * \param p_secure_buf Playback p_secure_buf addr.
 * \param len Playback p_secure_buf len.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_secure_buffer (DVR_WrapperPlayback_t playback,  uint8_t *p_secure_buf, uint32_t len);

/**
 * Set playback decrypt callback.
 * \param playback The playback handle.
 * \param func Playback dec cb.
 * \param userdata cb user data.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_decrypt_callback (DVR_WrapperPlayback_t playback,  DVR_CryptoFunction_t func, void *userdata);

/**
 * Delete all info of segment whose location is "*location"
 * \param[in] location The record of need del file's location
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_segment_del_by_location (const char *location);

/**
 * Get the info of segment whose location is "*location"
 * \param[in] location The record file's location
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_segment_get_info_by_location (const char *location, DVR_WrapperInfo_t *p_info);

/**
 * Stop timeshift.
 * \param playback The playback handle.
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_stop_timeshift (DVR_WrapperPlayback_t playback);
/**
 * reStart timeshift.
 * \param playback The playback handle.
 * \param flags Playback flags.
 * \param p_pids playback pids info
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_restart_timeshift(DVR_WrapperPlayback_t playback, DVR_PlaybackFlag_t flags, DVR_PlaybackPids_t *p_pids);

/**
 * Set log level
 * \param level Log level, 0: UNKNOWN, 1: DEFAULT, 2:VERBOSE, 3:DEBUG.
 *                         4: INFO, 5: WARN, 6: ERROR, 7: FATAL
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_log_level (int level);

/**
 * Set AC4 audio preselection id for playback
 * \param playback The playback handle.
 * \param presel_id AC4 audio preselection id
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_set_ac4_preselection_id(DVR_WrapperPlayback_t playback, int presel_id);

/**
 * Set various libdvr properties
 * \param[in] prop_name property name
 * \param[in] prop_value property value
 * \retval DVR_SUCCESS On success.
 * \return DVR_FAILURE On failure.
 */
int dvr_wrapper_property_set(const char* prop_name, const char* prop_value);

/**
 * Get various libdvr properties
 * \param[in] prop_name property name
 * \param[out] prop_value property value. Buffer lifecycle is managed by caller.
 * \param[in] length buffer length of prop_value
 * \retval DVR_SUCCESS On success.
 * \return DVR_FAILURE On failure.
 */
int dvr_wrapper_property_get(const char* prop_name, char* prop_value, int length);

/**
 * Control the internal recording logic
 * \param rec The record handle.
 * \param cmd control command
 * \param data control data
 * \param size size of the control data
 * \retval DVR_SUCCESS On success.
 * \return Error code.
 */
int dvr_wrapper_ioctl_record(DVR_WrapperRecord_t rec, unsigned int cmd, void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /*DVR_WRAPPER_H_*/

