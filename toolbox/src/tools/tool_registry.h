#ifndef TOOLBOX_TOOL_REGISTRY_H
#define TOOLBOX_TOOL_REGISTRY_H

typedef struct ToolDescriptor {
    const char *id;
    const char *name;
    void (*activate)(void);
} ToolDescriptor;

void tool_registry_init(void);
void tool_registry_shutdown(void);

int tool_registry_register(const ToolDescriptor *tool);
const ToolDescriptor *tool_registry_find(const char *id);
int tool_registry_count(void);
const ToolDescriptor *tool_registry_get(int index);

#endif /* TOOLBOX_TOOL_REGISTRY_H */
