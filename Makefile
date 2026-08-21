EE_BIN = MC_INSPECTOR.ELF
EE_OBJS = src/main.o
EE_LIBS = -ldebug -lpad -lmc -lkernel
EE_CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
