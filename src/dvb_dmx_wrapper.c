/***************************************************************************
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 *
 * @brief   linux dvb demux wrapper
 * @file    dvb_dmx_wrapper.c
 *
 * \author Chuanzhi Wang <chuanzhi.wang@amlogic.com>
 * \date 2020-07-16: create the document
 ***************************************************************************/

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>

#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dmx.h"
#include "dvb_dmx_wrapper.h"

#define DMX_COUNT (3)
#define DMX_FILTER_COUNT (32*DMX_COUNT)
#define SEC_BUF_SIZE (4096)
#define DMX_POLL_TIMEOUT (200)


typedef struct
{
    int dev_no;
    int fd;
    int used;
    int enable;
    int need_free;
    AML_DMX_DataCb cb;
    void *user_data;
}dvb_dmx_filter_t;

typedef struct
{
    int dev_no;
    int running;
    pthread_t thread;
    pthread_mutex_t lock;

    dvb_dmx_filter_t filter[DMX_FILTER_COUNT];
}dvb_dmx_t;

static dvb_dmx_t dmx_devices[DMX_COUNT];

static inline DVB_RESULT dmx_get_dev(int dev_no, dvb_dmx_t **dev)
{
    if ((dev_no < 0) || (dev_no >= DMX_COUNT))
    {
        DVB_INFO("invalid demux device number %d, must in(%d~%d)", dev_no, 0, DMX_COUNT-1);
        return DVB_FAILURE;
    }

    *dev = &dmx_devices[dev_no];
    return DVB_SUCCESS;
}

static void* dmx_data_thread(void *arg)
{
    int i, fid;
    int ret;
    int cnt, len;
    uint32_t mask;
    uint8_t *sec_buf = NULL;
    int fids[DMX_FILTER_COUNT];
    struct pollfd fds[DMX_FILTER_COUNT];
    dvb_dmx_filter_t *filter = NULL;
    dvb_dmx_t *dmx = (dvb_dmx_t *)arg;

    sec_buf = (uint8_t *)malloc(SEC_BUF_SIZE);
    prctl(PR_SET_NAME, "dmx_data_thread");
    while (dmx->running)
    {
        cnt = 0;
        mask = 0;

        pthread_mutex_lock(&dmx->lock);
        for (fid = 0; fid < DMX_FILTER_COUNT; fid++)
        {
           if (dmx->filter[fid].need_free)
           {
               filter = &dmx->filter[fid];
               close(filter->fd);
               filter->used = 0;
               filter->need_free = 0;
               filter->cb = NULL;
           }

           if (dmx->filter[fid].used)
           {
               fds[cnt].events = POLLIN | POLLERR;
               // It is not necessary to protect code block below with
               // Record_DeviceContext_t.lock, so the following annotation
               // is given to suppress related Coverity complaint.
               // coverity[missing_lock]
               fds[cnt].fd = dmx->filter[fid].fd;
               fids[cnt] = fid;
               cnt++;
           }
        }

        pthread_mutex_unlock(&dmx->lock);

        if (!cnt)
        {
            usleep(20*1000);
            continue;
        }

        ret = poll(fds, cnt, DMX_POLL_TIMEOUT);
        if (ret <= 0)
        {
            continue;
        }

        for (i = 0; i < cnt; i++)
        {
            if (fds[i].revents & (POLLIN | POLLERR))
            {
                pthread_mutex_lock(&dmx->lock);
                filter = &dmx->filter[fids[i]];
                if (!filter->enable || !filter->used || filter->need_free)
                {
                    DVB_INFO("ch[%d] not used, not read", fids[i]);
                    len = 0;
                }
                else
                {
                     len = read(filter->fd, sec_buf, SEC_BUF_SIZE);
                     if (len <= 0)
                     {
                         DVB_INFO("read demux filter[%d] failed (%s) %d", fids[i], strerror(errno), errno);
                     }
                }
                pthread_mutex_unlock(&dmx->lock);
#ifdef DEBUG_DEMUX_DATA
                if (len)
                    DVB_INFO("tid[%x] ch[%d] %x bytes", sec_buf[0], fids[i], len);
#endif
                if (len > 0 && filter->cb)
                {
                    filter->cb(filter->dev_no, fids[i], sec_buf, len, filter->user_data);
                }
            }
        }
    }

    if (sec_buf)
    {
        free(sec_buf);
    }

    return NULL;
}

static dvb_dmx_filter_t* dmx_get_filter(dvb_dmx_t * dev, int fhandle)
{
    if (fhandle >= DMX_FILTER_COUNT)
    {
        DVB_INFO("wrong filter no");
        return NULL;
    }

    if (!dev->filter[fhandle].used)
    {
        DVB_INFO("filter %d not allocated", fhandle);
        return NULL;
    }
    return &dev->filter[fhandle];
}

