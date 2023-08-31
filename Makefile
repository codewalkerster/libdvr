OUTPUT_FILES := libamdvr.so am_fend_test am_dmx_test am_smc_test dvr_wrapper_test

CFLAGS  := -Wall -O2 -fPIC -Iinclude
LDFLAGS := -L$(TARGET_DIR)/usr/lib -lmediahal_tsplayer -laudio_client -llog -lpthread -ldl

LIBAMDVR_SRCS := \
	src/dvb_dmx_wrapper.c\
	src/dvb_utils.c\
	src/dvr_record.c\
	src/dvr_utils.c\
	src/index_file.c\
	src/record_device.c\
	src/dvb_frontend_wrapper.c\
	src/dvr_playback.c\
	src/dvr_segment.c\
	src/dvr_wrapper.c\
	src/list_file.c\
	src/segment.c\
	src/segment_dataout.c\
	src/am_crypt.c\
	src/dvr_mutex.c

LIBAMDVR_OBJS := $(patsubst %.c,%.o,$(LIBAMDVR_SRCS))

AM_FEND_TEST_SRCS := \
	test/am_fend_test/am_fend_test.c
AM_FEND_TEST_OBJS := $(patsubst %.c,%.o,$(AM_FEND_TEST_SRCS))

AM_DMX_TEST_SRCS := \
	test/am_dmx_test/am_dmx_test.c
AM_DMX_TEST_OBJS := $(patsubst %.c,%.o,$(AM_DMX_TEST_SRCS))

AM_SMC_TEST_SRCS := \
	test/am_smc_test/am_sc2_smc_test.c \
	test/am_smc_test/am_smc.c \
	test/am_smc_test/aml.c \
	test/am_smc_test/am_time.c \
	test/am_smc_test/am_evt.c \
	test/am_smc_test/am_thread.c
AM_SMC_TEST_OBJS := $(patsubst %.c,%.o,$(AM_SMC_TEST_SRCS))

DVR_WRAPPER_TEST_SRCS := \
	test/dvr_wrapper_test/dvr_wrapper_test.c
DVR_WRAPPER_TEST_OBJS := $(patsubst %.c,%.o,$(DVR_WRAPPER_TEST_SRCS))


all: $(OUTPUT_FILES)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

libamdvr.so: $(LIBAMDVR_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

am_fend_test: $(AM_FEND_TEST_OBJS) libamdvr.so
	$(CC) -o $@ $(AM_FEND_TEST_OBJS) -L. -lamdvr $(LDFLAGS)

am_dmx_test: $(AM_DMX_TEST_OBJS) libamdvr.so
	$(CC) -o $@ $(AM_DMX_TEST_OBJS) -L. -lamdvr $(LDFLAGS)

am_smc_test: $(AM_SMC_TEST_OBJS) libamdvr.so
	$(CC) -o $@ $(AM_SMC_TEST_OBJS) -L. -lamdvr $(LDFLAGS)

dvr_wrapper_test: $(DVR_WRAPPER_TEST_OBJS) libamdvr.so
	$(CC) -o $@ $(DVR_WRAPPER_TEST_OBJS) -L. -lamdvr $(LDFLAGS)

install: $(OUTPUT_FILES)
	install -m 0755 ./libamdvr.so $(STAGING_DIR)/usr/lib
	install -m 0755 ./libamdvr.so $(TARGET_DIR)/usr/lib
	install -d -m 0755 $(STAGING_DIR)/usr/include/libdvr
	install -m 0644 ./include/* $(STAGING_DIR)/usr/include/libdvr
	install -m 0755 am_fend_test $(STAGING_DIR)/usr/bin
	install -m 0755 am_dmx_test $(STAGING_DIR)/usr/bin
	install -m 0755 am_smc_test $(STAGING_DIR)/usr/bin
	install -m 0755 dvr_wrapper_test $(STAGING_DIR)/usr/bin

clean:
	rm -f $(LIBAMDVR_OBJS) $(AM_FEND_TEST_OBJS) $(AM_DMX_TEST_OBJS) $(DVR_WRAPPER_TEST_OBJS) $(OUTPUT_FILES)

.PHONY: all install clean
