#ifndef WORKBENCH_TOOL_REGISTRY_H
#define WORKBENCH_TOOL_REGISTRY_H

typedef struct ToolDescriptor {
    const char *id;   /* stable machine-readable key */
    const char *name; /* user-facing label */
    void (*activate)(void);
} ToolDescriptor;

/* Process-global registry for built-in tools. It borrows descriptors and
 * their strings, so registrations must remain alive until shutdown. */
void tool_registry_init(void);
void tool_registry_shutdown(void);

/* Returns -1 when the fixed registry capacity is exhausted. */
int tool_registry_register(const ToolDescriptor *tool);
/* Lookup/accessor results are borrowed; NULL means not found/out of range. */
const ToolDescriptor *tool_registry_find(const char *id);
int tool_registry_count(void);
const ToolDescriptor *tool_registry_get(int index);

#endif /* WORKBENCH_TOOL_REGISTRY_H */
