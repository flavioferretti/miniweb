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
	/* Prima unveil per restringere il filesystem */
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
	unveil("/usr/bin", "rx");        /* Permessi di esecuzione! */
	unveil("/usr/sbin", "rx");       /* Permessi di esecuzione! */
	unveil("/bin", "rx");            /* Permessi di esecuzione! */
	unveil("/sbin", "rx");           /* Permessi di esecuzione! */
	unveil("/usr/local/bin", "rx");  /* Permessi di esecuzione! */
	unveil("/etc/passwd", "r");
	unveil("/etc/group", "r");
	unveil("/etc/resolv.conf", "r");
	unveil(NULL, NULL);

	/* Pledge con tutti i permessi necessari per i child */
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
