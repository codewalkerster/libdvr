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
 * \brief
 *
 * \author Gong Ke <ke.gong@amlogic.com>
 * \date 2010-06-07: create the document
 ***************************************************************************/

#define AM_DEBUG_LEVEL 5

//#include <am_debug.h>
#include "am_util.h"
#include <stdio.h>
#include "am_dmx.h"
#include <string.h>
#include <unistd.h>
#include "am_ca.h"
#include "ca.h"
#include "am_key.h"

#define FEND_DEV_NO   (0)

static int s_last_num = -1;

int freq = 0;
int layer = -1;
int src = 0;
int dmx = 0;
int timeout = 60 * 3;

static int bat = 0;
static int nit = 0;
static int user = 0;
static int pat = 0;
static int eit = 0;
static int pall = 0;

#define USER_MAX 10
static int u_pid[USER_MAX] = {[0 ... USER_MAX-1] = -1};
static int u_para[USER_MAX] = {[0 ... USER_MAX-1] = 0};
static char *u_ext[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_dsc_algo[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_odd_iv[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_odd_key[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_even_iv[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_even_key[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_00_iv[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static char *u_00_key[USER_MAX] = {[0 ... USER_MAX-1] = NULL};
static int u_bs[USER_MAX] = {[0 ... USER_MAX-1] = 0};
static int u_para_g;
static FILE *fp[USER_MAX];
static FILE *fp_e[USER_MAX];

//static AM_PES_Handle_t h_pes[USER_MAX];
static char *u_path_g;
static char u_path_g_b[256];
static char *u_input_g;
static char u_input_g_file[256];
static int key_fd;
static int s_index = 0;
static int key_index[128];
/*
   u_para format:
    d1 - 0:sec 1:pes
    d2 - 1:crc : sec only
    d3 - 1:print
    d4 - 1:swfilter
    d5 - 1:ts_tap :pes only
    d6 - 1:w2file
*/
#define PARA_TYPE      0xf
#define PARA_CRC       0xf0
#define PARA_PR        0xf00
#define PARA_SF        0xf000
#define PARA_DMX_TAP   0xf0000
#define PARA_FILE      0xf00000
#define PARA_PES2ES    0xf000000
#define PARA_CA         0x20000000
#define PARA_ES         0x40000000
#define PARA_PCR         0x80000000

#define get_u_para(_i) (u_para[(_i)]? u_para[(_i)] : u_para_g)

int inject_file(int dvr_no, char *inject_name);
int inject_file_and_rec_close(void);

#if 0
static void pes_cb(AM_PES_Handle_t handle, uint8_t *buf, int size) {
    int u = (int)(long)AM_PES_GetUserData(handle);
    printf("pes cb u=%d b=%p, s:%d\n", u, buf, size);
    int ret = fwrite(buf, 1, size, fp_e[u-1]);
    if (ret != size)
        printf("data w lost\n");

}
#endif
static void dump_bytes(int dev_no, int fid, const uint8_t *data, int len, void *user_data)
{
    int u = (int)(long)user_data;

    if (pall) {
        int i;
        printf("data:\n");
        for (i = 0; i < len; i++)
        {
            printf("%02x ", data[i]);
            if (((i + 1) % 16) == 0) printf("\n");
        }
        if ((i % 16) != 0 ) printf("\n");
    }
#if 1
    if (bat & PARA_PR) {
        if (data[0] == 0x4a) {
            printf("sec:tabid:0x%02x,bouquet_id:0x%02x%02x,section num:%4d,lat_section_num:%4d\n", data[0],
                data[3], data[4], data[6], data[7]);
        }

    }
    else if (nit & PARA_PR) {

        if (data[0] == 0x40) {
            printf("section:%8d,max:%8d\n", data[6], data[7]);
            if ((data[6] != s_last_num + 1) && (s_last_num != -1))//µÚÒ»¸ö»òÕß²»Á¬Ðø
            {
                if (s_last_num == data[7])//ÉÏÒ»¸öÊÇMAX
                {
                    if (data[6] != 0)//ÉÏÒ»¸öMAX Õâ¸ö²»ÊÇ 0
                    {
                        printf("##drop packet,tabid:0x%4x,cur:%8d,last:%8d,max:%8d\n", data[0],
                        data[6], s_last_num, data[7]);
                        //stop_section_flag =1;
                    }
                    else
                    {
                    }
                }
                else//ÉÏÒ»¸ö²»ÊÇ
                {
                    printf("##drop packet,tabid:%4x,cur:%8d,last:%8d,max:%8d\n", data[0],
                    data[6], s_last_num,data[7]);
                    //stop_section_flag = 1;
                }
            }
            else
            {
                //printf("section:%8d,",sectiondata->m_pucData[6]);
            }
            s_last_num = data[6];
        }
    }
    else if (pat & PARA_PR) {
        if (data[0] == 0x0)
            printf("%02x: %02x %02x %02x %02x %02x %02x %02x %02x\n", data[0], data[1], data[2], data[3], data[4],
                data[5], data[6], data[7], data[8]);
    }
    else {
        if (!user_data) {
            printf("%02x %02x %02x %02x %02x %02x %02x %02x %02x\n", data[0], data[1], data[2], data[3], data[4],
                data[5], data[6], data[7], data[8]);
            return;
        }

        if (get_u_para(u - 1) & PARA_PR)
            printf("[%d:%d %d] %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", u-1, u_pid[u-1], len,
                data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8]);
        if (get_u_para(u - 1) & PARA_FILE) {
            {
                int ret = fwrite(data, 1, len, fp[u - 1]);
                if (ret != len)
                    printf("data w lost\n");
            }
#if 0
            if (get_u_para(u - 1) & PARA_PES2ES) {
                if (!h_pes[u - 1]) {
                    AM_PES_Para_t para = {.packet = pes_cb, .user_data = (void*)(long)u, .payload_only = AM_TRUE,};
                    AM_PES_Create(&h_pes[u - 1], &para);
                }
                AM_PES_Decode(h_pes[u - 1], data, len);
            }
#endif
        }
    }
#endif
}
static void get_key_from_para(char *para_org, int para_len, char *key, int *len)
{
    int i = 0;
    int dst = 0;
    char hi = 0;
    char lo = 0;

    printf("get key len:%d, key:%s\n", para_len, para_org);
    if (para_len <= 2)
        return ;
    para_len -= 2;

    dst = 2;

    printf("key:");
    while (para_len >= 2) {
        hi = 0;
        if (para_org[dst] >= 0x30 && para_org[dst] <= 0x39)    {
            hi = para_org[dst] - 0x30;
        } else if (para_org[dst] >= 0x41 && para_org[dst] <= 0x46) {
            hi = para_org[dst] - 0x37;
        } else if (para_org[dst] >= 0x61 && para_org[dst] <= 0x66) {
            hi = para_org[dst] - 0x57;
        }
        lo = 0;
        if (para_org[dst+1] >= 0x30 && para_org[dst+1] <= 0x39)    {
            lo = para_org[dst+1] - 0x30;
        } else if (para_org[dst+1] >= 0x41 && para_org[dst+1] <= 0x46) {
            lo = para_org[dst+1] - 0x37;
        } else if (para_org[dst+1] >= 0x61 && para_org[dst+1] <= 0x66) {
            lo = para_org[dst+1] - 0x57;
        }
        key[i] = (hi << 4) | lo;
        printf("0x%0x ", key[i]);
        i++;
        dst += 2;
        para_len -= 2;
    }
    printf("\n");
    *len = i;
}

static int get_section(int dmx, int timeout)
{
    int fid_user[USER_MAX];
    int ca_index[USER_MAX];
    int i;
    int pcr_flag = 0;

    struct dmx_sct_filter_params param;
    struct dmx_pes_filter_params pparam;

    memset(&ca_index, -1, sizeof(ca_index));

    for (i = 0; i < USER_MAX; i++) {
        if (u_pid[i] != -1) {
            printf("set %d\n", __LINE__);
            AM_TRY(AM_DMX_AllocateFilter(dmx, &fid_user[i]));
            printf("set %d\n", __LINE__);

            AM_TRY(AM_DMX_SetCallback(dmx, fid_user[i], dump_bytes, (void*)(long)(i + 1)));
            printf("set %d\n", __LINE__);

            if (u_bs[i]) {
                AM_TRY(AM_DMX_SetBufferSize(dmx, fid_user[i], u_bs[i]));
                printf("buffersize => %d\n", u_bs[i]);
            }
            printf("set %d\n", __LINE__);

            if (get_u_para(i) & PARA_CA) {
                    int dsc_algo = 0;
                    int dsc_type = 0;
                    int parity = 0;
                    int key_userid = 0;
                    int key_algo = 0;
                    char key[32];
                    char key_len = 0;

                    ca_open(dmx);
                    key_fd = key_open();
                    if (u_dsc_algo[i]) {
                        sscanf(u_dsc_algo[i], "%x:%x", &dsc_algo, &dsc_type);
                    }
                    if (dsc_type == CA_DSC_COMMON_TYPE)
                        key_userid = DSC_NETWORK;
                    else if (dsc_type == CA_DSC_TSD_TYPE)
                        key_userid = DSC_LOC_DEC;
                    else if (dsc_type == CA_DSC_TSE_TYPE)
                        key_userid = DSC_LOC_ENC;
                    else {
                        printf("dsc_type:%d invalid\n", dsc_type);
                        key_userid = DSC_NETWORK;
                    }

                    switch (dsc_algo) {
                        case CA_ALGO_AES_ECB_CLR_END:
                        case CA_ALGO_AES_ECB_CLR_FRONT:
                        case CA_ALGO_AES_CBC_CLR_END:
                        case CA_ALGO_AES_CBC_IDSA:
                        case CA_ALGO_CPCM_LSA_MDI_CBC:
                        case CA_ALGO_CPCM_LSA_MDD_CBC:
                            key_algo = KEY_ALGO_AES;
                            break;
                        case CA_ALGO_TDES_ECB_CLR_END:
                            key_algo = KEY_ALGO_TDES;
                            break;
                        case CA_ALGO_DES_SCTE41:
                        case CA_ALGO_DES_SCTE52:
                            key_algo = KEY_ALGO_DES;
                            break;
                        case CA_ALGO_CSA2:
                            key_algo = KEY_ALGO_CSA2;
                            break;
                        case CA_ALGO_CSA3 :
                            key_algo = KEY_ALGO_CSA3;
                            break;
                        case CA_ALGO_ASA:
                            key_algo = KEY_ALGO_ND;
                            break;
                        case CA_ALGO_ASA_LIGHT:
                            key_algo = KEY_ALGO_NDL;
                            break;
                        case CA_ALGO_S17_ECB_CLR_END:
                        case CA_ALGO_S17_ECB_CTS:
                            key_algo = KEY_ALGO_S17;
                            break;
                        default:
                            printf("dsc algo:%d error\n", dsc_algo);
                    }
                    ca_index[i] = ca_alloc_chan(dmx, u_pid[i], dsc_algo, dsc_type);

                    if (u_odd_key[i]) {
                        memset(key, 0, sizeof(key));
                        get_key_from_para(u_odd_key[i], strlen(u_odd_key[i]), key, &key_len);
                        key_index[s_index] = key_alloc(key_fd, 0);
                        key_config(key_fd, key_index[s_index], key_userid, key_algo, 0);
                        key_set(key_fd, key_index[s_index], key, key_len);
                        ca_set_key(dmx, ca_index[i], CA_KEY_ODD_TYPE, key_index[s_index]);
                        s_index++;
                    }
                    if (u_even_key[i]) {
                        memset(key, 0, sizeof(key));
                        get_key_from_para(u_even_key[i], strlen(u_even_key[i]), key, &key_len);
                        key_index[s_index] = key_alloc(key_fd, 0);
                        key_config(key_fd, key_index[s_index], key_userid, key_algo, 0);
                        key_set(key_fd, key_index[s_index],key, key_len);
                        ca_set_key(dmx, ca_index[i], CA_KEY_EVEN_TYPE, key_index[s_index]);
                        s_index++;
                    }
                    if (u_00_key[i]) {
                        memset(key, 0, sizeof(key));
                        get_key_from_para(u_00_key[i], strlen(u_00_key[i]), key, &key_len);
                        key_index[s_index] = key_alloc(key_fd, 0);
                        key_config(key_fd, key_index[s_index], key_userid, key_algo, 0);
                        key_set(key_fd, key_index[s_index], key, key_len);
                        ca_set_key(dmx, ca_index[i], CA_KEY_00_TYPE, key_index[s_index]);
                        s_index++;
                    }
                    if (u_odd_iv[i]) {
                        memset(key, 0, sizeof(key));
                        get_key_from_para(u_odd_iv[i], strlen(u_odd_iv[i]), key, &key_len);
                        key_index[s_index] =  key_alloc(key_fd, 1);
                        key_config(key_fd, key_index[s_index], key_userid, key_algo, 0);
                        key_set(key_fd, key_index[s_index], key, key_len);
                        ca_set_key(dmx, ca_index[i], CA_KEY_ODD_IV_TYPE, key_index[s_index]);
                        s_index++;
                    }
                    if (u_even_iv[i]) {
                        memset(key, 0, sizeof(key));
                        get_key_from_para(u_even_iv[i], strlen(u_even_iv[i]), key, &key_len);
                        key_index[s_index] = key_alloc(key_fd, 1);
                        key_config(key_fd, key_index[s_index], key_userid, key_algo, 0);
                        key_set(key_fd, key_index[s_index], key, key_len);
                        ca_set_key(dmx, ca_index[i], CA_KEY_EVEN_IV_TYPE, key_index[s_index]);
                        s_index++;
                    }
                    if (u_00_iv[i]) {
                        memset(key, 0, sizeof(key));
                        get_key_from_para(u_00_iv[i], strlen(u_00_iv[i]), key, &key_len);
                        key_index[s_index] =  key_alloc(key_fd, 1);
                        key_config(key_fd, key_index[s_index], key_userid, key_algo, 0);
                        key_set(key_fd, key_index[s_index], key, key_len);
                        ca_set_key(dmx, ca_index[i], CA_KEY_00_IV_TYPE, key_index[s_index]);
                        s_index++;
                    }
            }

            if (get_u_para(i) & PARA_TYPE) {/*pes*/
                memset(&pparam, 0, sizeof(pparam));
                pparam.pid = u_pid[i];
                if (get_u_para(i) & PARA_PCR) {
                    pparam.pes_type = DMX_PES_PCR0;
                    printf("set %d\n", __LINE__);
                    pcr_flag = 1;
                }
                else
                    pparam.pes_type = DMX_PES_OTHER;

                pparam.input = DMX_IN_FRONTEND;
                pparam.output = DMX_OUT_TAP;
                if (get_u_para(i) & PARA_DMX_TAP)
                    pparam.output = DMX_OUT_TSDEMUX_TAP;
                if (get_u_para(i) & PARA_SF)
                    pparam.flags |= 0x100;
                if (get_u_para(i) & PARA_ES) {
                    pparam.flags |= DMX_ES_OUTPUT;
                    printf("set es flag\n");
                }
                AM_TRY(AM_DMX_SetPesFilter(dmx, fid_user[i], &pparam));
            } else {/*sct*/
                int v[16] = {[0 ... 15] = 0};
                int m[16] = {[0 ... 15] = 0};
                int ii;
                memset(&param, 0, sizeof(param));
                param.pid = u_pid[i];
                if (u_ext[i]) {
                sscanf(u_ext[i], "%x:%x,%x:%x,%x:%x,%x:%x"
                    ",%x:%x,%x:%x,%x:%x,%x:%x"
                    ",%x:%x,%x:%x,%x:%x,%x:%x"
                    ",%x:%x,%x:%x,%x:%x,%x:%x",
                    &v[0], &m[0], &v[1], &m[1],
                    &v[2], &m[2], &v[3], &m[3],
                    &v[4], &m[4], &v[5], &m[5],
                    &v[6], &m[6], &v[7], &m[7],
                    &v[8], &m[8], &v[9], &m[9],
                    &v[10], &m[10], &v[11], &m[11],
                    &v[12], &m[12], &v[13], &m[13],
                    &v[14], &m[14], &v[15], &m[15]);
                for (ii = 0; ii < 16; ii++) {
                    if (m[ii]) {
                        param.filter.filter[ii] = v[ii];
                        param.filter.mask[ii] = m[ii];
                        printf("ext%d: [%d]%x:%x\n", i, ii, v[ii], m[ii]);
                    }
                }
                }
                if (get_u_para(i) & PARA_CRC)
                    param.flags = DMX_CHECK_CRC;
                if (get_u_para(i) & PARA_SF)
                    param.flags |= 0x100;
                AM_TRY(AM_DMX_SetSecFilter(dmx, fid_user[i], &param));
            }

            if (get_u_para(i) & PARA_FILE) {
                char name[32];
                sprintf(name, "%s/u_%d.dump", u_path_g, i);
                fp[i] = fopen(name, "wb");
                if (fp[i])
                    printf("file open:[%s]\n", name);
                sprintf(name, "%s/u_%d.es.dump", u_path_g, i);
                fp_e[i] = fopen(name, "wb");
                if (fp_e[i])
                    printf("file open:[%s]\n", name);
            }

            AM_TRY(AM_DMX_StartFilter(dmx, fid_user[i]));
        }
    }

    printf("TS SRC = %d\n", src);
    if (src == 1 && u_input_g) {
        printf("inject file :%s\n",u_input_g_file);
        inject_file(dmx, u_input_g_file);
    }
//    sleep(1);
    if (pcr_flag == 1) {
        printf("get stc:\n");
        AM_DMX_GetSTC(dmx, fid_user[0]);
    }
    sleep(timeout);

    for (i = 0; i < USER_MAX; i++) {
        if (ca_index[i] != -1)
            ca_free_chan(dmx, ca_index[i]);
    }

    for (i=0; i < s_index; i++) {
        if (key_index[i] != -1)
            key_free(key_fd, key_index[i]);
    }

    for (i = 0; i < USER_MAX; i++) {
        if (u_pid[i] != -1) {
            AM_TRY(AM_DMX_StopFilter(dmx, fid_user[i]));
            AM_TRY(AM_DMX_FreeFilter(dmx, fid_user[i]));
            if ((get_u_para(i) & PARA_FILE) && fp[i])
                fclose(fp[i]);
            if (get_u_para(i) & PARA_CA)
                ca_free_chan(dmx, ca_index[i]);
        }
    }
    ca_close(dmx);

    return 0;
}

#if 0
static int setlayer(int layer/*1/2/4/7*/)
{
    AM_ErrorCode_t ret;

    struct dtv_property p = {.cmd = DTV_ISDBT_LAYER_ENABLED, .u.data = layer};
    struct dtv_properties props = {.num = 1, .props = &p};
    printf("AM FEND SetProp layer:%d\n", props.props[0].u.data);
    ret = AM_FEND_SetProp(FEND_DEV_NO, &props);
    return 0;
}
#endif

int get_para(char *argv)
{
    #define CASE(name, len, type, var) \
        if (!strncmp(argv, name"=", (len) + 1)) { \
            sscanf(&argv[(len)+1], type, &var); \
            printf("param["name"] => "type"\n", var); \
        }
    #define CASESTR(name, len, type, var) \
        if (!strncmp(argv, name"=", (len) + 1)) { \
            var = &argv[(len) + 1]; \
            printf("param["name"] => "type"\n", var); \
        }

    CASE("freq",      4, "%i", freq)
    else CASE("src",  3, "%i", src)
    else CASE("dmx",  3, "%i", dmx)
    else CASE("pat",  3, "%x", pat)
    else CASE("eit",  3, "%x", eit)
    else CASE("layer",5, "%i", layer)
    else CASE("bat",  3, "%x", bat)
    else CASE("nit",  3, "%x", nit)
    else CASE("timeout", 7, "%i", timeout)
    else CASE("pall", 4, "%i", pall)
    else CASE("pid0", 4, "%i", u_pid[0])
    else CASE("pid1", 4, "%i", u_pid[1])
    else CASE("pid2", 4, "%i", u_pid[2])
    else CASE("pid3", 4, "%i", u_pid[3])
    else CASE("pid4", 4, "%i", u_pid[4])
    else CASE("para0", 5, "%x", u_para[0])
    else CASE("para1", 5, "%x", u_para[1])
    else CASE("para2", 5, "%x", u_para[2])
    else CASE("para3", 5, "%x", u_para[3])
    else CASE("para4", 5, "%x", u_para[4])
    else CASESTR("ext0", 4, "%s", u_ext[0])
    else CASESTR("ext1", 4, "%s", u_ext[1])
    else CASESTR("ext2", 4, "%s", u_ext[2])
    else CASESTR("ext3", 4, "%s", u_ext[3])
    else CASESTR("ext4", 4, "%s", u_ext[4])
    else CASESTR("da0", 3, "%s", u_dsc_algo[0])
    else CASESTR("da1", 3, "%s", u_dsc_algo[1])
    else CASESTR("da2", 3, "%s", u_dsc_algo[2])
    else CASESTR("da3", 3, "%s", u_dsc_algo[3])
    else CASESTR("odd_iv0", 7, "%s", u_odd_iv[0])
    else CASESTR("odd_iv1", 7, "%s", u_odd_iv[1])
    else CASESTR("odd_iv2", 7, "%s", u_odd_iv[2])
    else CASESTR("odd_iv3", 7, "%s", u_odd_iv[3])
    else CASESTR("odd_key0", 8, "%s", u_odd_key[0])
    else CASESTR("odd_key1", 8, "%s", u_odd_key[1])
    else CASESTR("odd_key2", 8, "%s", u_odd_key[2])
    else CASESTR("odd_key3", 8, "%s", u_odd_key[3])
    else CASESTR("even_iv0", 8, "%s", u_even_iv[0])
    else CASESTR("even_iv1", 8, "%s", u_even_iv[1])
    else CASESTR("even_iv2", 8, "%s", u_even_iv[2])
    else CASESTR("even_iv3", 8, "%s", u_even_iv[3])
    else CASESTR("even_key0", 9, "%s", u_even_key[0])
    else CASESTR("even_key1", 9, "%s", u_even_key[1])
    else CASESTR("even_key2", 9, "%s", u_even_key[2])
    else CASESTR("even_key3", 9, "%s", u_even_key[3])
    else CASESTR("00_iv0", 6, "%s", u_00_iv[0])
    else CASESTR("00_iv1", 6, "%s", u_00_iv[1])
    else CASESTR("00_iv2", 6, "%s", u_00_iv[2])
    else CASESTR("00_iv3", 6, "%s", u_00_iv[3])
    else CASESTR("00_key0", 7, "%s", u_00_key[0])
    else CASESTR("00_key1", 7, "%s", u_00_key[1])
    else CASESTR("00_key2", 7, "%s", u_00_key[2])
    else CASESTR("00_key3", 7, "%s", u_00_key[3])
    else CASE("bs0", 3, "%i", u_bs[0])
    else CASE("bs1", 3, "%i", u_bs[1])
    else CASE("bs2", 3, "%i", u_bs[2])
    else CASE("bs3", 3, "%i", u_bs[3])
    else CASE("bs4", 3, "%i", u_bs[4])
    else CASE("para", 4, "%x", u_para_g)
    else CASESTR("path", 4, "%s", u_path_g)
    else CASESTR("input", 4, "%s", u_input_g)

    if (u_path_g) {
        char *e = strchr(u_path_g, ' ');
        if (e) {
            int l = e - u_path_g;
            memcpy(u_path_g_b, u_path_g, l);
            u_path_g_b[l] = '\0';
            u_path_g = u_path_g_b;
        }
    }

    if (u_input_g) {
        memcpy(u_input_g_file, u_input_g + 1, strlen(u_input_g) - 1);
    }

    return 0;
}

int main(int argc, char **argv)
{
    AM_DMX_OpenPara_t para;
//    AM_FEND_OpenPara_t fend_para;
//    struct dvb_frontend_parameters p;
//    fe_status_t status;
    int ret = 0;
    int i;

//    memset(&fend_para, 0, sizeof(fend_para));

    if (argc == 1)
    {
        printf(
            "Usage:%s [freq=] [src=] [dmx=] [layer=] [timeout=] [pat=] [eit=] [bat=] [nit=] [pidx=] [parax=] [para=] [extx=] [bsx=] [out=]\n"
            "  default   - src:0 dmx:0 layer:-1 parax:0\n"
            "  x         - 0~4\n"
            "  para      - d6->|111111|<-d1\n"
            "    d1 - 0:sec 1:pes (means enable for pat/eit/bat/nit)\n"
            "    d2 - 1:crc : sec only\n"
            "    d3 - 1:print\n"
            "    d4 - 1:swfilter\n"
            "    d5 - 1:ts tap : pes only\n"
            "    d6 - 1:w2file\n"
            "    d7 - 1:convert pes to es format, pes only\n"
            "  ext       - tid....\n"
            "    eg. 0x82:0xff,0x02:0xff\n"
            "    up to 16 filter data:mask(s)\n"
            "  bs        - buffersize\n"
            "  path      - output path\n"
            , argv[0]);
        return 0;
    }

    for (i = 1; i < argc; i++)
        get_para(argv[i]);

    memset(&para, 0, sizeof(para));
    //para.use_sw_filter = AM_TRUE;
    //para.dvr_fifo_no = 1;
    AM_TRY(AM_DMX_Open(dmx, &para));

    if (u_input_g) {
        AM_DMX_SetInput(dmx, INPUT_LOCAL);
        AM_DMX_SetSource(dmx, 0);
    }

    memset(&key_index, -1, sizeof(key_index));

    get_section(dmx, timeout);

    AM_DMX_Close(dmx);

    if (src == 1 && u_input_g)
        inject_file_and_rec_close();
    return ret;
}

