# Makefile for MiniWeb (kqueue, no libmicrohttpd) (OpenBSD/BSD make)

PROG=      miniweb
SRCDIR=    src
INCDIR=    include
BUILDDIR=  build
TESTDIR=   tests

SRCS=      ${SRCDIR}/app_main.c \
           ${SRCDIR}/net/server.c \
           ${SRCDIR}/net/connection_pool.c \
           ${SRCDIR}/net/worker.c \
           ${SRCDIR}/router/route_table.c \
           ${SRCDIR}/render/template_render.c \
           ${SRCDIR}/modules/metrics/metrics_module.c \
           ${SRCDIR}/modules/metrics/metrics_collectors.c \
           ${SRCDIR}/modules/metrics/metrics_service.c \
           ${SRCDIR}/modules/metrics/metrics_json.c \
           ${SRCDIR}/modules/metrics/metrics_process.c \
           ${SRCDIR}/modules/metrics/metrics_snapshot.c \
           ${SRCDIR}/modules/man/man_module.c \
           ${SRCDIR}/modules/man/man_query.c \
           ${SRCDIR}/modules/man/man_index.c \
           ${SRCDIR}/modules/man/man_render.c \
           ${SRCDIR}/modules/man/man_service.c \
           ${SRCDIR}/modules/man/man_json.c \
           ${SRCDIR}/http/utils.c \
           ${SRCDIR}/http/utils_subprocess.c \
           ${SRCDIR}/router/url_registry.c \
           ${SRCDIR}/router/url_registry_init.c \
           ${SRCDIR}/router/url_registry_lookup.c \
           ${SRCDIR}/router/url_registry_reverse.c \
           ${SRCDIR}/modules/networking/networking_module.c \
           ${SRCDIR}/modules/networking/networking_service.c \
           ${SRCDIR}/modules/networking/networking_json.c \
           ${SRCDIR}/http/response_api.c \
           ${SRCDIR}/http/response_helpers.c \
           ${SRCDIR}/http/response_file.c \
           ${SRCDIR}/http/response_io.c \
           ${SRCDIR}/http/response_pool.c \
           ${SRCDIR}/http/response_file_cache.c \
           ${SRCDIR}/modules/packages/packages_module.c \
           ${SRCDIR}/modules/packages/packages_service.c \
           ${SRCDIR}/modules/packages/packages_json.c \
           ${SRCDIR}/core/heartbeat.c \
           ${SRCDIR}/core/heartbeat_schedule.c \
           ${SRCDIR}/core/heartbeat_dispatch.c \
           ${SRCDIR}/router/router.c \
           ${SRCDIR}/router/module_attach.c \
           ${SRCDIR}/storage/sqlite_db.c \
           ${SRCDIR}/storage/sqlite_stmt.c \
           ${SRCDIR}/storage/sqlite_schema.c \
           ${SRCDIR}/core/conf.c \
           ${SRCDIR}/core/conf_defaults.c \
           ${SRCDIR}/core/conf_validation.c \
           ${SRCDIR}/core/log.c \
           ${SRCDIR}/net/work_queue.c \
           ${SRCDIR}/platform/openbsd/security.c

OBJS=      ${BUILDDIR}/app_main.o \
           ${BUILDDIR}/server.o \
           ${BUILDDIR}/connection_pool.o \
           ${BUILDDIR}/worker.o \
           ${BUILDDIR}/route_table.o \
           ${BUILDDIR}/template_render.o \
           ${BUILDDIR}/metrics_module.o \
           ${BUILDDIR}/metrics_collectors.o \
           ${BUILDDIR}/metrics_service.o \
           ${BUILDDIR}/metrics_json.o \
           ${BUILDDIR}/metrics_process.o \
           ${BUILDDIR}/metrics_snapshot.o \
           ${BUILDDIR}/man_module.o \
           ${BUILDDIR}/man_query.o \
           ${BUILDDIR}/man_index.o \
           ${BUILDDIR}/man_render.o \
           ${BUILDDIR}/man_service.o \
           ${BUILDDIR}/man_json.o \
           ${BUILDDIR}/http_utils.o \
           ${BUILDDIR}/http_utils_subprocess.o \
           ${BUILDDIR}/url_registry.o \
           ${BUILDDIR}/url_registry_init.o \
           ${BUILDDIR}/url_registry_lookup.o \
           ${BUILDDIR}/url_registry_reverse.o \
           ${BUILDDIR}/networking_module.o \
           ${BUILDDIR}/networking_service.o \
           ${BUILDDIR}/networking_json.o \
           ${BUILDDIR}/http_response_api.o \
           ${BUILDDIR}/http_response_helpers.o \
           ${BUILDDIR}/http_response_file.o \
           ${BUILDDIR}/http_response_io.o \
           ${BUILDDIR}/http_response_pool.o \
           ${BUILDDIR}/http_response_file_cache.o \
           ${BUILDDIR}/packages_module.o \
           ${BUILDDIR}/packages_service.o \
           ${BUILDDIR}/packages_json.o \
           ${BUILDDIR}/heartbeat.o \
           ${BUILDDIR}/heartbeat_schedule.o \
           ${BUILDDIR}/heartbeat_dispatch.o \
           ${BUILDDIR}/router.o \
           ${BUILDDIR}/module_attach.o \
           ${BUILDDIR}/sqlite_db.o \
           ${BUILDDIR}/sqlite_stmt.o \
           ${BUILDDIR}/sqlite_schema.o \
           ${BUILDDIR}/conf.o \
           ${BUILDDIR}/conf_defaults.o \
           ${BUILDDIR}/conf_validation.o \
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

