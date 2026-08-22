IOP_OBJS = mgtrace.o imports.o
IOP_CFLAGS += -G0 -O2 -Wall -Wextra -I../..

IOP_IMPORT_INCS += \
	sio/sio2man \
	system/sifcmd \
	system/sifman \
	system/sysclib \
	system/threadman

include $(PS2SDKSRC)/Defs.make
include $(PS2SDKSRC)/iop/Rules.bin.make
include $(PS2SDKSRC)/iop/Rules.make
include $(PS2SDKSRC)/iop/Rules.release
