# make        -> hdclone.exe (Windows XP..11, 32 bits) usando MinGW-w64
# make linux  -> hdclone (para testes no Linux)
CROSS ?= i686-w64-mingw32-
CFLAGS = -O2 -Wall -Wextra

all: hdclone.exe hdclone-gui.exe

hdclone-gui.exe: hdclone_gui.c hdclone.c hdclone_gui.rc hdclone_gui.manifest hdclone.ico
	$(CROSS)windres hdclone_gui.rc -O coff -o hdclone_gui_res.o
	$(CROSS)gcc $(CFLAGS) -march=i586 -mwindows -o $@ hdclone_gui.c hdclone_gui_res.o -static -static-libgcc -lz -lcomctl32 -lcomdlg32 \
		-Wl,--major-subsystem-version,5,--minor-subsystem-version,1,--major-os-version,5,--minor-os-version,1
	$(CROSS)strip $@
	rm -f hdclone_gui_res.o

hdclone.exe: hdclone.c hdclone.rc hdclone.manifest
	$(CROSS)windres hdclone.rc -O coff -o hdclone_res.o
	$(CROSS)gcc $(CFLAGS) -march=i586 -o $@ hdclone.c hdclone_res.o -static -static-libgcc -lz \
		-Wl,--major-subsystem-version,5,--minor-subsystem-version,1,--major-os-version,5,--minor-os-version,1
	$(CROSS)strip $@
	rm -f hdclone_res.o

linux: hdclone

hdclone: hdclone.c
	gcc $(CFLAGS) -o $@ hdclone.c -lz

clean:
	rm -f hdclone hdclone.exe hdclone-gui.exe *.o
