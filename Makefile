target   := gl_test
sources  := main.c video.c
CFLAGS   := -pthread -Wall -O2 -g -I$(SDKTARGETSYSROOT)/usr/include -I$(SDKTARGETSYSROOT)/usr/include/drm -pg
LFLAGS   := -pg
LIBS     := -pthread -ldl -lEGL -lGLESv2 -lgbm -ldrm -lasound -L$(SDKTARGETSYSROOT)/usr/lib -lglfw
packages := sdl2, libpng

# do not edit from here onwards
objects := $(addprefix build/,$(sources:.c=.o))
ifneq ($(packages),)
    LIBS    += $(shell pkg-config --libs-only-l $(packages))
    LFLAGS  += $(shell pkg-config --libs-only-L --libs-only-other $(packages))
    CFLAGS  += $(shell pkg-config --cflags $(packages))
endif

.PHONY: all clean

all: $(target)
clean:
	-rm -rf build
	-rm -f $(target)

$(target): Makefile $(objects)
	$(CC) $(LFLAGS) -o $@ $(objects) $(LIBS)

build/%.o: %.c Makefile
	-mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

-include $(addprefix build/,$(sources:.c=.d))
