UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  CC = clang
  CFLAGS = -Wall -O2 -mmacosx-version-min=14.0
  PLATFORM_LIBS = -framework CoreGraphics -framework CoreFoundation \
                  -framework ImageIO -framework CoreServices \
                  -framework ApplicationServices
  PLATFORM_OBJ = platform_macos.o
else ifeq ($(UNAME_S),Linux)
  CC ?= gcc
  CFLAGS = -Wall -O2
  PLATFORM_LIBS = -lX11 -lXtst -lpng
  PLATFORM_OBJ = platform_linux.o
endif

LIBS = -lcurl -lsqlite3 $(PLATFORM_LIBS)

OBJS = bot.o $(PLATFORM_OBJ) botlib.o sds.o cJSON.o sqlite_wrap.o \
       json_wrap.o qrcodegen.o sha1.o

all: tgterm

tgterm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

bot.o: bot.c botlib.h sds.h platform.h
	$(CC) $(CFLAGS) -c bot.c

platform_macos.o: platform_macos.c platform.h
	$(CC) $(CFLAGS) -c platform_macos.c

platform_linux.o: platform_linux.c platform.h
	$(CC) $(CFLAGS) -c platform_linux.c

botlib.o: botlib.c botlib.h sds.h cJSON.h sqlite_wrap.h
	$(CC) $(CFLAGS) -c botlib.c

sds.o: sds.c sds.h sdsalloc.h
	$(CC) $(CFLAGS) -c sds.c

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) -c cJSON.c

sqlite_wrap.o: sqlite_wrap.c sqlite_wrap.h
	$(CC) $(CFLAGS) -c sqlite_wrap.c

json_wrap.o: json_wrap.c cJSON.h
	$(CC) $(CFLAGS) -c json_wrap.c

qrcodegen.o: qrcodegen.c qrcodegen.h
	$(CC) $(CFLAGS) -c qrcodegen.c

sha1.o: sha1.c sha1.h
	$(CC) $(CFLAGS) -c sha1.c

clean:
	rm -f tgterm *.o

.PHONY: all clean
