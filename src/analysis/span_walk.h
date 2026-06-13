/**
 * span_walk.h - Shared span-tree traversal for the analysis layer
 *
 * Several analysis passes walk the reconstructed span tree in the same
 * preorder shape (visit node, then recurse children via first_child /
 * next_sibling). This helper centralizes that traversal so the passes
 * only supply what to do at each node.
 */

#ifndef DAPPER_ANALYSIS_SPAN_WALK_H
#define DAPPER_ANALYSIS_SPAN_WALK_H

#include "dapper/types.h"

typedef void (*span_visit_fn)(const span_t *span, void *ctx);

/** Visit every span in the subtree rooted at `root` in preorder. */
static inline void span_tree_walk_preorder(const span_t *root, span_visit_fn fn,
                                           void *ctx) {
  if (!root) {
    return;
  }
  fn(root, ctx);
  for (const span_t *child = root->first_child; child;
       child = child->next_sibling) {
    span_tree_walk_preorder(child, fn, ctx);
  }
}

#endif /* DAPPER_ANALYSIS_SPAN_WALK_H */
