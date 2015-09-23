cc = $(CROSS_COMPILE)gcc
out = cpumon
src = cpumon.c

ldflags = $(LDFLAGS) -lm -lxcb
ccflags += $(CFLAGS)

.PHONY: all clean distclean
all: $(out)

$(out):
	$(cc) $(ccflags) $(ldflags) -o $(out) $(src)
	@echo "(==) $(out) compilation finished" | grep --color -E "^...."

clean:
	-rm -f $(out)

distclean: clean
