/* config.h - Global configuration */
#ifndef CONFIG_H
#define CONFIG_H

/**
 * @brief Global verbose logging flag configured during process startup.
 *
 * Defined in app_main.c and consumed by modules that need process-wide
 * logging verbosity state.
 */
extern int config_verbose;

/**
 * @brief Runtime-selected static asset directory path.
 */
extern char config_static_dir[];

/**
 * @brief Runtime-selected templates directory path.
 */
extern char config_templates_dir[];

#endif /* CONFIG_H */
