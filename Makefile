# Makefile for MiniWeb with libmicrohttpd (OpenBSD/BSD make)

PROG=      miniweb
SRCDIR=    src
INCDIR=    include
BUILDDIR=  build

# Source files for libmicrohttpd MiniWeb
SRCS=      ${SRCDIR}/main.c \
           ${SRCDIR}/routes.c \
           ${SRCDIR}/template_engine.c \
           ${SRCDIR}/metrics.c \
           ${SRCDIR}/man.c \
           ${SRCDIR}/http_utils.c \
            ${SRCDIR}/urls.c

# Object files (placed in build directory)
OBJS=      ${BUILDDIR}/main.o \
           ${BUILDDIR}/routes.o \
           ${BUILDDIR}/template_engine.o \
           ${BUILDDIR}/metrics.o \
           ${BUILDDIR}/man.o \
           ${BUILDDIR}/http_utils.o \
           ${BUILDDIR}/urls.o

CC?=       cc

# CFLAGS: Compiler options
CFLAGS+=   -std=c99 -O2 -Wall -Wextra -pedantic
CFLAGS+=   -fstack-protector-strong -I${INCDIR}
CFLAGS+=   -D_FORTIFY_SOURCE=2
CFLAGS+=   -Wformat -Wformat-security
CFLAGS+=   -g

# Flags per libmicrohttpd
CFLAGS+=   -D_DEFAULT_SOURCE -I/usr/local/include

# LDFLAGS: Linker options
LDFLAGS+=  -Wl,-z,relro,-z,now -fno-plt -L/usr/local/lib
LDADD=     -Wl,-rpath,/usr/local/lib -lmicrohttpd -lm -lpthread -lssl -lcrypto -lz

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
${BUILDDIR}/http_utils.o: ${SRCDIR}/http_utils.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http_utils.c -o $@

${BUILDDIR}/main.o: ${SRCDIR}/main.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/main.c -o $@

${BUILDDIR}/man.o: ${SRCDIR}/man.c
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

${BUILDDIR}/urls.o: ${SRCDIR}/urls.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/urls.c -o $@

TESTDIR=    tests

unit-tests: ${BUILDDIR}/routes_test ${BUILDDIR}/template_test
	./${BUILDDIR}/routes_test
	./${BUILDDIR}/template_test

integration-test: ${BUILDDIR}/${PROG}
	bash ${TESTDIR}/integration_endpoints.sh

${BUILDDIR}/routes_test: ${TESTDIR}/routes_test.c ${SRCDIR}/routes.c ${SRCDIR}/metrics.c ${SRCDIR}/man.c ${SRCDIR}/template_engine.c ${SRCDIR}/http_utils.c ${SRCDIR}/urls.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} ${LDFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/routes_test.c ${SRCDIR}/routes.c ${SRCDIR}/metrics.c ${SRCDIR}/man.c ${SRCDIR}/template_engine.c ${SRCDIR}/http_utils.c ${SRCDIR}/urls.c ${LDADD}

${BUILDDIR}/template_test: ${TESTDIR}/template_test.c ${SRCDIR}/template_engine.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/template_test.c ${SRCDIR}/template_engine.c

.PHONY: all clean run debug install unit-tests integration-test
