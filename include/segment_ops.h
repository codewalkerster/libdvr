/*
 * \file
 * Segment module
 */

#ifndef _SEGMENT_OPS_H_
#define _SEGMENT_OPS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "dvr_types.h"

/**\brief Segment handle*/
typedef void* Segment_Handle_t;

/**\brief Segment open mode*/
typedef enum {
  SEGMENT_MODE_READ,            /**< Segment open read mode*/
  SEGMENT_MODE_WRITE,           /**< Segment open write mode*/
  SEGMENT_MODE_MAX              /**< Segment invalid open mode*/
} Segment_OpenMode_t;

/**\brief Segment open parameters*/
typedef struct Segment_OpenParams_s {
  char                  location[DVR_MAX_LOCATION_SIZE];        /**< Segment file location*/
  uint64_t              segment_id;                             /**< Segment index*/
  Segment_OpenMode_t    mode;                                   /**< Segment open mode*/
  DVR_Bool_t            force_sysclock;                         /**< If ture, force to use system clock as PVR index time source. If false, libdvr can determine index time source based on actual situation*/
} Segment_OpenParams_t;

typedef struct Segment_Ops_s {

  /**\brief Open a segment for a target giving some open parameters
   * \param[out] p_handle, Return the handle of the newly created segment
   * \param[in] params, Segment open parameters
   * \return DVR_SUCCESS on success
   * \return error code on failure
   */
  int (*segment_open)(Segment_OpenParams_t *params, Segment_Handle_t *p_handle);

  /**\brief Close a segment
   * \param[in] handle, Segment handle
   * \return DVR_SUCCESS on success
   * \return error code on failure
   */
  int (*segment_close)(Segment_Handle_t handle);

  /**\brief control the giving segment
   * \param[in] handle, Segment handle
   * \param[in] cmd, The control command
   * \param[in] data, The control data
   * \param[in] size, Size of the control data
   * \return DVR_SUCCESS on success
   * \return error code on failure
   */
  int (*segment_ioctl)(Segment_Handle_t handle, int cmd, void *data, size_t size);

  /**\brief Read data from the giving segment
   * \param[out] buf, The buffer of data
   * \param[in] handle, Segment handle
   * \param[in] count, The data count
   * \return The number of bytes read on success
   * \return error code on failure
   */
  ssize_t (*segment_read)(Segment_Handle_t handle, void *buf, size_t count);

  /**\brief Write data from the giving segment
   * \param[in] buf, The buffer of data
   * \param[in] handle, Segment handle
   * \param[in] count, The data count
   * \return The number of bytes write on success
   * \return error code on failure
   */
  ssize_t (*segment_write)(Segment_Handle_t handle, void *buf, size_t count);

  /**\brief force Update the pts and offset when record
   * \param[in] handle, Segment handle
   * \param[in] pts, Current pts
   * \param[in] offset, Current segment offset
   * \return DVR_SUCCESS on success
   * \return error code on failure
   */
  int (*segment_update_pts_force)(Segment_Handle_t handle, uint64_t pts, loff_t offset);


  /**\brief Update the pts and offset when record
   * \param[in] handle, Segment handle
   * \param[in] pts, Current pts
   * \param[in] offset, Current segment offset
   * \return DVR_SUCCESS on success
   * \return error code on failure
   */
  int (*segment_update_pts)(Segment_Handle_t handle, uint64_t pts, loff_t offset);

  /**\brief Seek the segment to the correct position which match the giving time
   * \param[in] handle, Segment handle
   * \param[in] time, The time offset
   * \param[in] block_size, if block_size is > 0, we need aligned to block_size-byte boundary
   * \return The segment current read position on success
   * \return error code on failure
   */
  loff_t (*segment_seek)(Segment_Handle_t handle, uint64_t time, int block_size);

  /**\brief Tell the current position for the giving segment
   * \param[in] handle, Segment handle
   * \return The segment current read position on success
   * \return error code on failure
   */
  loff_t (*segment_tell_position)(Segment_Handle_t handle);

  /**\brief Tell position time of the given segment's postion. Function is used for playback.
   * \param[in] handle, Segment handle
   * \param[in] position, Segment's file position
   * \return position time in ms on success, or -1 on failure
   */
  loff_t (*segment_tell_position_time)(Segment_Handle_t handle, loff_t position);

  /**\brief Tell current playback time of the given segment. Function is used for playback.
   * \param[in] handle, Segment handle
   * \return segment's current playback time in ms on success, or -1 on failure
   */
  loff_t (*segment_tell_current_time)(Segment_Handle_t handle);

  /**\brief Tell total time of the given segment.
   * \param[in] handle, Segment handle
   * \return The segment's total time in ms on success, or -1 on failure
   */
  loff_t (*segment_tell_total_time)(Segment_Handle_t handle);

  /**\brief Store the segment information to a file
   * \param[in] handle, The segment handle
   * \param[in] p_info, The segment information pointer
   * \return DVR_SUCCESS On success
   * \return Error code On failure
   */
  int (*segment_store_info)(Segment_Handle_t handle, Segment_StoreInfo_t *p_info);

  /**\brief Store the segment all information to a file
   * \param[in] handle, The segment handle
   * \param[in] p_info, The segment information pointer
   * \return DVR_SUCCESS On success
   * \return Error code On failure
   */
  int (*segment_store_allInfo)(Segment_Handle_t handle, Segment_StoreInfo_t *p_info);

  /**\brief Load the segment information from a file
   * \param[in] handle, The segment handle
   * \param[out] p_info, The segment information pointer
   * \return DVR_SUCCESS On success
   * \return Error code On failure
   */
  int (*segment_load_info)(Segment_Handle_t handle, Segment_StoreInfo_t *p_info);

  /**\brief Load the segment information from a file
   * \param[in] handle, The segment handle
   * \param[out] p_info, The segment information pointer
   * \return DVR_SUCCESS On success
   * \return Error code On failure
   */
  int (*segment_load_allInfo)(Segment_Handle_t handle, struct list_head *list);


  /**\brief Delete the segment information file
   * \param[in] location, The record file's location
   * \param[in] segment_id, The segment's index
   * \return DVR_SUCCESS On success
   * \return Error code On failure
   */
  int (*segment_delete)(const char *location, uint64_t segment_id);

  /**\brief check the segment is ongoing file
   * \param[in] handle, The segment handle
   * \return DVR_SUCCESS On success
   * \return Error code not ongoing
   */
  int (*segment_ongoing)(Segment_Handle_t handle);

  /**\brief get current ongoing segment size
   * \param[in] handle, The segment handle
   * \return segment size
   */
  off_t (*segment_get_cur_segment_size)(Segment_Handle_t handle);

  /**\brief get current ongoing segment id
   * \param[in] handle, The segment handle
   * \return segment id
   */
  uint64_t (*segment_get_cur_segment_id)(Segment_Handle_t handle);
} Segment_Ops_t;

#ifdef __cplusplus
}
#endif

#endif /*END _SEGMENT_H_H_*/
