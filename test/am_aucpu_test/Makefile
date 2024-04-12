CROSS_COMPILE ?= /opt/gcc-linaro-6.3.1-2017.02-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc

OUTPUT = am_aucpu_test
INSTALL ?= ./

OBJS = $(patsubst %.c,%.o,$(SRC_FILES))

SRC_FILES = am_dmx_test.c am_inject.c am_dmx.c linux_dvb.c

CFLAGS := -c -Wall -I./

LDFLAGS := -lpthread -static -lc -ldl

all : $(OBJS) $(OUTPUT)

$(OBJS) : %.o : %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT) : $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

install:
	cp -rf $(OUTPUT) $(INSTALL)
	rm -f $(OUTPUT) *.o

clean:
	@rm -f $(OBJS)
