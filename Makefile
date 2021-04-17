# Net Talk Proxy Makefile
INCLUDES=-I include $(CONFIG) $(EXTRA)
INDENT_FLAGS=-br -ce -i4 -bl -bli0 -bls -c4 -cdw -ci4 -cs -nbfda -l100 -lp -prs -nlp -nut -nbfde -npsl -nss

OBJS = \
	bin/startup.o \
	bin/proxy.o

all: host

internal: prepare
	@echo "  CC    src/startup.c"
	@$(CC) $(CFLAGS) $(INCLUDES) src/startup.c -o bin/startup.o
	@echo "  CC    src/proxy.c"
	@$(CC) $(CFLAGS) $(INCLUDES) src/proxy.c -o bin/proxy.o
	@echo "  LD    bin/nettalk-proxy"
	@$(LD) -o bin/nettalk-proxy $(OBJS) $(LDFLAGS)

prepare:
	@mkdir -p bin

verbose:
	@make $(TARGET) EXTRA=-DVERBOSE_MODE

host:
	@make internal \
		CC=gcc \
		LD=gcc \
		CFLAGS='-c -Wall -Wextra -O2 -ffunction-sections -fdata-sections -Wstrict-prototypes' \
		LDFLAGS='-s -Wl,--gc-sections -Wl,--relax'

nodaemon:
	@make internal \
		CC=gcc \
		LD=gcc \
		CFLAGS='-c -Wall -Wextra -O2 -ffunction-sections -fdata-sections -Wstrict-prototypes -DNO_DAEMON' \
		LDFLAGS='-s -Wl,--gc-sections -Wl,--relax'

mipsel:
	@make internal \
		CC=mips-unknown-linux-gnu-gcc \
		LD=mips-unknown-linux-gnu-gcc \
		CFLAGS='-c $(MIPSEL_CFLAGS) -I $(ESLIB_INC) -O2 -EL' \
		LDFLAGS='$(MIPSEL_LDFLAGS) -L $(ESLIB_DIR) -les-mipsel -EL'

mipseb:
	@make internal \
		CC=mips-unknown-linux-gnu-gcc \
		LD=mips-unknown-linux-gnu-gcc \
		CFLAGS='-c $(MIPSEB_CFLAGS) -I $(ESLIB_INC) -O2 -EB' \
		LDFLAGS='$(MIPSEB_LDFLAGS) -L $(ESLIB_DIR) -les-mipseb -EB'

arm:
	@make internal \
		CC=arm-linux-gnueabi-gcc \
		LD=arm-linux-gnueabi-gcc \
		CFLAGS='-c $(ARM_CFLAGS) -I $(ESLIB_INC) -O2' \
		LDFLAGS='$(ARM_LDFLAGS) -L $(ESLIB_DIR) -les-arm'

install:
	@cp -v bin/nettalk-proxy /usr/bin/nettalk-proxy

uninstall:
	@rm -fv /usr/bin/nettalk-proxy

post:
	@echo "  STRIP nettalk-proxy"
	@sstrip bin/nettalk-proxy
	@echo "  UPX   nettalk-proxy"
	@upx bin/nettalk-proxy
	@echo "  LCK   nettalk-proxy"
	@perl -pi -e 's/UPX!/EsNf/g' bin/nettalk-proxy
	@echo "  AEM   nettalk-proxy"
	@nogdb bin/nettalk-proxy

post2:
	@echo "  STRIP nettalk-proxy"
	@sstrip bin/nettalk-proxy
	@echo "  AEM   nettalk-proxy"
	@nogdb bin/nettalk-proxy

indent:
	@indent $(INDENT_FLAGS) ./*/*.h
	@indent $(INDENT_FLAGS) ./*/*.c
	@rm -rf ./*/*~

clean:
	@echo "  CLEAN ."
	@rm -rf bin

analysis:
	@scan-build make
	@cppcheck --force */*.h
	@cppcheck --force */*.c

gendoc:
	@doxygen aux/doxygen.conf
