#include "tool_registry.h"

#include <string.h>

#define TOOL_REGISTRY_MAX_TOOLS 128

static const ToolDescriptor *g_tools[TOOL_REGISTRY_MAX_TOOLS];
static int g_tool_count = 0;

void tool_registry_init(void) {
    g_tool_count = 0;
}

void tool_registry_shutdown(void) {
    g_tool_count = 0;
}

int tool_registry_register(const ToolDescriptor *tool) {
    if (g_tool_count >= TOOL_REGISTRY_MAX_TOOLS) {
        return -1;
    }
    g_tools[g_tool_count++] = tool;
    return 0;
}

const ToolDescriptor *tool_registry_find(const char *id) {
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i]->id, id) == 0) {
            return g_tools[i];
        }
    }
    return NULL;
}

int tool_registry_count(void) {
    return g_tool_count;
}

const ToolDescriptor *tool_registry_get(int index) {
    if (index < 0 || index >= g_tool_count) {
        return NULL;
    }
    return g_tools[index];
}
