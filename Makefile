all: ffserver
LAV_CFLAGS = $(shell pkg-config --cflags libavformat libavcodec libavutil)
LAV_LIBS = $(shell pkg-config --libs libavformat libavcodec libavutil)
LUA_CFLAGS = $(shell pkg-config --cflags lua)
LUA_LIBS = $(shell pkg-config --libs lua)
MHD_CFLAGS = $(shell pkg-config --cflags libmicrohttpd)
MHD_LIBS = $(shell pkg-config --libs libmicrohttpd)
# CFLAGS=-fsanitize=address -fsanitize=undefined
CFLAGS=
# LAV_FLAGS = -L/usr/local/lib -lavcodec -lavformat -lavutil

ffserver: segment.o publisher.o fileserver.o lavfhttpd.o lmhttpd.o configreader.o ffserver.c
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) $(LUA_CFLAGS) $(MHD_CFLAGS) -o ffserver segment.o publisher.o fileserver.o lavfhttpd.o lmhttpd.o configreader.o -lpthread $(LAV_LIBS) $(LUA_LIBS) $(MHD_LIBS) ffserver.c

segment.o: segment.c segment.h
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) -c segment.c

publisher.o: publisher.c publisher.h
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) -c publisher.c

fileserver.o: fileserver.c fileserver.h
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) -c fileserver.c

lavfhttpd.o: lavfhttpd.c httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) -c lavfhttpd.c

lmhttpd.o: lmhttpd.c httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) $(MHD_FLAGS) -c lmhttpd.c

configreader.o: configreader.c configreader.h httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_CFLAGS) $(LUA_CFLAGS) -c configreader.c
clean:
	rm -f *.o ffserver