/**\brief dmx device init, creat dmx thread
 * \param dmx device number
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_Open(int dev_no)
{
    dvb_dmx_t *dev = NULL;

    if (dmx_get_dev(dev_no, &dev))
        return DVB_FAILURE;

    if (dev->running)
    {
        DVB_INFO("dmx already initialized");
        return DVB_FAILURE;
    }

    dev->dev_no = dev_no;

    pthread_mutex_init(&dev->lock, NULL);
    dev->running = 1;
    pthread_create(&dev->thread, NULL, dmx_data_thread, dev);

    return DVB_SUCCESS;
}

/**\brief allocate dmx filter
 * \param dmx device number
 * \param get dmx filter index
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_AllocateFilter(int dev_no, int *fhandle)
{
    int fd;
    int fid;
    dvb_dmx_filter_t *filter = NULL;
    char dev_name[32];

    dvb_dmx_t *dev = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("demux allocate failed, wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);
    filter = &dev->filter[0];
    for (fid = 0; fid < DMX_FILTER_COUNT; fid++)
    {
        if (!filter[fid].used)
        {
            break;
        }
    }

    if (fid >= DMX_FILTER_COUNT)
    {
        DVB_INFO("filter id:%d, have no filter to alloc", fid);
        pthread_mutex_unlock(&dev->lock);
        return DVB_FAILURE;
    }

    memset(dev_name, 0, sizeof(dev_name));
    sprintf(dev_name, "/dev/dvb0.demux%d", dev_no);
    fd = open(dev_name, O_RDWR);
    if (fd == -1)
    {
        DVB_INFO("cannot open \"%s\" (%s)", dev_name, strerror(errno));
        pthread_mutex_unlock(&dev->lock);
        return DVB_FAILURE;
    }

    memset(&filter[fid], 0, sizeof(dvb_dmx_filter_t));
    filter[fid].dev_no = dev_no;
    filter[fid].fd = fd;
    filter[fid].used = 1;
    *fhandle = fid;

    pthread_mutex_unlock(&dev->lock);

    //DVB_INFO("fhandle = %d", fid);
    return DVB_SUCCESS;
}

/**\brief set demux section filter
 * \param dmx device number
 * \param dmx filter index
 * \param dmx section filter param
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_SetSecFilter(int dev_no, int fhandle, const struct dmx_sct_filter_params *params)
{
    DVB_RESULT ret = DVB_SUCCESS;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("Wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    if (!params)
        return DVB_FAILURE;

    pthread_mutex_lock(&dev->lock);

    filter = dmx_get_filter(dev, fhandle);
    if (filter)
    {
       if (ioctl(filter->fd, DMX_STOP, 0) < 0)
       {
           DVB_INFO("dmx stop filter failed error:%s", strerror(errno));
           ret = DVB_FAILURE;
       }
       else if (ioctl(filter->fd, DMX_SET_FILTER, params) < 0)
       {
             DVB_INFO("set filter failed error:%s", strerror(errno));
          ret = DVB_FAILURE;
       }
    }

    pthread_mutex_unlock(&dev->lock);
    //DVB_INFO("pid = %x", params->pid);
    return ret;
}

/**\brief set demux pes filter
 * \param dmx device number
 * \param dmx filter index
 * \param dmx pes filter param
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_SetPesFilter(int dev_no, int fhandle, const struct dmx_pes_filter_params *params)
{
    DVB_RESULT ret = DVB_SUCCESS;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_ERROR("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    if (!params)
        return DVB_FAILURE;

    pthread_mutex_lock(&dev->lock);

    filter = dmx_get_filter(dev, fhandle);
    if (filter)
    {
        ret = DVB_FAILURE;
        if (ioctl(filter->fd, DMX_STOP, 0) < 0)
        {
            DVB_ERROR("stopping demux filter fails with errno:%d(%s)",errno,strerror(errno));
        }
        else if (fcntl(filter->fd, F_SETFL, O_NONBLOCK) < 0)
        {
            DVB_ERROR("setting filter non-block flag fails with errno:%d(%s)",errno,strerror(errno));
        }
        else if (ioctl(filter->fd, DMX_SET_PES_FILTER, params) < 0)
        {
            DVB_ERROR("setting PES filter fails with errno:%d(%s)",errno,strerror(errno));
        }
        else
        {
            ret = DVB_SUCCESS;
        }
    }

    pthread_mutex_unlock(&dev->lock);
    //DVB_INFO("pid = %x", params->pid);
    return ret;
}

/**\brief set demux filter buffer
 * \param dmx device number
 * \param dmx filter index
 * \param dmx filter buffer size
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_SetBufferSize(int dev_no, int fhandle, int size)
{
    DVB_RESULT ret = DVB_SUCCESS;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);

    filter = dmx_get_filter(dev, fhandle);
    if (filter)
    {
        if (ioctl(filter->fd, DMX_SET_BUFFER_SIZE, size) < 0)
        {
              DVB_INFO("set buf size failed error:%s", strerror(errno));
          ret = DVB_FAILURE;
        }
    }

    pthread_mutex_unlock(&dev->lock);
    return ret;
}

/**\brief free demux filter
 * \param dmx device number
 * \param dmx filter index
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_FreeFilter(int dev_no, int fhandle)
{
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);

    filter = dmx_get_filter(dev, fhandle);
    if (filter)
    {
        filter->need_free = 1;
    }

    pthread_mutex_unlock(&dev->lock);

    //DVB_INFO("fhandle = %d", fhandle);
    return DVB_SUCCESS;
}

/**\brief start demux filter
 * \param dmx device number
 * \param dmx filter index
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_StartFilter(int dev_no, int fhandle)
{
    DVB_RESULT ret = DVB_SUCCESS;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);

    filter = dmx_get_filter(dev, fhandle);
    if (filter && !filter->enable)
    {
        if (ioctl(filter->fd, DMX_START, 0) < 0)
        {
            DVB_INFO("dmx start filter failed error:%s", strerror(errno));
            ret = DVB_FAILURE;
        }
        else
        {
            filter->enable = 1;
        }
    }

    pthread_mutex_unlock(&dev->lock);
    //DVB_INFO("ret = %d", ret);
    return ret;
}

/**\brief stop demux filter
 * \param dmx device number
 * \param dmx filter index
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_StopFilter(int dev_no, int fhandle)
{
    DVB_RESULT ret = DVB_SUCCESS;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);

    filter = dmx_get_filter(dev, fhandle);
    if (filter && filter->enable)
    {
        if (ioctl(filter->fd, DMX_STOP, 0) < 0)
        {
            DVB_INFO("dmx stop filter failed error:%s", strerror(errno));
            ret = DVB_FAILURE;
        }
        else
        {
            filter->enable = 0;
        }
    }

    pthread_mutex_unlock(&dev->lock);
    //DVB_INFO("ret = %d", ret);
    return ret;
}

/**\brief set demux callback
 * \param dmx device number
 * \param dmx filter index
 * \param dmx filter callback
 * \param dmx filter callback param
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_SetCallback(int dev_no, int fhandle, AML_DMX_DataCb cb, void *user_data)
{
    DVB_RESULT ret = DVB_SUCCESS;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_INFO("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);
    filter = dmx_get_filter(dev, fhandle);
    if (filter)
    {
        filter->cb = cb;
        filter->user_data = user_data;
    }
    else
    {
        ret = DVB_FAILURE;
    }

    pthread_mutex_unlock(&dev->lock);

    //DVB_INFO("ret = %d", ret);
    return ret;
}

/**\brief dmx device uninit, destroy dmx thread
 * \param dmx device number
 * \return DVB_SUCCESS On success, DVB_FAILURE on error.
 */
DVB_RESULT AML_DMX_Close(int dev_no)
{
    int i;
    int open_count = 0;
    dvb_dmx_t *dev = NULL;
    dvb_dmx_filter_t *filter = NULL;
    DVB_RESULT ret = DVB_SUCCESS;

    if (dmx_get_dev(dev_no, &dev))
    {
        DVB_ERROR("wrong dmx device no %d", dev_no);
        return DVB_FAILURE;
    }

    pthread_mutex_lock(&dev->lock);

    for (i = 0; i < DMX_FILTER_COUNT; i++)
    {
        filter = &dev->filter[i];
        if (filter->used && filter->dev_no == dev_no)
        {
            if (filter->enable)
            {
                if (ioctl(filter->fd, DMX_STOP, 0)<0)
                {
                    DVB_ERROR("fails to stop filter. fd:%d", filter->fd);
                    ret = DVB_FAILURE;
                }
            }
            close(filter->fd);
        }
        else if (filter->used)
        {
            open_count++;
        }
    }

    pthread_mutex_unlock(&dev->lock);

    if (open_count == 0)
    {
        dev->running = 0;
        pthread_join(dev->thread, NULL);
    }

    pthread_mutex_destroy(&dev->lock);

    return ret;
}
