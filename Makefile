all: ffserver
LAV_FLAGS = $(shell pkg-config --libs --cflags libavformat libavcodec libavutil)
CFLAGS=-fsanitize=address -fsanitize=undefined
# LAV_FLAGS = -L/usr/local/lib -lavcodec -lavformat -lavutil

ffserver: segment.o publisher.o lavfhttpd.o ffserver.c
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -o ffserver segment.o publisher.o lavfhttpd.o ffserver.c

segment.o: segment.c segment.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c segment.c

publisher.o: publisher.c publisher.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c publisher.c

lavfhttpd.o: lavfhttpd.c httpd.h
	cc -g -Wall $(CFLAGS) $(LAV_FLAGS) -lpthread -c lavfhttpd.c

clean:
	rm -f *.o ffserver
