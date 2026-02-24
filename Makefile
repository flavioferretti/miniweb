# Makefile for MiniWeb (kqueue, no libmicrohttpd) (OpenBSD/BSD make)

PROG=      miniweb
SRCDIR=    src
INCDIR=    include
BUILDDIR=  build
TESTDIR=   tests

SRCS=      ${SRCDIR}/app_main.c \
           ${SRCDIR}/router/route_table.c \
           ${SRCDIR}/render/template_render.c \
           ${SRCDIR}/modules/metrics/metrics_module.c \
           ${SRCDIR}/modules/man/man_module.c \
           ${SRCDIR}/http/utils.c \
           ${SRCDIR}/router/url_registry.c \
           ${SRCDIR}/modules/networking/networking_module.c \
           ${SRCDIR}/http/response.c \
           ${SRCDIR}/modules/packages/packages_module.c \
           ${SRCDIR}/core/heartbeat.c \
           ${SRCDIR}/router/router.c \
           ${SRCDIR}/router/module_attach.c \
           ${SRCDIR}/storage/sqlite_db.c \
           ${SRCDIR}/storage/sqlite_stmt.c \
           ${SRCDIR}/storage/sqlite_schema.c \
           ${SRCDIR}/core/conf.c \
           ${SRCDIR}/core/log.c \
           ${SRCDIR}/net/work_queue.c \
           ${SRCDIR}/platform/openbsd/security.c

OBJS=      ${BUILDDIR}/app_main.o \
           ${BUILDDIR}/route_table.o \
           ${BUILDDIR}/template_render.o \
           ${BUILDDIR}/metrics_module.o \
           ${BUILDDIR}/man_module.o \
           ${BUILDDIR}/http_utils.o \
           ${BUILDDIR}/url_registry.o \
           ${BUILDDIR}/networking_module.o \
           ${BUILDDIR}/http_response.o \
           ${BUILDDIR}/packages_module.o \
           ${BUILDDIR}/heartbeat.o \
           ${BUILDDIR}/router.o \
           ${BUILDDIR}/module_attach.o \
           ${BUILDDIR}/sqlite_db.o \
           ${BUILDDIR}/sqlite_stmt.o \
           ${BUILDDIR}/sqlite_schema.o \
           ${BUILDDIR}/conf.o \
           ${BUILDDIR}/log.o \
           ${BUILDDIR}/work_queue.o \
           ${BUILDDIR}/security.o

CC?=       cc
CFLAGS+=   -std=c99 -O2 -Wall -Wextra -pedantic
CFLAGS+=   -fstack-protector-strong -I${INCDIR}
CFLAGS+=   -D_FORTIFY_SOURCE=2
CFLAGS+=   -Wformat -Wformat-security
CFLAGS+=   -g
CFLAGS+=   -D_DEFAULT_SOURCE

LDFLAGS+=  -Wl,-z,relro,-z,now -fno-plt -L/usr/local/lib
LDADD=     -lm -lpthread

PREFIX?=   /usr/local
BINDIR?=   ${PREFIX}/bin

all: ${BUILDDIR}/${PROG}

${BUILDDIR}/${PROG}: ${OBJS}
	@mkdir -p ${BUILDDIR}
	${CC} ${LDFLAGS} -o $@ ${OBJS} ${LDADD}

debug:
	${MAKE} CFLAGS="${CFLAGS} -g -O0" clean all

run: ${BUILDDIR}/${PROG}
	./${BUILDDIR}/${PROG}

install: ${BUILDDIR}/${PROG}
	install -d ${BINDIR}
	install -m 755 ${BUILDDIR}/${PROG} ${BINDIR}/${PROG}

man:
	doas cp docs/miniweb.1 /usr/local/man/man1
	doas makewhatis /usr/local/man

clean:
	rm -rf ${BUILDDIR}
	rm -f ${PROG} *.o

# --- Individual Compilation Rules ---
# These rules handle the compilation of each .c file into its .o counterpart
# Keep explicit per-file rules for BSD make portability (avoid GNU-specific $< usage).

