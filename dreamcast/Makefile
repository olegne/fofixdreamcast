TARGET = FoFiX_Dreamcast.elf
SOURCES = src/main.cpp src/audio.cpp src/chart.cpp src/song_library.cpp src/view.cpp
OBJECTS = $(SOURCES:.cpp=.o) romdisk.o
KOS_ROMDISK_DIR = build/romdisk
CXXFLAGS += -std=c++17 -Wall -Wextra -MMD -MP

.PHONY: all clean cdi
all: $(TARGET)

ifndef KOS_BASE
$(error Carregue o environ.sh do KallistiOS antes de compilar)
endif
include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJECTS)
	kos-c++ -o $@ $^ -lvorbis -logg -lm

romdisk.img: build/assets.stamp

build/assets.stamp: tools/prepare_assets.py tools/make_demo.py Makefile
	rm -rf $(KOS_ROMDISK_DIR)
	PYTHONDONTWRITEBYTECODE=1 python3 tools/prepare_assets.py ../data $(KOS_ROMDISK_DIR)
	touch $@

cdi: $(TARGET)
ifneq ($(strip $(SONGS_DIR)),)
	test -d "$(SONGS_DIR)"
	rm -rf build/cd/songs
	mkdir -p build/cd/songs
	cp -R "$(SONGS_DIR)/." build/cd/songs/
endif
	mkdcdisc -e $(TARGET) -o $(TARGET:.elf=.cdi) -n "FoFiX Dreamcast" -N $(if $(strip $(SONGS_DIR)),-D build/cd)

clean:
	rm -rf build
	rm -f $(OBJECTS) $(SOURCES:.cpp=.d) romdisk.img $(TARGET) $(TARGET:.elf=.cdi)

-include $(SOURCES:.cpp=.d)
