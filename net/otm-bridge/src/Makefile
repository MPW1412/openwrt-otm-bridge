.PHONY: all compile clean

all compile: otm-bridge

otm-bridge: otm-bridge.c
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -o $@ $< $(LDFLAGS) -lpcap -lmosquitto

clean:
	rm -f otm-bridge
