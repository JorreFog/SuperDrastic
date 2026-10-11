/* dsgeom.h: DraStic's geometry stage, replaced where it pays (dsgeom.c). */
#ifndef DSGEOM_H
#define DSGEOM_H
#include <stdint.h>

void geom_init(void);                   /* rast_init, renderer hooked: installs per RAST_GEOM / RAST_GEOMCHECK */
typedef void geom_queue_fn(uint8_t *geom, uint32_t cmd, uint32_t param);
extern geom_queue_fn *geom_queue_orig;  /* DraStic's queue_geometry_command (the trampoline once installed) */
void geom_tables(uintptr_t ds_base);     /* copies DraStic's tables (geom_init does; the unit tests call it) */
/* queue_geometry_command's fast paths: 1 when done (the original's stores), 0 (nothing written) for the original */
int geom_queue_ours(uint8_t *geom, uint32_t cmd, uint32_t param);
void geom_queue_command(uint8_t *geom, uint32_t cmd, uint32_t param);          /* the replacement */
void geom_queue_command_check(uint8_t *geom, uint32_t cmd, uint32_t param);    /* RAST_GEOMCHECK=1 */
typedef void geom_vertex_fn(uint8_t *geom, int32_t x, int32_t y, int32_t z);
extern geom_vertex_fn *geom_vertex_orig;    /* DraStic's geometry_add_vertex */
int geom_vertex_ours(uint8_t *geom, int32_t x, int32_t y, int32_t z);       /* as geom_queue_ours */
void geom_add_vertex(uint8_t *geom, int32_t x, int32_t y, int32_t z);
void geom_add_vertex_check(uint8_t *geom, int32_t x, int32_t y, int32_t z);
typedef void geom_ysort_fn(uint8_t *geom, uint8_t *bins, uint8_t *polys, uint8_t *verts, uint32_t flags);
extern geom_ysort_fn *geom_ysort_orig;      /* DraStic's video_3d_bin_polygons_y_sort_4x */
void geom_ysort(uint8_t *geom, uint8_t *bins, uint8_t *polys, uint8_t *verts, uint32_t flags);
void geom_ysort_check(uint8_t *geom, uint8_t *bins, uint8_t *polys, uint8_t *verts, uint32_t flags);
typedef void geom_tvb_fn(uint8_t *geom);
extern geom_tvb_fn *geom_tvb_orig;          /* DraStic's geometry_transform_vertex_block_asm */
void geom_tvb(uint8_t *geom);               /* geom_tvb.S: the same, scheduled for the A55 */
void geom_tvb_check(uint8_t *geom);
#endif
