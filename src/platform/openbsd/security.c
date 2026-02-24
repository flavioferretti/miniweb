#include <miniweb/platform/openbsd/security.h>

#include <fcntl.h>
#include <unistd.h>

#include <miniweb/core/log.h>

/**
 * Apply OpenBSD unveil(2)/pledge(2) sandboxing when available.
 */
void
miniweb_apply_openbsd_security(const miniweb_conf_t *config)
{
#ifdef __OpenBSD__
	unveil("/etc", "r");
	unveil("/usr/share/man", "r");
	unveil("/usr/X11R6/man", "r");
	unveil("/usr/local/man", "r");
	unveil("/usr/local/share/man", "r");
	unveil(config->static_dir, "rwc");
	unveil(config->templates_dir, "r");
	unveil("/tmp", "rwc");
	unveil("/dev", "r");
	unveil("/var/run", "r");
	unveil("/var/db/pkg", "r");
	unveil("/usr/local", "r");
	unveil("/usr/bin", "r");
	unveil("/usr/sbin", "r");
	unveil("/bin", "r");
	unveil("/sbin", "r");
	unveil("/usr/local/bin", "r");
	unveil("/etc/passwd", "r");
	unveil("/etc/group", "r");
	unveil("/etc/resolv.conf", "r");
	unveil(NULL, NULL);

	const char *promises =
	    "stdio rpath wpath cpath inet route proc exec vminfo ps getpw";
	if (pledge(promises, NULL) == -1) {
		log_errno("pledge");
		log_error("Continuing without pledge...");
	} else if (config->verbose) {
		log_debug("Pledge promises set: %s", promises);
	}
#else
	if (config->verbose)
		log_debug("OpenBSD security features disabled on this platform.");
#endif
}
