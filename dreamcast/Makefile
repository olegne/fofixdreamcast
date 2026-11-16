TARGET = FoFiX_Dreamcast_R1.elf
SOURCES = src/main.cpp src/audio.cpp src/chart.cpp src/view.cpp
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

romdisk.img: build/demo.stamp

build/demo.stamp: tools/make_demo.py
	python3 $< $(KOS_ROMDISK_DIR)
	touch $@

cdi: $(TARGET)
	mkdcdisc -e $(TARGET) -o $(TARGET:.elf=.cdi) -n "FoFiX Dreamcast R1" -N

clean:
	rm -rf build
	rm -f $(OBJECTS) $(SOURCES:.cpp=.d) romdisk.img $(TARGET) $(TARGET:.elf=.cdi)

-include $(SOURCES:.cpp=.d)
