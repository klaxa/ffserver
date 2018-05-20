all: ffserver
LAV_FLAGS = $(shell pkg-config --libs --cflags libavformat libavcodec libavutil)
LUA_FLAGS = $(shell pkg-config --libs --cflags lua5.3)
CFLAGS=-fsanitize=address -fsanitize=undefined
# LAV_FLAGS = -L/usr/local/lib -lavcodec -lavformat -lavutil

ffserver: segment.o publisher.o lavfhttpd.o configreader.o ffserver.c
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) $(LUA_FLAGS) -lpthread -o ffserver segment.o publisher.o lavfhttpd.o configreader.o ffserver.c

segment.o: segment.c segment.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c segment.c

publisher.o: publisher.c publisher.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c publisher.c

lavfhttpd.o: lavfhttpd.c httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c lavfhttpd.c

configreader.o: configreader.c configreader.h httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) $(LUA_FLAGS) -c configreader.c
clean:
	rm -f *.o ffserver
