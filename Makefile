# make        -> hdclone.exe + hdclone-gui.exe        (32 bits: Windows XP..11)
#                hdclone64.exe + hdclone-gui64.exe    (64 bits: necessarios para VSS no Windows 64 bits)
# make linux  -> hdclone (para testes no Linux)
#
# Requer: gcc-mingw-w64-i686 gcc-mingw-w64-x86-64 libz-mingw-w64-dev
CROSS32 ?= i686-w64-mingw32-
CROSS64 ?= x86_64-w64-mingw32-
CFLAGS  = -O2 -Wall -Wextra
LIBS    = -static -static-libgcc -lz -lole32
XP32    = -Wl,--major-subsystem-version,5,--minor-subsystem-version,1,--major-os-version,5,--minor-os-version,1
XP64    = -Wl,--major-subsystem-version,5,--minor-subsystem-version,2,--major-os-version,5,--minor-os-version,2
GUILIBS = -mwindows -lcomctl32 -lcomdlg32

all: hdclone.exe hdclone-gui.exe hdclone64.exe hdclone-gui64.exe

hdclone.exe: hdclone.c hdclone.rc hdclone.manifest
	$(CROSS32)windres hdclone.rc -O coff -o res32.o
	$(CROSS32)gcc $(CFLAGS) -march=i586 -o $@ hdclone.c res32.o $(LIBS) $(XP32)
	$(CROSS32)strip $@ && rm -f res32.o

hdclone-gui.exe: hdclone_gui.c hdclone.c hdclone_gui.rc hdclone_gui.manifest hdclone.ico
	$(CROSS32)windres hdclone_gui.rc -O coff -o resg32.o
	$(CROSS32)gcc $(CFLAGS) -march=i586 -o $@ hdclone_gui.c resg32.o $(GUILIBS) $(LIBS) $(XP32)
	$(CROSS32)strip $@ && rm -f resg32.o

hdclone64.exe: hdclone.c hdclone64.rc hdclone64.manifest
	$(CROSS64)windres hdclone64.rc -O coff -o res64.o
	$(CROSS64)gcc $(CFLAGS) -o $@ hdclone.c res64.o $(LIBS) $(XP64)
	$(CROSS64)strip $@ && rm -f res64.o

hdclone-gui64.exe: hdclone_gui.c hdclone.c hdclone_gui64.rc hdclone_gui64.manifest hdclone.ico
	$(CROSS64)windres hdclone_gui64.rc -O coff -o resg64.o
	$(CROSS64)gcc $(CFLAGS) -o $@ hdclone_gui.c resg64.o $(GUILIBS) $(LIBS) $(XP64)
	$(CROSS64)strip $@ && rm -f resg64.o

linux: hdclone

hdclone: hdclone.c
	gcc $(CFLAGS) -o $@ hdclone.c -lz

clean:
	rm -f hdclone *.exe *.o
