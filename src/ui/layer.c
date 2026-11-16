#include "layer.h"
#include "app.h"
#include <stdio.h>

#define MAX_LAYERS 16
static layer *stack[MAX_LAYERS];
static int depth;

void layer_push(layer *l) {
    if (!l) return;
    for (int i = 0; i < depth; i++) if (stack[i] == l) { layer_pop(l); break; }
    if (depth >= MAX_LAYERS) { fprintf(stderr, "layers=overflow\n"); return; }
    stack[depth++] = l;
    app_dirty();
}
void layer_pop(layer *l) {
    for (int i = depth - 1; i >= 0; i--) {
        if (stack[i] != l) continue;
        while (depth > i) {
            layer *t = stack[--depth];
            if (t->closed) t->closed(t);
        }
        break;
    }
    app_dirty();
}
void layer_pop_all(void) {
    while (depth) {
        layer *t = stack[--depth];
        if (t->closed) t->closed(t);
    }
    app_dirty();
}
layer *layer_top(void) { return depth ? stack[depth - 1] : NULL; }
int layer_count(void) { return depth; }
layer *layer_at(int i) { return i >= 0 && i < depth ? stack[i] : NULL; }
int layer_has(const layer *l) {
    for (int i = 0; i < depth; i++) if (stack[i] == l) return 1;
    return 0;
}
int layer_dispatch(int key, int ev) {
    layer *t = layer_top();
    if (!t || !t->key) return 0;
    int r = t->key(t, key, ev);
    app_dirty();
    return r;
}
int layer_top_has_long(int key) {
    layer *t = layer_top();
    return t && t->has_long && t->has_long(t, key);
}
void layer_draw_all(gfx_t *g) {
    int from = 0;
    for (int i = depth - 1; i >= 0; i--) if (stack[i]->covers) { from = i; break; }
    for (int i = from; i < depth; i++) if (stack[i]->draw) stack[i]->draw(stack[i], g);
}
void layer_tick_all(double now) {
    for (int i = 0; i < depth; i++) if (stack[i]->tick) stack[i]->tick(stack[i], now);
}