man: docs/miniweb.1
	doas cp docs/miniweb.1 /usr/local/man/man1
	doas makewhatis /usr/local/man
	mandoc -Tmarkdown docs/miniweb.1 | \
	awk '/^# DESCRIPTION/ { \
		print "# DESCRIPTION\n---\n![screenshot](https://raw.githubusercontent.com/flavioferretti/miniweb/refs/heads/main/docs/screenshot.png)\n---\n![call graph](https://raw.githubusercontent.com/flavioferretti/miniweb/refs/heads/main/docs/miniweb_diagram.svg)\n---"; \
		next \
	} { print }' > README.md
	rm -rf static/man/*

clean:
	rm -rf ${BUILDDIR}
	rm -f ${PROG} *.o

# --- Individual Compilation Rules ---
# These rules handle the compilation of each .c file into its .o counterpart
# Keep explicit per-file rules for BSD make portability (avoid GNU-specific $< usage).

${BUILDDIR}/packages_module.o: ${SRCDIR}/modules/packages/packages_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/packages/packages_module.c -o $@

${BUILDDIR}/http_response_api.o: ${SRCDIR}/http/response_api.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response_api.c -o $@

${BUILDDIR}/http_response_helpers.o: ${SRCDIR}/http/response_helpers.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response_helpers.c -o $@

${BUILDDIR}/http_response_file.o: ${SRCDIR}/http/response_file.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response_file.c -o $@

${BUILDDIR}/http_response_io.o: ${SRCDIR}/http/response_io.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response_io.c -o $@

${BUILDDIR}/http_response_pool.o: ${SRCDIR}/http/response_pool.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response_pool.c -o $@

${BUILDDIR}/http_response_file_cache.o: ${SRCDIR}/http/response_file_cache.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/response_file_cache.c -o $@

${BUILDDIR}/http_utils.o: ${SRCDIR}/http/utils.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/utils.c -o $@

${BUILDDIR}/http_utils_subprocess.o: ${SRCDIR}/http/utils_subprocess.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/http/utils_subprocess.c -o $@

${BUILDDIR}/app_main.o: ${SRCDIR}/app_main.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/app_main.c -o $@

${BUILDDIR}/server.o: ${SRCDIR}/net/server.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/net/server.c -o $@

${BUILDDIR}/connection_pool.o: ${SRCDIR}/net/connection_pool.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/net/connection_pool.c -o $@

${BUILDDIR}/worker.o: ${SRCDIR}/net/worker.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/net/worker.c -o $@

${BUILDDIR}/man_module.o: ${SRCDIR}/modules/man/man_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_module.c -o $@

${BUILDDIR}/man_query.o: ${SRCDIR}/modules/man/man_query.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_query.c -o $@

${BUILDDIR}/man_index.o: ${SRCDIR}/modules/man/man_index.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_index.c -o $@

${BUILDDIR}/man_render.o: ${SRCDIR}/modules/man/man_render.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_render.c -o $@

${BUILDDIR}/metrics_module.o: ${SRCDIR}/modules/metrics/metrics_module.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_module.c -o $@

${BUILDDIR}/metrics_collectors.o: ${SRCDIR}/modules/metrics/metrics_collectors.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_collectors.c -o $@

${BUILDDIR}/metrics_process.o: ${SRCDIR}/modules/metrics/metrics_process.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_process.c -o $@

${BUILDDIR}/metrics_snapshot.o: ${SRCDIR}/modules/metrics/metrics_snapshot.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_snapshot.c -o $@

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

${BUILDDIR}/url_registry_init.o: ${SRCDIR}/router/url_registry_init.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/url_registry_init.c -o $@

${BUILDDIR}/url_registry_lookup.o: ${SRCDIR}/router/url_registry_lookup.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/url_registry_lookup.c -o $@

${BUILDDIR}/url_registry_reverse.o: ${SRCDIR}/router/url_registry_reverse.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/router/url_registry_reverse.c -o $@

${BUILDDIR}/heartbeat.o: ${SRCDIR}/core/heartbeat.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/heartbeat.c -o $@

${BUILDDIR}/heartbeat_schedule.o: ${SRCDIR}/core/heartbeat_schedule.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/heartbeat_schedule.c -o $@

${BUILDDIR}/heartbeat_dispatch.o: ${SRCDIR}/core/heartbeat_dispatch.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/heartbeat_dispatch.c -o $@

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

${BUILDDIR}/conf_defaults.o: ${SRCDIR}/core/conf_defaults.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/conf_defaults.c -o $@

${BUILDDIR}/conf_validation.o: ${SRCDIR}/core/conf_validation.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/conf_validation.c -o $@

${BUILDDIR}/log.o: ${SRCDIR}/core/log.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/core/log.c -o $@

${BUILDDIR}/work_queue.o: ${SRCDIR}/net/work_queue.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/net/work_queue.c -o $@

${BUILDDIR}/security.o: ${SRCDIR}/platform/openbsd/security.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/platform/openbsd/security.c -o $@

unit-tests: ${BUILDDIR}/routes_test ${BUILDDIR}/template_test ${BUILDDIR}/heartbeat_test ${BUILDDIR}/sqlite_db_test
	./${BUILDDIR}/routes_test
	./${BUILDDIR}/template_test
	./${BUILDDIR}/heartbeat_test
	./${BUILDDIR}/sqlite_db_test

integration-test: ${BUILDDIR}/${PROG}
	bash ${TESTDIR}/integration_endpoints.sh

${BUILDDIR}/routes_test: ${TESTDIR}/routes_test.c ${TESTDIR}/routes_test_stubs.c ${SRCDIR}/router/route_table.c ${SRCDIR}/router/url_registry.c ${SRCDIR}/router/url_registry_init.c ${SRCDIR}/router/url_registry_lookup.c ${SRCDIR}/router/url_registry_reverse.c ${SRCDIR}/router/router.c ${SRCDIR}/router/module_attach.c ${SRCDIR}/render/template_render.c ${SRCDIR}/http/utils.c ${SRCDIR}/http/utils_subprocess.c ${SRCDIR}/http/response_api.c ${SRCDIR}/http/response_helpers.c ${SRCDIR}/http/response_file.c ${SRCDIR}/http/response_io.c ${SRCDIR}/http/response_pool.c ${SRCDIR}/http/response_file_cache.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} ${LDFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/routes_test.c ${TESTDIR}/routes_test_stubs.c ${SRCDIR}/router/route_table.c ${SRCDIR}/router/url_registry.c ${SRCDIR}/router/url_registry_init.c ${SRCDIR}/router/url_registry_lookup.c ${SRCDIR}/router/url_registry_reverse.c ${SRCDIR}/router/router.c ${SRCDIR}/router/module_attach.c ${SRCDIR}/render/template_render.c ${SRCDIR}/http/utils.c ${SRCDIR}/http/utils_subprocess.c ${SRCDIR}/http/response_api.c ${SRCDIR}/http/response_helpers.c ${SRCDIR}/http/response_file.c ${SRCDIR}/http/response_io.c ${SRCDIR}/http/response_pool.c ${SRCDIR}/http/response_file_cache.c ${SRCDIR}/core/log.c ${LDADD}

${BUILDDIR}/template_test: ${TESTDIR}/template_test.c ${SRCDIR}/render/template_render.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/template_test.c ${SRCDIR}/render/template_render.c

${BUILDDIR}/heartbeat_test: ${TESTDIR}/heartbeat_test.c ${SRCDIR}/core/heartbeat.c ${SRCDIR}/core/heartbeat_schedule.c ${SRCDIR}/core/heartbeat_dispatch.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} ${LDFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/heartbeat_test.c ${SRCDIR}/core/heartbeat.c ${SRCDIR}/core/heartbeat_schedule.c ${SRCDIR}/core/heartbeat_dispatch.c ${LDADD}

${BUILDDIR}/sqlite_db_test: ${TESTDIR}/sqlite_db_test.c ${SRCDIR}/storage/sqlite_db.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -I${INCDIR} -o $@ ${TESTDIR}/sqlite_db_test.c ${SRCDIR}/storage/sqlite_db.c

.PHONY: all clean run debug install man unit-tests integration-test

${BUILDDIR}/packages_service.o: ${SRCDIR}/modules/packages/packages_service.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/packages/packages_service.c -o $@

${BUILDDIR}/packages_json.o: ${SRCDIR}/modules/packages/packages_json.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/packages/packages_json.c -o $@

${BUILDDIR}/metrics_service.o: ${SRCDIR}/modules/metrics/metrics_service.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_service.c -o $@

${BUILDDIR}/metrics_json.o: ${SRCDIR}/modules/metrics/metrics_json.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/metrics/metrics_json.c -o $@

${BUILDDIR}/man_service.o: ${SRCDIR}/modules/man/man_service.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_service.c -o $@

${BUILDDIR}/man_json.o: ${SRCDIR}/modules/man/man_json.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/man/man_json.c -o $@

${BUILDDIR}/networking_service.o: ${SRCDIR}/modules/networking/networking_service.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/networking/networking_service.c -o $@

${BUILDDIR}/networking_json.o: ${SRCDIR}/modules/networking/networking_json.c
	@mkdir -p ${BUILDDIR}
	${CC} ${CFLAGS} -c ${SRCDIR}/modules/networking/networking_json.c -o $@
