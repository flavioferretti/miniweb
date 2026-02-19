/* config.h - Global configuration */
#ifndef CONFIG_H
#define CONFIG_H

/* Global verbose flag - defined in main.c */
extern int config_verbose;

/* Runtime-selected asset directories (set from config at startup). */
extern char config_static_dir[];
extern char config_templates_dir[];

#endif /* CONFIG_H */
