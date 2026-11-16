TARGET = FretsOnFire_Dreamcast.elf
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
	kos-c++ -o $@ $^ -Wl,--wrap=maple_queue_frame -lvorbis -logg -lsh4zam -lm

romdisk.img: build/assets.stamp

build/assets.stamp: tools/prepare_assets.py tools/prepare_classic_sounds.py Makefile
	@test -n "$(CLASSIC_DATA)" || { echo 'Informe CLASSIC_DATA=/caminho/para/Frets on Fire-1.3.110/data'; exit 1; }
	rm -rf $(KOS_ROMDISK_DIR)
	PYTHONDONTWRITEBYTECODE=1 python3 tools/prepare_assets.py "$(CLASSIC_DATA)" $(KOS_ROMDISK_DIR)
	touch $@

SONGS_DIR ?= $(CLASSIC_DATA)/songs

cdi: $(TARGET)
	test -d "$(SONGS_DIR)"
	rm -rf build/cd/songs
	mkdir -p build/cd/songs
	cp -R "$(SONGS_DIR)/." build/cd/songs/
	mkdcdisc -e $(TARGET) -o $(TARGET:.elf=.cdi) -n "Frets on Fire Dreamcast" -N -D build/cd

clean:
	rm -rf build
	rm -f $(OBJECTS) $(SOURCES:.cpp=.d) romdisk.img $(TARGET) $(TARGET:.elf=.cdi)

-include $(SOURCES:.cpp=.d)
