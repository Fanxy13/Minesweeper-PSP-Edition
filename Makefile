TARGET = minesweeper
OBJS   = main.o

CFLAGS   = -O2 -G0 -Wall -fno-lto
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBS = -lpspgu -lpspge -lpspctrl -lpspdisplay -lpspsdk

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# Auto-package after linking
all: $(TARGET).elf
	mksfoex -d MEMSIZE=1 "Minesweeper" PARAM.SFO
	pack-pbp EBOOT.PBP PARAM.SFO \
		$(if $(wildcard ICON0.PNG),ICON0.PNG,NULL) \
		NULL \
		$(if $(wildcard PIC1.PNG),PIC1.PNG,NULL) \
		NULL NULL \
		$(TARGET).elf NULL
