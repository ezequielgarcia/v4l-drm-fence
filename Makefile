CROSS_COMPILE ?=

CC	?= $(CROSS_COMPILE)gcc
LDFLAGS	?= -pthread
CFLAGS	?= -g -O2 -W -Wall -std=gnu99 `pkg-config --cflags libdrm` -Wno-unused-parameter
LIBS	:= -lrt -ldrm `pkg-config --libs libdrm libv4l2`

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: drm.o v4l2.o main.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	-rm -f *.o test
