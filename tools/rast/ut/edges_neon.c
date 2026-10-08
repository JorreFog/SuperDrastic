/* edges_neon.c: spec/edges.c's routines in their NEON form (edges_impl.h with EDGES_NEON, as the hi-res pipeline
 * uses them) under the spec_ names, so t_edges.c tests them against DraStic's originals:
 *   tools/rast/ut/run.sh tools/rast/ut/t_edges.c tools/rast/ut/edges_neon.c */
#include <arm_neon.h>
#include <string.h>
#include "spec/edges.h"
#define EDGES_FN(name) spec_##name
#define EDGES_ARR  SPAN_ARR
#define EDGES_XMAX 0x200
#define EDGES_NEON 1
#include "spec/edges_impl.h"
