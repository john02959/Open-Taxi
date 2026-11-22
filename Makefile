EE_BIN = taxi.elf
EE_OBJS = main.o audsrv_irx.o

GSKIT ?= $(PS2DEV)/gsKit
EE_INCS += -I$(GSKIT)/include
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS = -lgskit -ldmakit -lpad -laudsrv -lcdvd -lpatches -lc -lkernel -lm
EE_CFLAGS += -O2 -Wall

BIN2C = $(PS2SDK)/bin/bin2c

all: $(EE_BIN)

audsrv_irx.c: $(PS2SDK)/iop/irx/audsrv.irx
	$(BIN2C) $< $@ audsrv_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) audsrv_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
