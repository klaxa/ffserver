all: ffserver
LAV_FLAGS = $(shell pkg-config --libs --cflags libavformat libavcodec libavutil)
LUA_FLAGS = $(shell pkg-config --libs --cflags lua5.3)
MHD_FLAGS = $(shell pkg-config --libs --cflags libmicrohttpd)
# CFLAGS=-fsanitize=address -fsanitize=undefined
CFLAGS=
# LAV_FLAGS = -L/usr/local/lib -lavcodec -lavformat -lavutil

ffserver: segment.o publisher.o fileserver.o lavfhttpd.o lmhttpd.o configreader.o ffserver.c
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) $(LUA_FLAGS) $(MHD_FLAGS) -lpthread -o ffserver segment.o publisher.o fileserver.o lavfhttpd.o lmhttpd.o configreader.o ffserver.c

segment.o: segment.c segment.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c segment.c

publisher.o: publisher.c publisher.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c publisher.c

fileserver.o: fileserver.c fileserver.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c fileserver.c

lavfhttpd.o: lavfhttpd.c httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c lavfhttpd.c

lmhttpd.o: lmhttpd.c httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) $(MHD_FLAGS) -lpthread -c lmhttpd.c

configreader.o: configreader.c configreader.h httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) $(LUA_FLAGS) -c configreader.c
clean:
	rm -f *.o ffserver