${BUILDDIR}/packages_module.o: ${SRCDIR}/modules/packages/packages_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/packages/packages_module.c -o $@

${BUILDDIR}/http_response.o: ${SRCDIR}/http/response.c ${SRCDIR}/core/log.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response.c -o $@

${BUILDDIR}/http_utils.o: ${SRCDIR}/http/utils.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/utils.c -o $@

${BUILDDIR}/app_main.o: ${SRCDIR}/app_main.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/app_main.c -o $@

${BUILDDIR}/man_module.o: ${SRCDIR}/modules/man/man_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_module.c -o $@

${BUILDDIR}/metrics_module.o: ${SRCDIR}/modules/metrics/metrics_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_module.c -o $@

${BUILDDIR}/networking_module.o: ${SRCDIR}/modules/networking/networking_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/networking/networking_module.c -o $@

${BUILDDIR}/route_table.o: ${SRCDIR}/router/route_table.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/route_table.c -o $@

${BUILDDIR}/template_render.o: ${SRCDIR}/render/template_render.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/render/template_render.c -o $@

${BUILDDIR}/url_registry.o: ${SRCDIR}/router/url_registry.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/url_registry.c -o $@

${BUILDDIR}/heartbeat.o: ${SRCDIR}/core/heartbeat.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/heartbeat.c -o $@

${BUILDDIR}/router.o: ${SRCDIR}/router/router.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/router.c -o $@

${BUILDDIR}/module_attach.o: ${SRCDIR}/router/module_attach.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/module_attach.c -o $@

${BUILDDIR}/sqlite_db.o: ${SRCDIR}/storage/sqlite_db.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/storage/sqlite_db.c -o $@

${BUILDDIR}/sqlite_stmt.o: ${SRCDIR}/storage/sqlite_stmt.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/storage/sqlite_stmt.c -o $@

${BUILDDIR}/sqlite_schema.o: ${SRCDIR}/storage/sqlite_schema.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/storage/sqlite_schema.c -o $@

${BUILDDIR}/conf.o: ${SRCDIR}/core/conf.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/conf.c -o $@

${BUILDDIR}/log.o: ${SRCDIR}/core/log.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/log.c -o $@

${BUILDDIR}/work_queue.o: ${SRCDIR}/net/work_queue.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/net/work_queue.c -o $@

${BUILDDIR}/security.o: ${SRCDIR}/platform/openbsd/security.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/platform/openbsd/security.c -o $@

unit-tests: ${BUILDDIR}/routes_test ${BUILDDIR}/template_test ${BUILDDIR}/heartbeat_test
	./${BUILDDIR}/routes_test
	./${BUILDDIR}/template_test
	./${BUILDDIR}/heartbeat_test

integration-test: ${BUILDDIR}/${PROG}
	bash ${TESTDIR}/integration_endpoints.sh

${BUILDDIR}/routes_test: ${TESTDIR}/routes_test.c ${TESTDIR}/routes_test_stubs.c ${SRCDIR}/router/route_table.c ${SRCDIR}/router/url_registry.c ${SRCDIR}/router/router.c ${SRCDIR}/router/module_attach.c ${SRCDIR}/render/template_render.c ${SRCDIR}/http/utils.c ${SRCDIR}/http/response.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} ${LDFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/routes_test.c ${TESTDIR}/routes_test_stubs.c ${SRCDIR}/router/route_table.c ${SRCDIR}/router/url_registry.c ${SRCDIR}/router/router.c ${SRCDIR}/router/module_attach.c ${SRCDIR}/render/template_render.c ${SRCDIR}/http/utils.c ${SRCDIR}/http/response.c ${SRCDIR}/core/log.c ${LDADD}

${BUILDDIR}/template_test: ${TESTDIR}/template_test.c ${SRCDIR}/render/template_render.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/template_test.c ${SRCDIR}/render/template_render.c

${BUILDDIR}/heartbeat_test: ${TESTDIR}/heartbeat_test.c ${SRCDIR}/core/heartbeat.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} ${LDFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/heartbeat_test.c ${SRCDIR}/core/heartbeat.c ${LDADD}

.PHONY: all clean run debug install man unit-tests integration-test
