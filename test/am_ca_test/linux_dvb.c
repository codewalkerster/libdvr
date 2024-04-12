#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
/***************************************************************************
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
/**\file
 * \brief Linux DVB demux 驱动
 *
 * \author Gong Ke <ke.gong@amlogic.com>
 * \date 2010-07-21: create the document
 ***************************************************************************/

#define printf_LEVEL 5

#include "am_mem.h"
//#include <am_misc.h>
#include <stdio.h>
#include "am_dmx_internal.h"
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>

/*add for config define for linux dvb *.h*/
//#include <am_config.h>
#include "dmx.h"

#define open(a...)\
    ({\
     int ret, times = 3;\
     do {\
        ret = open(a);\
        if (ret == -1)\
        {\
           usleep(100*1000);\
        }\
     } while (ret == -1 && times--);\
     ret;\
     })

/****************************************************************************
 * Type definitions
 ***************************************************************************/

typedef struct
{
    char   dev_name[32];
    int    fd[DMX_FILTER_COUNT];
} DVBDmx_t;

/****************************************************************************
 * Static data definitions
 ***************************************************************************/

static AM_ErrorCode_t dvb_open(AM_DMX_Device_t *dev, const AM_DMX_OpenPara_t *para);
static AM_ErrorCode_t dvb_close(AM_DMX_Device_t *dev);
static AM_ErrorCode_t dvb_alloc_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter);
static AM_ErrorCode_t dvb_free_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter);
static AM_ErrorCode_t dvb_set_sec_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, const struct dmx_sct_filter_params *params);
static AM_ErrorCode_t dvb_set_pes_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, const struct dmx_pes_filter_params *params);
static AM_ErrorCode_t dvb_enable_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, AM_Bool_t enable);
static AM_ErrorCode_t dvb_set_buf_size(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, int size);
static AM_ErrorCode_t dvb_poll(AM_DMX_Device_t *dev, AM_DMX_FilterMask_t *mask, int timeout);
static AM_ErrorCode_t dvb_read(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, uint8_t *buf, int *size);
static AM_ErrorCode_t dvb_set_input(AM_DMX_Device_t *dev, int input);
static AM_ErrorCode_t dvb_get_stc(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter);
static AM_ErrorCode_t dvb_set_source(AM_DMX_Device_t *dev, int source);

const AM_DMX_Driver_t linux_dvb_dmx_drv = {
.open  = dvb_open,
.close = dvb_close,
.alloc_filter = dvb_alloc_filter,
.free_filter  = dvb_free_filter,
.set_sec_filter = dvb_set_sec_filter,
.set_pes_filter = dvb_set_pes_filter,
.enable_filter  = dvb_enable_filter,
.set_buf_size   = dvb_set_buf_size,
.poll           = dvb_poll,
.read           = dvb_read,
.set_input     = dvb_set_input,
.set_source     = dvb_set_source,
.get_stc        = dvb_get_stc
};

/****************************************************************************
 * Static functions
 ***************************************************************************/

