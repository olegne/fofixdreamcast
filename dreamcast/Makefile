TARGET = FretsOnFire_Dreamcast.elf
SOURCES = src/main.cpp src/audio.cpp src/chart.cpp src/song_library.cpp src/view.cpp
SOURCES += src/credits.cpp src/records.cpp src/settings.cpp
SOURCES += src/language.cpp src/chooser.cpp
SOURCES += src/editor.cpp src/editor_vmu.cpp
SOURCES += src/ranking.cpp src/ranking_wire.cpp
OBJECTS = $(SOURCES:.cpp=.o) romdisk.o
KOS_ROMDISK_DIR = build/romdisk
CXXFLAGS += -std=c++17 -Wall -Wextra -MMD -MP
CPPFLAGS += -I$(KOS_PORTS)/zlib/inst/include

.PHONY: all clean cdi
all: $(TARGET)

ifndef KOS_BASE
$(error Carregue o environ.sh do KallistiOS antes de compilar)
endif
include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJECTS)
	kos-c++ -o $@ $^ -Wl,--wrap=maple_queue_frame -lvorbis -logg -lsh4zam -lz -lm

romdisk.img: build/assets.stamp

build/assets.stamp: tools/prepare_assets.py tools/prepare_classic_sounds.py tools/prepare_locale.py Makefile
	@test -n "$(CLASSIC_DATA)" || { echo 'Informe CLASSIC_DATA=/caminho/para/Frets on Fire-1.3.110/data'; exit 1; }
	rm -rf $(KOS_ROMDISK_DIR)
	PYTHONDONTWRITEBYTECODE=1 python3 tools/prepare_assets.py "$(CLASSIC_DATA)" $(KOS_ROMDISK_DIR)
	touch $@

SONGS_DIR ?= $(CLASSIC_DATA)/songs
# Optional prepared Ogg files used as sources by the editor.
IMPORT_DIR ?=
# Optional PNG images selectable in the editor's Set Cassette Label dialog.
LABEL_DIR ?=
# Optional text file containing a compatible HTTP ranking endpoint.
RANKING_URL_FILE ?=

cdi: $(TARGET)
	test -d "$(SONGS_DIR)"
	test -z "$(IMPORT_DIR)" || test -d "$(IMPORT_DIR)"
	test -z "$(LABEL_DIR)" || test -d "$(LABEL_DIR)"
	test -z "$(RANKING_URL_FILE)" || test -f "$(RANKING_URL_FILE)"
	rm -rf build/cd/songs build/cd/imports build/cd/labels
	rm -f build/cd/ranking.url
	mkdir -p build/cd/songs
	cp -R "$(SONGS_DIR)/." build/cd/songs/
	@if test -n "$(IMPORT_DIR)"; then mkdir -p build/cd/imports && cp -R "$(IMPORT_DIR)/." build/cd/imports/; fi
	@if test -n "$(RANKING_URL_FILE)"; then cp "$(RANKING_URL_FILE)" build/cd/ranking.url; fi
	PYTHONDONTWRITEBYTECODE=1 python3 tools/prepare_labels.py "$(CLASSIC_DATA)/songs" build/cd --choices "$(LABEL_DIR)"
	PYTHONDONTWRITEBYTECODE=1 python3 tools/prepare_scores.py build/cd
	mkdcdisc -e $(TARGET) -o $(TARGET:.elf=.cdi) -n "Frets on Fire Dreamcast" -N -D build/cd

clean:
	rm -rf build
	rm -f $(OBJECTS) $(SOURCES:.cpp=.d) romdisk.img $(TARGET) $(TARGET:.elf=.cdi)

-include $(SOURCES:.cpp=.d)
