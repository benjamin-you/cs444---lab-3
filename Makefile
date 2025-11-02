CC = gcc
CFLAGS = -fpic -Wall -Werror -pthread
SHARED_LIB_FLAGS = -shared

# there are better ways to write a Makefile, but this is simple and works on all platforms

# Library target - removed *.h dependency since we don't have header files
libcrawler.so: crawler.c ../os-crawler-framework/api.h 
	$(CC) $(CFLAGS) $(SHARED_LIB_FLAGS) -o libcrawler.so crawler.c

# Test program (links with our library)
crawl: ../os-crawler-framework/driver.c libcrawler.so
	$(CC) $(CFLAGS) -L. -o crawl ../os-crawler-framework/driver.c -lcrawler 

# Clean up
clean:
	rm -f *.o *.so crawl

# Set library path for running test (may not be necessary on your system)
run-crawl: crawl
	export LD_LIBRARY_PATH=.:$$LD_LIBRARY_PATH && ./crawl ../os-crawler-framework/test1
	export LD_LIBRARY_PATH=.:$$LD_LIBRARY_PATH && ./crawl ../os-crawler-framework/test2
	export LD_LIBRARY_PATH=.:$$LD_LIBRARY_PATH && ./crawl ../os-crawler-framework/test3

.PHONY: clean run-crawl