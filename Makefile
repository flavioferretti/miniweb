# Makefile per MiniWeb con libmicrohttpd (OpenBSD/BSD make)

PROG=      miniweb
SRCDIR=    src
INCDIR=    include
BUILDDIR=  build

# Source files per la nuova implementazione con libmicrohttpd
SRCS=      ${SRCDIR}/main.c \
           ${SRCDIR}/routes.c \
           ${SRCDIR}/template_engine.c \
           ${SRCDIR}/metrics.c \
           ${SRCDIR}/man.c

# Object files (placed in build directory)
OBJS=      ${BUILDDIR}/main.o \
           ${BUILDDIR}/routes.o \
           ${BUILDDIR}/template_engine.o \
           ${BUILDDIR}/metrics.o \
           ${BUILDDIR}/man.o

CC?=       cc

# CFLAGS: Compiler options
CFLAGS+=   -std=c99 -O2 -Wall -Wextra -pedantic
CFLAGS+=   -fstack-protector-strong -I${INCDIR}
CFLAGS+=   -D_FORTIFY_SOURCE=2
CFLAGS+=   -Wformat -Wformat-security
CFLAGS+=   -g -D__OpenBSD__
CFLAGS+=   -D_OPENBSD  # Per abilitare pledge/unveil

# Flags per libmicrohttpd
CFLAGS+=   -D_DEFAULT_SOURCE -I/usr/local/include

# LDFLAGS: Linker options
LDFLAGS+=  -Wl,-z,relro,-z,now -fno-plt -L/usr/local/lib
LDADD=     -Wl,-rpath,/usr/local/lib -lmicrohttpd -lssl -lcrypto -lz

# LDFLAGS+= -Wl,-z,relro,-z,now -fno-plt -L/usr/local/lib
# LDADD = -Wl,-rpath,/usr/local/lib -lmicrohttpd -lsqlite3 -lssl -lcrypto -lz

PREFIX?=   /usr/local
BINDIR?=   ${PREFIX}/bin

# Default target
all: ${BUILDDIR}/${PROG}

# Linking phase
${BUILDDIR}/${PROG}: ${OBJS}
	@mkdir -p ${BUILDDIR}
	${CC} ${LDFLAGS} -o $@ ${OBJS} ${LDADD}

# Predefined build variants
debug:
	${MAKE} CFLAGS="${CFLAGS} -g -O0" clean all

run: ${BUILDDIR}/${PROG}
	./${BUILDDIR}/${PROG}

# Installation target
install: ${BUILDDIR}/${PROG}
	install -d ${BINDIR}
	install -m 755 ${BUILDDIR}/${PROG} ${BINDIR}/${PROG}

clean:
	rm -rf ${BUILDDIR}
	rm -f ${PROG} *.o

# Compilation rules for individual objects
${BUILDDIR}/main.o: ${SRCDIR}/main.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/main.c -o $@

${BUILDDIR}/man.o: ${SRCDIR}/main.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/man.c -o $@

${BUILDDIR}/metrics.o: ${SRCDIR}/metrics.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/metrics.c -o $@

${BUILDDIR}/routes.o: ${SRCDIR}/routes.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/routes.c -o $@

${BUILDDIR}/template_engine.o: ${SRCDIR}/template_engine.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/template_engine.c -o $@




.PHONY: all clean run debug install
