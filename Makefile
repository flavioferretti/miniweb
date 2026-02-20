# Makefile for MiniWeb (kqueue, no libmicrohttpd) (OpenBSD/BSD make)

# --- Project Structure and Program Name ---
PROG=      miniweb
SRCDIR=    src
INCDIR=    include
BUILDDIR=  build

# --- Source Files Selection ---
# Defines the list of C source files located in the src directory
SRCS=      ${SRCDIR}/main.c \
           ${SRCDIR}/routes.c \
           ${SRCDIR}/template_engine.c \
           ${SRCDIR}/metrics.c \
           ${SRCDIR}/man.c \
           ${SRCDIR}/http_utils.c \
           ${SRCDIR}/urls.c \
           ${SRCDIR}/networking.c \
           ${SRCDIR}/http_handler.c \
           ${SRCDIR}/conf.c \
           ${SRCDIR}/pkg_manager.c

# --- Object Files Mapping ---
# Maps source files to their respective object files in the build directory
OBJS=      ${BUILDDIR}/main.o \
           ${BUILDDIR}/routes.o \
           ${BUILDDIR}/template_engine.o \
           ${BUILDDIR}/metrics.o \
           ${BUILDDIR}/man.o \
           ${BUILDDIR}/http_utils.o \
           ${BUILDDIR}/urls.o \
           ${BUILDDIR}/networking.o \
           ${BUILDDIR}/http_handler.o \
           ${BUILDDIR}/conf.o \
           ${BUILDDIR}/pkg_manager.o

# --- Compiler Configuration ---
CC?=       cc

# CFLAGS: Standard C99, optimization, and comprehensive warning flags
CFLAGS+=   -std=c99 -O2 -Wall -Wextra -pedantic
CFLAGS+=   -fstack-protector-strong -I${INCDIR}
CFLAGS+=   -D_FORTIFY_SOURCE=2
CFLAGS+=   -Wformat -Wformat-security
CFLAGS+=   -g

# Local include paths for OpenBSD
CFLAGS+=   -D_DEFAULT_SOURCE

# --- Linker Configuration ---
# LDFLAGS: Hardening options (Relro/Now) and library search paths
LDFLAGS+=  -Wl,-z,relro,-z,now -fno-plt -L/usr/local/lib

# LDADD: Libraries linked to the project (math, threads)
LDADD=     -lm -lpthread

# --- Installation Paths ---
PREFIX?=   /usr/local
BINDIR?=   ${PREFIX}/bin

# --- Primary Targets ---

# Default target: builds the main application binary
all: ${BUILDDIR}/${PROG}

# Linking phase: produces the final executable from object files
${BUILDDIR}/${PROG}: ${OBJS}
	@mkdir -p ${BUILDDIR}
	${CC} ${LDFLAGS} -o $@ ${OBJS} ${LDADD}

# Debug variant: compiles with no optimization and full debug symbols
debug:
	${MAKE} CFLAGS="${CFLAGS} -g -O0" clean all

# Run target: convenient way to build and execute the application
run: ${BUILDDIR}/${PROG}
	./${BUILDDIR}/${PROG}

# Installation target: copies the binary to the system path
install: ${BUILDDIR}/${PROG}
	install -d ${BINDIR}
	install -m 755 ${BUILDDIR}/${PROG} ${BINDIR}/${PROG}

# Man page installation: deploys the manual and updates the whatis database
man:
	doas cp docs/miniweb.1 /usr/local/man/man1
	doas makewhatis /usr/local/man

# Clean target: removes the build directory and artifacts
clean:
	rm -rf ${BUILDDIR}
	rm -f ${PROG} *.o

# --- Individual Compilation Rules ---
# These rules handle the compilation of each .c file into its .o counterpart

${BUILDDIR}/conf.o: ${SRCDIR}/conf.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/conf.c -o $@

${BUILDDIR}/pkg_manager.o: ${SRCDIR}/pkg_manager.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/pkg_manager.c -o $@

${BUILDDIR}/http_handler.o: ${SRCDIR}/http_handler.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http_handler.c -o $@

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

${BUILDDIR}/networking.o: ${SRCDIR}/networking.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/networking.c -o $@

${BUILDDIR}/routes.o: ${SRCDIR}/routes.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/routes.c -o $@

${BUILDDIR}/template_engine.o: ${SRCDIR}/template_engine.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/template_engine.c -o $@

${BUILDDIR}/urls.o: ${SRCDIR}/urls.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/urls.c -o $@

# --- Testing Infrastructure ---
TESTDIR=    tests

# Unit tests: builds and runs logic-specific tests
unit-tests: ${BUILDDIR}/routes_test ${BUILDDIR}/template_test
	./${BUILDDIR}/routes_test
	./${BUILDDIR}/template_test

# Integration test: runs an external shell script against the compiled binary
integration-test: ${BUILDDIR}/${PROG}
	bash ${TESTDIR}/integration_endpoints.sh

# Compilation of the routes test binary
${BUILDDIR}/routes_test: ${TESTDIR}/routes_test.c ${SRCDIR}/routes.c ${SRCDIR}/metrics.c ${SRCDIR}/man.c ${SRCDIR}/networking.c ${SRCDIR}/template_engine.c ${SRCDIR}/http_utils.c ${SRCDIR}/urls.c ${SRCDIR}/http_handler.c ${SRCDIR}/conf.c ${SRCDIR}/pkg_manager.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} ${LDFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/routes_test.c ${SRCDIR}/networking.c ${SRCDIR}/routes.c ${SRCDIR}/metrics.c ${SRCDIR}/man.c ${SRCDIR}/template_engine.c ${SRCDIR}/http_utils.c ${SRCDIR}/http_handler.c ${SRCDIR}/conf.c ${SRCDIR}/pkg_manager.c ${SRCDIR}/urls.c ${LDADD}

# Compilation of the template engine test binary
${BUILDDIR}/template_test: ${TESTDIR}/template_test.c ${SRCDIR}/template_engine.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/template_test.c ${SRCDIR}/template_engine.c

# .PHONY ensures these targets are always executed regardless of file existence
.PHONY: all clean run debug install man unit-tests integration-test