static AM_ErrorCode_t dvb_open(AM_DMX_Device_t *dev, const AM_DMX_OpenPara_t *para)
{
    DVBDmx_t *dmx;
    int i;

    UNUSED(para);

    dmx = (DVBDmx_t*)malloc(sizeof(DVBDmx_t));
    if (!dmx)
    {
        printf("not enough memory");
        return AM_DMX_ERR_NO_MEM;
    }

    snprintf(dmx->dev_name, sizeof(dmx->dev_name), "/dev/dvb0.demux%d", dev->dev_no);

    for (i = 0; i < DMX_FILTER_COUNT; i++)
        dmx->fd[i] = -1;

    dev->drv_data = dmx;
    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_close(AM_DMX_Device_t *dev)
{
    DVBDmx_t *dmx = (DVBDmx_t*)dev->drv_data;

    free(dmx);
    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_alloc_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter)
{
    DVBDmx_t *dmx = (DVBDmx_t*)dev->drv_data;
    int fd;

    fd = open(dmx->dev_name, O_RDWR);
    if (fd == -1)
    {
        printf("cannot open \"%s\" (%s)", dmx->dev_name, strerror(errno));
        return AM_DMX_ERR_CANNOT_OPEN_DEV;
    }

    dmx->fd[filter->id] = fd;

    filter->drv_data = (void*)(long)fd;

    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_free_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter)
{
    DVBDmx_t *dmx = (DVBDmx_t*)dev->drv_data;
    int fd = (long)filter->drv_data;

    close(fd);
    dmx->fd[filter->id] = -1;

    return AM_SUCCESS;
}
AM_ErrorCode_t dvb_get_stc(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter)
{
    int fd = (long)filter->drv_data;
    int ret;
    struct dmx_stc stc;
    int i = 0;

    UNUSED(dev);

    for (i = 0; i < 3; i++)
    {
        memset(&stc, 0, sizeof(struct dmx_stc));
        stc.num = i;
        ret = ioctl(fd, DMX_GET_STC, &stc);
        if (ret == 0) {
            printf("get stc num %d: base:0x%0x, stc:0x%lx\n", stc.num, stc.base, stc.stc);
        } else {
            printf("get stc %d, fail\n", i);
        }
    }
    return 0;
}

static AM_ErrorCode_t dvb_set_sec_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, const struct dmx_sct_filter_params *params)
{
    struct dmx_sct_filter_params p;
    int fd = (long)filter->drv_data;
    int ret;

    UNUSED(dev);

    p = *params;
    /*
    if (p.filter.mask[0] == 0) {
        p.filter.filter[0] = 0x00;
        p.filter.mask[0]   = 0x80;
    }
    */
    ret = ioctl(fd, DMX_SET_FILTER, &p);
    if (ret == -1)
    {
        printf("set section filter failed (%s)", strerror(errno));
        return AM_DMX_ERR_SYS;
    }

    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_set_pes_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, const struct dmx_pes_filter_params *params)
{
    int fd = (long)filter->drv_data;
    int ret;

    UNUSED(dev);

    fcntl(fd,F_SETFL,O_NONBLOCK);

    ret = ioctl(fd, DMX_SET_PES_FILTER, params);
    if (ret == -1)
    {
        printf("set section filter failed (%s)", strerror(errno));
        return AM_DMX_ERR_SYS;
    }
    printf("%s success\n", __FUNCTION__);
    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_enable_filter(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, AM_Bool_t enable)
{
    int fd = (long)filter->drv_data;
    int ret;

    UNUSED(dev);

    if (enable)
        ret = ioctl(fd, DMX_START, 0);
    else
        ret = ioctl(fd, DMX_STOP, 0);

    if (ret == -1)
    {
        printf("start filter failed (%s)", strerror(errno));
        return AM_DMX_ERR_SYS;
    }

    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_set_buf_size(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, int size)
{
    int fd = (long)filter->drv_data;
    int ret;

    UNUSED(dev);

    ret = ioctl(fd, DMX_SET_BUFFER_SIZE, size);
    if (ret == -1)
    {
        printf("set buffer size failed (%s)", strerror(errno));
        return AM_DMX_ERR_SYS;
    }

    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_poll(AM_DMX_Device_t *dev, AM_DMX_FilterMask_t *mask, int timeout)
{
    DVBDmx_t *dmx = (DVBDmx_t*)dev->drv_data;
    struct pollfd fds[DMX_FILTER_COUNT];
    int fids[DMX_FILTER_COUNT];
    int i, cnt = 0, ret;

    for (i = 0; i < DMX_FILTER_COUNT; i++)
    {
        if (dmx->fd[i] != -1)
        {
            fds[cnt].events = POLLIN|POLLERR;
            fds[cnt].fd     = dmx->fd[i];
            fids[cnt] = i;
            cnt++;
        }
    }

    if (!cnt)
        return AM_DMX_ERR_TIMEOUT;

    ret = poll(fds, cnt, timeout);
    if (ret <= 0)
    {
        return AM_DMX_ERR_TIMEOUT;
    }

    for (i = 0; i < cnt; i++)
    {
        if (fds[i].revents & (POLLIN | POLLERR))
        {
            AM_DMX_FILTER_MASK_SET(mask, fids[i]);
        }
    }

    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_read(AM_DMX_Device_t *dev, AM_DMX_Filter_t *filter, uint8_t *buf, int *size)
{
    int fd = (long)filter->drv_data;
    int len = *size;
    int ret;
    struct pollfd pfd;

    UNUSED(dev);

    if (fd == -1)
        return AM_DMX_ERR_NOT_ALLOCATED;

    pfd.events = POLLIN | POLLERR;
    pfd.fd     = fd;

    ret = poll(&pfd, 1, 0);
    if (ret <= 0)
        return AM_DMX_ERR_NO_DATA;

    ret = read(fd, buf, len);
    if (ret <= 0)
    {
        if (errno == ETIMEDOUT)
            return AM_DMX_ERR_TIMEOUT;
        printf("read demux failed (%s) %d", strerror(errno), errno);
        return AM_DMX_ERR_SYS;
    }

    *size = ret;
    return AM_SUCCESS;
}

static AM_ErrorCode_t dvb_set_input(AM_DMX_Device_t *dev, int input)
{
    int ret;
    int fd = 0;
    DVBDmx_t *dmx = (DVBDmx_t*)dev->drv_data;

    fd = open(dmx->dev_name, O_RDWR);
    if (fd == -1)
    {
        printf("cannot open for set input \"%s\" (%s)", dmx->dev_name, strerror(errno));
        return AM_DMX_ERR_CANNOT_OPEN_DEV;
    }

    ret = ioctl(fd, DMX_SET_INPUT, input);

    printf("dmx set_input ret:%d, input:%d\n", ret, input);

    close(fd);
    return 0;
}

static AM_ErrorCode_t dvb_set_source(AM_DMX_Device_t *dev, int source)
{
    int ret;
    int fd = 0;
    DVBDmx_t *dmx = (DVBDmx_t*)dev->drv_data;

    fd = open(dmx->dev_name, O_RDWR);
    if (fd == -1)
    {
        printf("cannot open for set dmx source \"%s\" (%s)", dmx->dev_name, strerror(errno));
        return AM_DMX_ERR_CANNOT_OPEN_DEV;
    }

    ret = ioctl(fd, DMX_SET_HW_SOURCE, source);

    printf("dmx set_source ret:%d, source:%d\n", ret, source);

    close(fd);
    return 0;
}

