/* Copyright (C) 2001-2022 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/


/* Standard memory allocator */
#include "gx.h"
#include "memory_.h"
#include "gserrors.h"
#include "gsexit.h"
#include "gsmdebug.h"
#include "gsstruct.h"
#include "gxalloc.h"
#include "stream.h"		/* for clearing stream list */
#include "malloc_.h" /* For MEMENTO */

/*
 * Define whether to try consolidating space before adding a new clump.
 * The default is not to do this, because it is computationally
 * expensive and doesn't seem to help much.  However, this is done for
 * "controlled" spaces whether or not the #define is in effect.
 */
/*#define CONSOLIDATE_BEFORE_ADDING_CLUMP */

/*
 * This allocator produces tracing messages of the form
 *      [aNMOTS]...
 * where
 *   N is the VM space number, +1 if we are allocating from stable memory.
 *   M is : for movable objects, | for immovable,
 *   O is {alloc = +, free = -, grow = >, shrink = <},
 *   T is {bytes = b, object = <, ref = $, string = >}, and
 *   S is {small freelist = f, large freelist = F, LIFO = space,
 *      own clump = L, lost = #, lost own clump = ~, other = .}.
 */
#ifdef DEBUG
static int
alloc_trace_space(const gs_ref_memory_t *imem)
{
    return (int)(imem->space + (imem->stable_memory == (const gs_memory_t *)imem));
}
static void
alloc_trace(const char *chars, gs_ref_memory_t * imem, client_name_t cname,
            gs_memory_type_ptr_t stype, uint size, const void *ptr)
{
    if_debug7m('A', (const gs_memory_t *)imem, "[a%d%s]%s %s(%u) %s"PRI_INTPTR"\n",
               alloc_trace_space(imem), chars, client_name_string(cname),
               (ptr == 0 || stype == 0 ? "" :
                struct_type_name_string(stype)),
               size, (chars[1] == '+' ? "= " : ""), (intptr_t)ptr);
}
static bool
alloc_size_is_ok(gs_memory_type_ptr_t stype)
{
    return (stype->ssize > 0 && stype->ssize < 0x200000);
}
#  define ALLOC_CHECK_SIZE(mem,stype)\
    BEGIN\
      if (!alloc_size_is_ok(stype)) {\
        mlprintf2(mem,"size of struct type "PRI_INTPTR" is 0x%lx!\n",\
                  (intptr_t)(stype), (ulong)((stype)->ssize));\
        return 0;\
      }\
    END
#else
#  define alloc_trace(chars, imem, cname, stype, size, ptr) DO_NOTHING
#  define ALLOC_CHECK_SIZE(mem,stype) DO_NOTHING
#endif

/*
 * The structure descriptor for allocators.  Even though allocators
 * are allocated outside GC space, they reference objects within it.
 */
public_st_ref_memory();
static
ENUM_PTRS_BEGIN(ref_memory_enum_ptrs) return 0;
ENUM_PTR3(0, gs_ref_memory_t, streams, names_array, changes);
ENUM_PTR(3, gs_ref_memory_t, saved);
ENUM_PTR(4, gs_ref_memory_t, scan_limit);
ENUM_PTRS_END
static RELOC_PTRS_WITH(ref_memory_reloc_ptrs, gs_ref_memory_t *mptr)
{
    RELOC_PTR(gs_ref_memory_t, streams);
    RELOC_PTR(gs_ref_memory_t, names_array);
    RELOC_PTR(gs_ref_memory_t, changes);
    RELOC_PTR(gs_ref_memory_t, scan_limit);
    /* Don't relocate the saved pointer now -- see igc.c for details. */
    mptr->reloc_saved = RELOC_OBJ(mptr->saved);
}
RELOC_PTRS_END

/*
 * Define the flags for alloc_obj, which implements all but the fastest
 * case of allocation.
 */
typedef enum {
    ALLOC_IMMOVABLE = 1,
    ALLOC_DIRECT = 2		/* called directly, without fast-case checks */
} alloc_flags_t;

/* Forward references */
static void remove_range_from_freelist(gs_ref_memory_t *mem, void* bottom, void* top);
static obj_header_t *large_freelist_alloc(gs_ref_memory_t *mem, obj_size_t size);
static obj_header_t *scavenge_low_free(gs_ref_memory_t *mem, unsigned request_size);
static size_t compute_free_objects(gs_ref_memory_t *);
static obj_header_t *alloc_obj(gs_ref_memory_t *, obj_size_t, gs_memory_type_ptr_t, alloc_flags_t, client_name_t);
static void consolidate_clump_free(clump_t *cp, gs_ref_memory_t *mem);
static void trim_obj(gs_ref_memory_t *mem, obj_header_t *obj, obj_size_t size, clump_t *cp);
static clump_t *alloc_acquire_clump(gs_ref_memory_t *, size_t, bool, client_name_t);
static clump_t *alloc_add_clump(gs_ref_memory_t *, size_t, client_name_t);
void alloc_close_clump(gs_ref_memory_t *);

/*
 * Define the standard implementation (with garbage collection)
 * of Ghostscript's memory manager interface.
 */
/* Raw memory procedures */
static gs_memory_proc_alloc_bytes(i_alloc_bytes_immovable);
static gs_memory_proc_resize_object(i_resize_object);
static gs_memory_proc_free_object(i_free_object);
static gs_memory_proc_stable(i_stable);
static gs_memory_proc_status(i_status);
static gs_memory_proc_free_all(i_free_all);
static gs_memory_proc_consolidate_free(i_consolidate_free);

/* Object memory procedures */
static gs_memory_proc_alloc_bytes(i_alloc_bytes);
static gs_memory_proc_alloc_struct(i_alloc_struct);
static gs_memory_proc_alloc_struct(i_alloc_struct_immovable);
static gs_memory_proc_alloc_byte_array(i_alloc_byte_array);
static gs_memory_proc_alloc_byte_array(i_alloc_byte_array_immovable);
static gs_memory_proc_alloc_struct_array(i_alloc_struct_array);
static gs_memory_proc_alloc_struct_array(i_alloc_struct_array_immovable);
static gs_memory_proc_object_size(i_object_size);
static gs_memory_proc_object_type(i_object_type);
static gs_memory_proc_alloc_string(i_alloc_string);
static gs_memory_proc_alloc_string(i_alloc_string_immovable);
static gs_memory_proc_resize_string(i_resize_string);
static gs_memory_proc_free_string(i_free_string);
static gs_memory_proc_register_root(i_register_root);
static gs_memory_proc_unregister_root(i_unregister_root);
static gs_memory_proc_enable_free(i_enable_free);
static gs_memory_proc_set_object_type(i_set_object_type);
static gs_memory_proc_defer_frees(i_defer_frees);

/* We export the procedures for subclasses. */
const gs_memory_procs_t gs_ref_memory_procs =
{
    /* Raw memory procedures */
    i_alloc_bytes_immovable,
    i_resize_object,
    i_free_object,
    i_stable,
    i_status,
    i_free_all,
    i_consolidate_free,
    /* Object memory procedures */
    i_alloc_bytes,
    i_alloc_struct,
    i_alloc_struct_immovable,
    i_alloc_byte_array,
    i_alloc_byte_array_immovable,
    i_alloc_struct_array,
    i_alloc_struct_array_immovable,
    i_object_size,
    i_object_type,
    i_alloc_string,
    i_alloc_string_immovable,
    i_resize_string,
    i_free_string,
    i_register_root,
    i_unregister_root,
    i_enable_free,
    i_set_object_type,
    i_defer_frees
};

/*
 * Previous versions of this code used a simple linked list of
 * clumps. We change here to use a splay tree of clumps.
 * Splay Trees can be found documented in "Algorithms and Data
 * Structures" by Jeffrey H Kingston.
 *
 * Essentially they are binary trees, ordered by address of the
 * 'cbase' pointer. The 'cunning' feature with them is that
 * when a node in the tree is accessed, we do a 'move to root'
 * operation. This involves performing various 'rotations' as
 * we move up the tree, the net effect of which tends to
 * lead to more balanced trees (see Kingston for analysis).
 * It also leads to better locality of reference in that
 * recently accessed nodes stay near the root.
 */

/* #define DEBUG_CLUMPS */
#ifdef DEBUG_CLUMPS
#define SANITY_CHECK(cp) sanity_check(cp)
#define SANITY_CHECK_MID(cp) sanity_check_mid(cp)

static void
broken_splay()
{
    dlprintf("Broken splay tree!\n");
}

void sanity_check_rec(clump_t *cp)
{
    splay_dir_t from = SPLAY_FROM_ABOVE;

    while (cp)
    {
        if (from == SPLAY_FROM_ABOVE)
        {
            /* We have arrived from above. Step left. */
            if (cp->left)
            {
                if (cp->left->cbase > cp->cbase || cp->left->parent != cp)
                    broken_splay();
                cp = cp->left;
                from = SPLAY_FROM_ABOVE;
                continue;
            }
            /* No left to step to, so imagine we have just arrived from there */
            from = SPLAY_FROM_LEFT;
        }
        if (from == SPLAY_FROM_LEFT)
        {
            /* We have arrived from the left. Step right. */
            if (cp->right)
            {
                if (cp->right->cbase < cp->cbase || cp->right->parent != cp)
                    broken_splay();
                cp = cp->right;
                from = SPLAY_FROM_ABOVE;
                continue;
            }
            /* No right to step to, so imagine we have just arrived from there. */
            from = SPLAY_FROM_RIGHT;
        }
        if (from == SPLAY_FROM_RIGHT)
        {
            /* We have arrived from the right. Step up. */
            if (cp->parent == NULL)
                break;
            if (cp->parent->left != cp && cp->parent->right != cp)
                broken_splay();
            from = (cp->parent->left == cp ? SPLAY_FROM_LEFT : SPLAY_FROM_RIGHT);
            cp = cp->parent;
        }
    }
}

void sanity_check(clump_t *cp)
{
    sanity_check_rec(cp);
}

void sanity_check_mid(clump_t *cp)
{
    clump_t *parent;

    while ((parent = cp->parent) != NULL)
    {
        if (parent->left == cp)
        {
            if (parent->right == cp)
                broken_splay();
        }
        else if (parent->right != cp)
            broken_splay();
        cp = parent;
    }

    sanity_check_rec(cp);
}

#else
#define SANITY_CHECK(cp) while (0) {}
#define SANITY_CHECK_MID(cp) while (0) {}
#endif

/* When initing with the root, we want to pass the smallest inorder one
 * back immediately, and set it up so that we step right for the next
 * one. */
clump_t *
clump_splay_walk_init(clump_splay_walker *sw, const gs_ref_memory_t *mem)
{
    clump_t *cp = mem->root;

    if (cp)
    {
        SANITY_CHECK(cp);

        sw->from = SPLAY_FROM_LEFT;
        while (cp->left)
        {
            cp = cp->left;
        }
    }
    sw->cp = cp;
    sw->end = NULL;
    return cp;
}

clump_t *
clump_splay_walk_bwd_init(clump_splay_walker *sw, const gs_ref_memory_t *mem)
{
    clump_t *cp = mem->root;

    if (cp)
    {
        SANITY_CHECK(cp);

        sw->from = SPLAY_FROM_RIGHT;
        while (cp->right)
        {
            cp = cp->right;
        }
    }
    sw->cp = cp;
    sw->end = NULL;
    return cp;
}

/* When initing 'mid walk' (i.e. with a non-root node), we want to
 * return the node we are given as the first one, and continue
 * onwards in an in order fashion.
 */
clump_t *
clump_splay_walk_init_mid(clump_splay_walker *sw, clump_t *cp)
{
    sw->from = SPLAY_FROM_LEFT;
    sw->cp = cp;
    sw->end = cp;
    if (cp)
    {
        SANITY_CHECK_MID(cp);
    }
    return cp;
}

clump_t *
clump_splay_walk_fwd(clump_splay_walker *sw)
{
    clump_t *cp = sw->cp;
    int from = sw->from;

    if (cp == NULL)
        return NULL;

    /* We step through the tree, and stop when we arrive
     * at sw->end in an in order manner (i.e. by moving from
     * the left). */
    while (1)
    {
        if (from == SPLAY_FROM_ABOVE)
        {
            /* We have arrived from above. Step left. */
            if (cp->left)
            {
                cp = cp->left;
                from = SPLAY_FROM_ABOVE;
                continue;
            }
            /* No left to step to, so imagine we have just arrived from there */
            from = SPLAY_FROM_LEFT;
            /* Have we reached the stopping point? */
            if (cp == sw->end)
                cp = NULL;
            /* We want to stop here, for inorder operation. So break out of the loop. */
            break;
        }
        if (from == SPLAY_FROM_LEFT)
        {
            /* We have arrived from the left. Step right. */
            if (cp->right)
            {
                cp = cp->right;
                from = SPLAY_FROM_ABOVE;
                continue;
            }
            /* No right to step to, so imagine we have just arrived from there. */
            from = SPLAY_FROM_RIGHT;
        }
        if (from == SPLAY_FROM_RIGHT)
        {
            /* We have arrived from the right. Step up. */
            clump_t *old = cp;
            cp = cp->parent;
            if (cp == NULL)
            {
                /* We've reached the root of the tree. Is this our stopping point? */
                if (sw->end == NULL)
                    break;
                /* If not, step on. */
                cp = old;
                from = SPLAY_FROM_ABOVE;
            }
            else
            {
                from = (cp->left == old ? SPLAY_FROM_LEFT : SPLAY_FROM_RIGHT);
                if (from == SPLAY_FROM_LEFT)
                {
                    /* Have we reached the stopping point? */
                    if (cp == sw->end)
                        cp = NULL;
                    break;
                }
            }
        }
    }
    sw->cp = cp;
    sw->from = from;
    return cp;
}

clump_t *
clump_splay_walk_bwd(clump_splay_walker *sw)
{
    clump_t *cp = sw->cp;
    int from = sw->from;

    if (cp == NULL)
        return NULL;

    /* We step backwards through the tree, and stop when we arrive
     * at sw->end in a reverse in order manner (i.e. by moving from
     * the right). */
    while (1)
    {
        if (from == SPLAY_FROM_ABOVE)
        {
            /* We have arrived from above. Step right. */
            if (cp->right)
            {
                cp = cp->right;
                from = SPLAY_FROM_ABOVE;
                continue;
            }
            /* No right to step to, so imagine we have just arrived from there. */
            from = SPLAY_FROM_RIGHT;
            /* Have we reached our end? */
            if (cp == sw->end)
                cp = NULL;
            /* Stop to run inorder operation */
            break;
        }
        if (from == SPLAY_FROM_RIGHT)
        {
            /* We have arrived from the right. Step left. */
            if (cp->left)
            {
                cp = cp->left;
                from = SPLAY_FROM_ABOVE;
                continue;
            }
            /* No left to step to, so imagine we have just arrived from there. */
            from = SPLAY_FROM_LEFT;
        }
        if (from == SPLAY_FROM_LEFT)
        {
            /* We have arrived from the left. Step up. */
            clump_t *old = cp;
            cp = cp->parent;
            from = (cp == NULL || cp->left != old ? SPLAY_FROM_RIGHT : SPLAY_FROM_LEFT);
            if (from == SPLAY_FROM_RIGHT)
            {
                if (cp == sw->end)
                    cp = NULL;
                break;
            }
        }
    }
    sw->cp = cp;
    sw->from = from;
    return cp;
}

static clump_t *
clump_splay_remove(clump_t *cp, gs_ref_memory_t *imem)
{
    clump_t *replacement;

    if (cp->left == NULL)
    {
        /* At most one child - easy */
        replacement = cp->right;
    }
    else if (cp->right == NULL)
    {
        /* Strictly one child - easy */
        replacement = cp->left;
    }
    else
    {
        /* 2 Children - tricky */
        /* Find in-order predecessor to f */
        replacement = cp->left;
        while (replacement->right)
            replacement = replacement->right;
        /* Remove replacement - easy as just one child */
        (void)clump_splay_remove(replacement, imem);
        /* Replace cp with replacement */
        if (cp->left)
            cp->left->parent = replacement;
        cp->right->parent = replacement;
        replacement->left = cp->left;
        replacement->right = cp->right;
    }
    if (cp->parent)
    {
        if (cp->parent->left == cp)
            cp->parent->left = replacement;
        else
            cp->parent->right = replacement;
    }
    else
        imem->root = replacement;
    if (replacement)
        replacement->parent = cp->parent;
    return replacement;
}

/* Here we apply a function to all the nodes in a tree in
 * depth first order. This means that the given function
 * can safely alter: 1) the clump, 2) it's children,
 * 3) it's parents child pointer that points to it
 * without fear of corruption. Specifically this means
 * that the function can free (and unlink) the node
 * if it wants.
 */
clump_t *
clump_splay_app(clump_t *root, gs_ref_memory_t *imem, splay_app_result_t (*fn)(clump_t *, void *), void *arg)
{
    clump_t *step_to;
    clump_t *cp = root;
    int from = SPLAY_FROM_ABOVE;
    splay_app_result_t res;

    SANITY_CHECK(cp);

    while (cp)
    {
        if (from == SPLAY_FROM_ABOVE)
        {
            /* We have arrived from above. Step left. */
            step_to = cp->left;
            if (step_to)
            {
                from = SPLAY_FROM_ABOVE;
                cp = step_to;
            }
            else
            {
                /* No left to step to, so imagine we have just arrived from the left */
                from = SPLAY_FROM_LEFT;
            }
        }
        if (from == SPLAY_FROM_LEFT)
        {
            /* We have arrived from the left. Step right. */
            step_to = cp->right;
            if (step_to)
            {
                from = SPLAY_FROM_ABOVE;
                cp = step_to;
            }
            else
            {
                /* No right to step to, so imagine we have just arrived from the right. */
                from = SPLAY_FROM_RIGHT;
            }
        }
        if (from == SPLAY_FROM_RIGHT)
        {
            /* We have arrived from the right. Step up. */
            step_to = cp->parent;
            if (step_to)
            {
                from = (step_to->left == cp ? SPLAY_FROM_LEFT : SPLAY_FROM_RIGHT);
            }
            res = fn(cp, arg);
            if (res & SPLAY_APP_STOP)
                return cp;
            cp = step_to;
        }
    }
    return cp;
}

/* Move the given node to the root of the tree, by
 * performing a series of the following rotations.
 * The key observation here is that all these
 * rotations preserve the ordering of the tree, and
 * result in 'x' getting higher.
 *
 * Case 1:   z          x           Case 1b:   z                   x
 *          # #        # #                    # #                 # #
 *         y   D      A   y                  A   y               y   D
 *        # #     =>     # #                    # #     =>      # #
 *       x   C          B   z                  B   x           z   C
 *      # #                # #                    # #         # #
 *     A   B              C   D                  C   D       A   B
 *
 * Case 2:   z             x        Case 2b:   z                  x
 *          # #          ## ##                # #               ## ##
 *         y   D        y     z              A   y             z     y
 *        # #     =>   # #   # #                # #     =>    # #   # #
 *       A   x        A   B C   D              x   D         A   B C   D
 *          # #                               # #
 *         B   C                             B   C
 *
 * Case 3:   y          x           Case 3b:  y                  x
 *          # #        # #                   # #                # #
 *         x   C  =>  A   y                 A   x       =>     y   C
 *        # #            # #                   # #            # #
 *       A   B          B   C                 B   C          A   B
 */
static void
splay_move_to_root(clump_t *x, gs_ref_memory_t *mem)
{
    clump_t *y, *z;

    if (x == NULL)
        return;

    while ((y = x->parent) != NULL) {
        if ((z = y->parent) != NULL) {
            x->parent = z->parent;
            if (x->parent) {
                if (x->parent->left == z)
                    x->parent->left = x;
                else
                    x->parent->right  = x;
            }
            y->parent = x;
            /* Case 1, 1b, 2 or 2b */
            if (y->left == x) {
                /* Case 1 or 2b */
                if (z->left == y) {
                    /* Case 1 */
                    y->left = x->right;
                    if (y->left)
                        y->left->parent = y;
                    z->left = y->right;
                    if (z->left)
                        z->left->parent = z;
                    y->right = z;
                    z->parent = y;
                } else {
                    /* Case 2b */
                    z->right = x->left;
                    if (z->right)
                        z->right->parent = z;
                    y->left = x->right;
                    if (y->left)
                        y->left->parent = y;
                    x->left = z;
                    z->parent = x;
                }
                x->right      = y;
            } else {
                /* Case 2 or 1b */
                if (z->left == y) {
                    /* Case 2 */
                    y->right = x->left;
                    if (y->right)
                        y->right->parent = y;
                    z->left = x->right;
                    if (z->left)
                        z->left->parent = z;
                    x->right = z;
                    z->parent = x;
                } else {
                    /* Case 1b */
                    z->right = y->left;
                    if (z->right)
                        z->right->parent = z;
                    y->right = x->left;
                    if (y->right)
                        y->right->parent = y;
                    y->left = z;
                    z->parent = y;
                }
                x->left = y;
            }
        } else {
            /* Case 3 or 3b */
            x->parent = NULL;
            y->parent = x;
            if (y->left == x) {
                /* Case 3 */
                y->left = x->right;
                if (y->left)
                    y->left->parent = y;
                x->right = y;
            } else {
                /* Case 3b */
                y->right = x->left;
                if (y->right)
                    y->right->parent = y;
                x->left = y;
            }
        }
    }
    mem->root = x;
}

static void
splay_insert(clump_t *cp, gs_ref_memory_t *mem)
{
    clump_t *node = NULL;
    clump_t **root = &mem->root;

    while (*root) {
        node = *root;
        if (PTR_LT(cp->cbase, node->cbase)) {
            root = &node->left;
        } else {
            root = &node->right;
        }
    }
    *root = cp;
    cp->left = NULL;
    cp->right = NULL;
    cp->parent = node;
    splay_move_to_root(cp, mem);
}

/*
 * Allocate and mostly initialize the state of an allocator (system, global,
 * or local).  Does not initialize global or space.
 */
static void *ialloc_solo(gs_memory_t *, gs_memory_type_ptr_t,
                          clump_t **);
gs_ref_memory_t *
ialloc_alloc_state(gs_memory_t * parent, uint clump_size)
{
    clump_t *cp;
    gs_ref_memory_t *iimem = ialloc_solo(parent, &st_ref_memory, &cp);

    if (iimem == 0)
        return 0;
    iimem->stable_memory = (gs_memory_t *)iimem;
    iimem->procs = gs_ref_memory_procs;
    iimem->gs_lib_ctx = parent->gs_lib_ctx;
    iimem->non_gc_memory = parent;
    iimem->thread_safe_memory = parent->thread_safe_memory;
    iimem->clump_size = clump_size;
#if defined(MEMENTO) || defined(SINGLE_OBJECT_MEMORY_BLOCKS_ONLY)
    iimem->large_size = 1;
#else
    iimem->large_size = ((clump_size / 4) & -obj_align_mod) + 1;
#endif
    iimem->is_controlled = false;
    iimem->gc_status.vm_threshold = clump_size * 3L;
    iimem->gc_status.max_vm = MAX_MAX_VM;
    iimem->gc_status.signal_value = 0;
    iimem->gc_status.enabled = false;
    iimem->gc_status.requested = 0;
    iimem->gc_allocated = 0;
    iimem->previous_status.allocated = 0;
    iimem->previous_status.used = 0;
    ialloc_reset(iimem);
    iimem->root = cp;
    ialloc_set_limit(iimem);
    iimem->cc = NULL;
    iimem->save_level = 0;
    iimem->new_mask = 0;
    iimem->test_mask = ~0;
    iimem->streams = 0;
    iimem->names_array = 0;
    iimem->roots = 0;
    iimem->num_contexts = 0;
    iimem->saved = 0;
    return iimem;
}

/* Allocate a 'solo' object with its own clump. */
static void *
ialloc_solo(gs_memory_t * parent, gs_memory_type_ptr_t pstype,
            clump_t ** pcp)
{	/*
         * We can't assume that the parent uses the same object header
         * that we do, but the GC requires that allocators have
         * such a header.  Therefore, we prepend one explicitly.
         */
    clump_t *cp =
        gs_raw_alloc_struct_immovable(parent, &st_clump,
                                      "ialloc_solo(clump)");
    uint csize =
        ROUND_UP(sizeof(clump_head_t) + sizeof(obj_header_t) +
                 pstype->ssize,
                 obj_align_mod);
    byte *cdata = gs_alloc_bytes_immovable(parent, csize, "ialloc_solo");
    obj_header_t *obj = (obj_header_t *) (cdata + sizeof(clump_head_t));

    if (cp == 0 || cdata == 0) {
        gs_free_object(parent, cp, "ialloc_solo(allocation failure)");
        gs_free_object(parent, cdata, "ialloc_solo(allocation failure)");
        return 0;
    }
    alloc_init_clump(cp, cdata, cdata + csize, false, (clump_t *) NULL);
    cp->cbot = cp->ctop;
    cp->parent = cp->left = cp->right = 0;
    cp->c_alone = true;
    /* Construct the object header "by hand". */
    obj->o_pad = 0;
    obj->o_alone = 1;
    obj->o_size = pstype->ssize;
    obj->o_type = pstype;
    *pcp = cp;
    return (void *)(obj + 1);
}

void
ialloc_free_state(gs_ref_memory_t *iimem)
{
    clump_t *cp;
    gs_memory_t *mem;
    if (iimem == NULL)
        return;
    cp = iimem->root;
    mem = iimem->non_gc_memory;
    if (cp == NULL)
        return;
    gs_free_object(mem, cp->chead, "ialloc_solo(allocation failure)");
    gs_free_object(mem, cp, "ialloc_solo(allocation failure)");
}

/*
 * Add a clump to an externally controlled allocator.  Such allocators
 * allocate all objects as immovable, are not garbage-collected, and
 * don't attempt to acquire additional memory on their own.
 */
int
ialloc_add_clump(gs_ref_memory_t *imem, ulong space, client_name_t cname)
{
    clump_t *cp;

    /* Allow acquisition of this clump. */
    imem->is_controlled = false;
    imem->large_size = imem->clump_size;
    imem->limit = imem->gc_status.max_vm = MAX_MAX_VM;

    /* Acquire the clump. */
    cp = alloc_add_clump(imem, space, cname);

    /*
     * Make all allocations immovable.  Since the "movable" allocators
     * allocate within existing clumps, whereas the "immovable" ones
     * allocate in new clumps, we equate the latter to the former, even
     * though this seems backwards.
     */
    imem->procs.alloc_bytes_immovable = imem->procs.alloc_bytes;
    imem->procs.alloc_struct_immovable = imem->procs.alloc_struct;
    imem->procs.alloc_byte_array_immovable = imem->procs.alloc_byte_array;
    imem->procs.alloc_struct_array_immovable = imem->procs.alloc_struct_array;
    imem->procs.alloc_string_immovable = imem->procs.alloc_string;

    /* Disable acquisition of additional clumps. */
    imem->is_controlled = true;
    imem->limit = 0;

    return (cp ? 0 : gs_note_error(gs_error_VMerror));
}

/* Prepare for a GC by clearing the stream list. */
/* This probably belongs somewhere else.... */
void
ialloc_gc_prepare(gs_ref_memory_t * mem)
{	/*
         * We have to unlink every stream from its neighbors,
         * so that referenced streams don't keep all streams around.
         */
    while (mem->streams != 0) {
        stream *s = mem->streams;

        mem->streams = s->next;
        s->prev = s->next = 0;
    }
}

/* Initialize after a save. */
void
ialloc_reset(gs_ref_memory_t * mem)
{
    mem->root = 0;
    mem->cc = NULL;
    mem->allocated = 0;
    mem->changes = 0;
    mem->scan_limit = 0;
    mem->total_scanned = 0;
    mem->total_scanned_after_compacting = 0;
    ialloc_reset_free(mem);
}

/* Initialize after a save or GC. */
void
ialloc_reset_free(gs_ref_memory_t * mem)
{
    int i;
    obj_header_t **p;

    mem->lost.objects = 0;
    mem->lost.refs = 0;
    mem->lost.strings = 0;
    mem->cfreed.cp = 0;
    for (i = 0, p = &mem->freelists[0]; i < num_freelists; i++, p++)
        *p = 0;
    mem->largest_free_size = 0;
}

/*
 * Set an arbitrary limit so that the amount of allocated VM does not grow
 * indefinitely even when GC is disabled.  Benchmarks have shown that
 * the resulting GC's are infrequent enough not to degrade performance
 * significantly.
 */
#define FORCE_GC_LIMIT 8000000

/* Set the allocation limit after a change in one or more of */
/* vm_threshold, max_vm, or enabled, or after a GC. */
void
ialloc_set_limit(register gs_ref_memory_t * mem)
{	/*
         * The following code is intended to set the limit so that
         * we stop allocating when allocated + previous_status.allocated
         * exceeds the lesser of max_vm or (if GC is enabled)
         * gc_allocated + vm_threshold.
         */
    size_t max_allocated =
    (mem->gc_status.max_vm > mem->previous_status.allocated ?
     mem->gc_status.max_vm - mem->previous_status.allocated :
     0);

    if (mem->gc_status.enabled) {
        size_t limit = mem->gc_allocated + mem->gc_status.vm_threshold;

        if (limit < mem->previous_status.allocated)
            mem->limit = 0;
        else {
            limit -= mem->previous_status.allocated;
            mem->limit = min(limit, max_allocated);
        }
    } else
        mem->limit = min(max_allocated, mem->gc_allocated + FORCE_GC_LIMIT);
    if_debug7m('0', (const gs_memory_t *)mem,
               "[0]space=%d, max_vm=%"PRIdSIZE", prev.alloc=%"PRIdSIZE", enabled=%d, "
               "gc_alloc=%"PRIdSIZE", threshold=%"PRIdSIZE" => limit=%"PRIdSIZE"\n",
               mem->space, mem->gc_status.max_vm,
               mem->previous_status.allocated,
               mem->gc_status.enabled, mem->gc_allocated,
               mem->gc_status.vm_threshold, mem->limit);
}

struct free_data
{
    gs_ref_memory_t *imem;
    clump_t         *allocator;
};

static splay_app_result_t
free_all_not_allocator(clump_t *cp, void *arg)
{
    struct free_data *fd = (struct free_data *)arg;

    if (cp->cbase + sizeof(obj_header_t) != (byte *)fd->imem)
        alloc_free_clump(cp, fd->imem);
    else
        fd->allocator = cp;

    return SPLAY_APP_CONTINUE;
}

static splay_app_result_t
free_all_allocator(clump_t *cp, void *arg)
{
    struct free_data *fd = (struct free_data *)arg;

    if (cp->cbase + sizeof(obj_header_t) != (byte *)fd->imem)
        return SPLAY_APP_CONTINUE;

    fd->allocator = cp;
    alloc_free_clump(cp, fd->imem);
    return SPLAY_APP_STOP;
}

/*
 * Free all the memory owned by the allocator, except the allocator itself.
 * Note that this only frees memory at the current save level: the client
 * is responsible for restoring to the outermost level if desired.
 */
static void
i_free_all(gs_memory_t * mem, uint free_mask, client_name_t cname)
{
    gs_ref_memory_t * imem = (gs_ref_memory_t *)mem;
    struct free_data fd;

    fd.imem = imem;
    fd.allocator = NULL;

    if (free_mask & FREE_ALL_DATA && imem->root != NULL) {
        /* Free every clump except the allocator */
        clump_splay_app(imem->root, imem, free_all_not_allocator, &fd);

        /* Reinstate the allocator as the sole clump */
        imem->root = fd.allocator;
        if (fd.allocator)
            fd.allocator->parent = fd.allocator->left = fd.allocator->right = NULL;
    }
    if (free_mask & FREE_ALL_ALLOCATOR) {
        /* Walk the tree to find the allocator. */
        clump_splay_app(imem->root, imem, free_all_allocator, &fd);
    }
}

/* ================ Accessors ================ */

/* Get the size of an object from the header. */
static size_t
i_object_size(gs_memory_t * mem, const void /*obj_header_t */ *obj)
{
    return pre_obj_contents_size((const obj_header_t *)obj - 1);
}

/* Get the type of a structure from the header. */
static gs_memory_type_ptr_t
i_object_type(const gs_memory_t * mem, const void /*obj_header_t */ *obj)
{
    return ((const obj_header_t *)obj - 1)->o_type;
}

/* Get the GC status of a memory. */
void
gs_memory_gc_status(const gs_ref_memory_t * mem, gs_memory_gc_status_t * pstat)
{
    *pstat = mem->gc_status;
}

/* Set the GC status of a memory. */
void
gs_memory_set_gc_status(gs_ref_memory_t * mem, const gs_memory_gc_status_t * pstat)
{
    mem->gc_status = *pstat;
    ialloc_set_limit(mem);
}

/* Set VM threshold. Value passed as int64_t since it is signed */
void
gs_memory_set_vm_threshold(gs_ref_memory_t * mem, int64_t val)
{
    gs_memory_gc_status_t stat;
    gs_ref_memory_t * stable = (gs_ref_memory_t *)mem->stable_memory;

    if (val < MIN_VM_THRESHOLD)
        val = MIN_VM_THRESHOLD;
    else if (val > MAX_VM_THRESHOLD)
        val = MAX_VM_THRESHOLD;
    gs_memory_gc_status(mem, &stat);
    stat.vm_threshold = val;
    gs_memory_set_gc_status(mem, &stat);
    gs_memory_gc_status(stable, &stat);
    stat.vm_threshold = val;
    gs_memory_set_gc_status(stable, &stat);
}

/* Set VM reclaim. */
void
gs_memory_set_vm_reclaim(gs_ref_memory_t * mem, bool enabled)
{
    gs_memory_gc_status_t stat;
    gs_ref_memory_t * stable = (gs_ref_memory_t *)mem->stable_memory;

    gs_memory_gc_status(mem, &stat);
    stat.enabled = enabled;
    gs_memory_set_gc_status(mem, &stat);
    gs_memory_gc_status(stable, &stat);
    stat.enabled = enabled;
    gs_memory_set_gc_status(stable, &stat);
}

/* ================ Objects ================ */

/* Allocate a small object quickly if possible. */
/* The size must be substantially less than max_uint. */
/* ptr must be declared as obj_header_t *. */
/* pfl must be declared as obj_header_t **. */
#define IF_FREELIST_ALLOC(ptr, imem, size, pstype, pfl)\
        if ( size <= max_freelist_size &&\
             *(pfl = &imem->freelists[(size + obj_align_mask) >> log2_obj_align_mod]) != 0\
           )\
        {	ptr = *pfl;\
                *pfl = *(obj_header_t **)ptr;\
                ptr[-1].o_size = (obj_size_t)size;\
                ptr[-1].o_type = pstype;\
                /* If debugging, clear the block in an attempt to */\
                /* track down uninitialized data errors. */\
                gs_alloc_fill(ptr, gs_alloc_fill_alloc, size);
#define ELSEIF_BIG_FREELIST_ALLOC(ptr, imem, size, pstype)\
        }\
        else if (size > max_freelist_size &&\
                 (ptr = large_freelist_alloc(imem, size)) != 0)\
        {	ptr[-1].o_type = pstype;\
                /* If debugging, clear the block in an attempt to */\
                /* track down uninitialized data errors. */\
                gs_alloc_fill(ptr, gs_alloc_fill_alloc, size);
#define ELSEIF_LIFO_ALLOC(ptr, imem, size, pstype)\
        }\
        else if ( imem->cc && !imem->cc->c_alone && \
                (imem->cc->ctop - (byte *)(ptr = (obj_header_t *)imem->cc->cbot))\
                >= size + (obj_align_mod + sizeof(obj_header_t) * 2) &&\
             size < imem->large_size\
           )\
        {	imem->cc->cbot = (byte *)ptr + obj_size_round(size);\
                ptr->o_pad = 0;\
                ptr->o_alone = 0;\
                ptr->o_size = (obj_size_t)size;\
                ptr->o_type = pstype;\
                ptr++;\
                /* If debugging, clear the block in an attempt to */\
                /* track down uninitialized data errors. */\
                gs_alloc_fill(ptr, gs_alloc_fill_alloc, size);
#define ELSE_ALLOC\
        }\
        else

static byte *
i_alloc_bytes(gs_memory_t * mem, size_t ssize, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *obj;
    obj_header_t **pfl;
    obj_size_t size = (obj_size_t)ssize;

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    if ((size_t)size != ssize)
        return NULL;

    IF_FREELIST_ALLOC(obj, imem, size, &st_bytes, pfl)
        alloc_trace(":+bf", imem, cname, NULL, size, obj);
    ELSEIF_BIG_FREELIST_ALLOC(obj, imem, size, &st_bytes)
        alloc_trace(":+bF", imem, cname, NULL, size, obj);
    ELSEIF_LIFO_ALLOC(obj, imem, (uint)size, &st_bytes)
        alloc_trace(":+b ", imem, cname, NULL, size, obj);
    ELSE_ALLOC
    {
        obj = alloc_obj(imem, size, &st_bytes, 0, cname);
        if (obj == 0)
            return 0;
        alloc_trace(":+b.", imem, cname, NULL, size, obj);
    }
#if IGC_PTR_STABILITY_CHECK
        obj[-1].d.o.space_id = imem->space_id;
#endif
    return (byte *)Memento_label(obj, cname);
}
static byte *
i_alloc_bytes_immovable(gs_memory_t * mem, size_t ssize, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *obj;
    obj_size_t size = (obj_size_t)ssize;

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    if ((size_t)size != ssize)
        return NULL;

    obj = alloc_obj(imem, size, &st_bytes,
                    ALLOC_IMMOVABLE | ALLOC_DIRECT, cname);
    if (obj == 0)
        return 0;
    alloc_trace("|+b.", imem, cname, NULL, size, obj);
    return (byte *)Memento_label(obj, cname);
}
static void *
i_alloc_struct(gs_memory_t * mem, gs_memory_type_ptr_t pstype,
               client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_size_t size = pstype->ssize;
    obj_header_t *obj;
    obj_header_t **pfl;

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    ALLOC_CHECK_SIZE(mem,pstype);
    IF_FREELIST_ALLOC(obj, imem, size, pstype, pfl)
        alloc_trace(":+<f", imem, cname, pstype, size, obj);
    ELSEIF_BIG_FREELIST_ALLOC(obj, imem, size, pstype)
        alloc_trace(":+<F", imem, cname, pstype, size, obj);
    ELSEIF_LIFO_ALLOC(obj, imem, size, pstype)
        alloc_trace(":+< ", imem, cname, pstype, size, obj);
    ELSE_ALLOC
    {
        obj = alloc_obj(imem, size, pstype, 0, cname);
        if (obj == 0)
            return 0;
        alloc_trace(":+<.", imem, cname, pstype, size, obj);
    }
#if IGC_PTR_STABILITY_CHECK
        obj[-1].d.o.space_id = imem->space_id;
#endif
    return Memento_label(obj, cname);
}
static void *
i_alloc_struct_immovable(gs_memory_t * mem, gs_memory_type_ptr_t pstype,
                         client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_size_t size = pstype->ssize;
    obj_header_t *obj;

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    ALLOC_CHECK_SIZE(mem,pstype);
    obj = alloc_obj(imem, size, pstype, ALLOC_IMMOVABLE | ALLOC_DIRECT, cname);
    alloc_trace("|+<.", imem, cname, pstype, size, obj);
    return Memento_label(obj, cname);
}

static inline bool
alloc_array_check_size(size_t num_elements, size_t elt_size, size_t *lsize)
{
    int shift0, shift1;
    size_t m, n;

    /* Avoid the loops in the overwhelming number of cases. */
    if ((num_elements | elt_size) >= 65536) {
        /* Slightly conservative, but it'll work for our purposes. */
        /* m is the maximum unsigned value representable in shift0 bits */
        for (m=0, shift0 = 0; m < num_elements; m = (m<<1)+1, shift0++);
        /* n is the maximum unsigned value representable in shift1 bits */
        for (n=0, shift1 = 0; n < elt_size; n = (n<<1)+1, shift1++);
        /* An shift0 bit unsigned number multiplied by an shift1 bit
         * unsigned number is guaranteed to fit in n+m-1 bits. */
        if (shift0+shift1-1 > 8*sizeof(size_t))
            return false; /* Overflow */
    }

    *lsize = num_elements * elt_size;
    return true;
}

static byte *
i_alloc_byte_array(gs_memory_t * mem, size_t num_elements, size_t elt_size,
                   client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *obj;
    size_t slsize;
    obj_size_t lsize;
#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif
    if (alloc_array_check_size(num_elements, elt_size, &slsize) == false)
        return NULL;
    lsize = (obj_size_t)slsize;
    if ((size_t)lsize != slsize)
        return NULL;
    obj = alloc_obj(imem, lsize,
                    &st_bytes, ALLOC_DIRECT, cname);

    if_debug6m('A', mem, "[a%d:+b.]%s -bytes-*(%"PRIuSIZE"=%"PRIuSIZE"*%"PRIuSIZE") = "PRI_INTPTR"\n",
               alloc_trace_space(imem), client_name_string(cname),
               num_elements * elt_size,
               num_elements, elt_size, (intptr_t)obj);
    return (byte *)Memento_label(obj, cname);
}
static byte *
i_alloc_byte_array_immovable(gs_memory_t * mem, size_t num_elements,
                             size_t elt_size, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *obj;
    size_t slsize;
    obj_size_t lsize;
#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif
    if (alloc_array_check_size(num_elements, elt_size, &slsize) == false)
        return NULL;
    lsize = (obj_size_t)slsize;
    if ((size_t)lsize != slsize)
        return NULL;
    obj = alloc_obj(imem, lsize,
                    &st_bytes, ALLOC_IMMOVABLE | ALLOC_DIRECT,
                    cname);

    if_debug6m('A', mem, "[a%d|+b.]%s -bytes-*(%"PRIuSIZE"=%"PRIuSIZE"*%"PRIuSIZE") = "PRI_INTPTR"\n",
               alloc_trace_space(imem), client_name_string(cname),
               num_elements * elt_size,
               num_elements, elt_size, (intptr_t)obj);
    return (byte *)Memento_label(obj, cname);
}
static void *
i_alloc_struct_array(gs_memory_t * mem, size_t num_elements,
                     gs_memory_type_ptr_t pstype, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *obj;
    size_t slsize;
    obj_size_t lsize;
#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    ALLOC_CHECK_SIZE(mem,pstype);
#ifdef DEBUG
    if (pstype->enum_ptrs == basic_enum_ptrs) {
        dmprintf2(mem, "  i_alloc_struct_array: called with incorrect structure type (not element), struct='%s', client='%s'\n",
                pstype->sname, cname);
        return NULL;		/* fail */
    }
#endif
    if (alloc_array_check_size(num_elements, pstype->ssize, &slsize) == false)
        return NULL;
    lsize = (obj_size_t)slsize;
    if ((size_t)lsize != slsize)
        return NULL;
    obj = alloc_obj(imem, lsize, pstype, ALLOC_DIRECT, cname);
    if_debug7m('A', mem, "[a%d:+<.]%s %s*(%"PRIuSIZE"=%"PRIuSIZE"*%u) = "PRI_INTPTR"\n",
               alloc_trace_space(imem), client_name_string(cname),
               struct_type_name_string(pstype),
               num_elements * pstype->ssize,
               num_elements, pstype->ssize, (intptr_t)obj);
    return (char *)Memento_label(obj, cname);
}
static void *
i_alloc_struct_array_immovable(gs_memory_t * mem, size_t num_elements,
                               gs_memory_type_ptr_t pstype, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *obj;
    size_t slsize;
    obj_size_t lsize;
#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    ALLOC_CHECK_SIZE(mem,pstype);
    if (alloc_array_check_size(num_elements, pstype->ssize, &slsize) == false)
        return NULL;
    lsize = (obj_size_t)slsize;
    if ((size_t)lsize != slsize)
        return NULL;
    obj = alloc_obj(imem, lsize, pstype, ALLOC_IMMOVABLE | ALLOC_DIRECT, cname);
    if_debug7m('A', mem, "[a%d|+<.]%s %s*(%"PRIuSIZE"=%"PRIuSIZE"*%u) = "PRI_INTPTR"\n",
               alloc_trace_space(imem), client_name_string(cname),
               struct_type_name_string(pstype),
               num_elements * pstype->ssize,
               num_elements, pstype->ssize, (intptr_t)obj);
    return (char *)Memento_label(obj, cname);
}
static void *
i_resize_object(gs_memory_t * mem, void *obj, size_t new_num_elements,
                client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *pp = (obj_header_t *) obj - 1;
    gs_memory_type_ptr_t pstype = pp->o_type;
    size_t old_size = pre_obj_contents_size(pp);
    size_t new_size = pstype->ssize * new_num_elements;
    size_t old_size_rounded = obj_align_round(old_size);
    size_t new_size_rounded = obj_align_round(new_size);
    void *new_obj = NULL;

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif

    if (new_size_rounded != (obj_size_t)new_size_rounded)
        return NULL;

    if (old_size_rounded == new_size_rounded) {
        pp->o_size = (obj_size_t)new_size;
        new_obj = obj;
    } else
        if (imem->cc && (byte *)obj + old_size_rounded == imem->cc->cbot &&
            imem->cc->ctop - (byte *)obj >= new_size_rounded ) {
            imem->cc->cbot = (byte *)obj + new_size_rounded;
            pp->o_size = (obj_size_t)new_size;
            new_obj = obj;
        } else /* try and trim the object -- but only if room for a dummy header */
            if (new_size_rounded + sizeof(obj_header_t) <= old_size_rounded) {
                trim_obj(imem, obj, (obj_size_t)new_size, (clump_t *)0);
                new_obj = obj;
            }
    if (new_obj) {
        if_debug8m('A', mem, "[a%d:%c%c ]%s %s(%"PRIuSIZE"=>%"PRIuSIZE") "PRI_INTPTR"\n",
                   alloc_trace_space(imem),
                   (new_size > old_size ? '>' : '<'),
                   (pstype == &st_bytes ? 'b' : '<'),
                   client_name_string(cname),
                   struct_type_name_string(pstype),
                   old_size, new_size, (intptr_t)obj);
        return Memento_label(new_obj, cname);
    }
    /* Punt. */
    new_obj = gs_alloc_struct_array(mem, new_num_elements, void,
                                    pstype, cname);
    if (new_obj == 0)
        return 0;
    memcpy(new_obj, obj, min(old_size, new_size));
    gs_free_object(mem, obj, cname);
    return Memento_label(new_obj, cname);
}
static void
i_free_object(gs_memory_t * mem, void *ptr, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    obj_header_t *pp;
    gs_memory_type_ptr_t pstype;
    gs_memory_struct_type_t saved_stype;

    struct_proc_finalize((*finalize));
    size_t size, rounded_size;

    if (ptr == 0)
        return;
    pp = (obj_header_t *) ptr - 1;
    pstype = pp->o_type;
#ifdef DEBUG
    if (gs_debug_c('?')) {
        clump_locator_t cld;

        if (pstype == &st_free) {
            mlprintf2(mem, "%s: object "PRI_INTPTR" already free!\n",
                      client_name_string(cname), (intptr_t)ptr);
            return;		/*gs_abort(); */
        }
        /* Check that this allocator owns the object being freed. */
        cld.memory = imem;
        while ((cld.cp = cld.memory->root),
               !clump_locate_ptr(ptr, &cld)
            ) {
            if (!cld.memory->saved) {
                mlprintf3(mem, "%s: freeing "PRI_INTPTR", not owned by memory "PRI_INTPTR"!\n",
                          client_name_string(cname), (intptr_t)ptr,
                          (intptr_t)mem);
                return;		/*gs_abort(); */
            }
                  /****** HACK: we know the saved state is the first ******
                   ****** member of an alloc_save_t. ******/
            cld.memory = (gs_ref_memory_t *) cld.memory->saved;
        }
        /* Check that the object is in the allocated region. */
        if (!(PTR_BETWEEN((const byte *)pp, cld.cp->cbase,
                          cld.cp->cbot))
            ) {
            mlprintf5(mem, "%s: freeing "PRI_INTPTR",\n\toutside clump "PRI_INTPTR" cbase="PRI_INTPTR", cbot="PRI_INTPTR"!\n",
                      client_name_string(cname), (intptr_t) ptr,
                      (intptr_t) cld.cp, (intptr_t) cld.cp->cbase,
                      (intptr_t) cld.cp->cbot);
            return;		/*gs_abort(); */
        }
    }
#endif
    size = pre_obj_contents_size(pp);
    rounded_size = obj_align_round(size);
    finalize = pstype->finalize;
    if (finalize != 0) {

        /* unfortunately device finalize procedures will clobber the
           stype which is used for later debugging with "A" debug
           tracing, so we save stype it in a local. */
        if (gs_debug['a'] || gs_debug['A'])
            saved_stype = *pstype;

        if_debug3m('u', mem, "[u]finalizing %s "PRI_INTPTR" (%s)\n",
                   struct_type_name_string(pstype),
                   (intptr_t)ptr, client_name_string(cname));
        (*finalize) (mem, ptr);

        if (gs_debug['a'] || gs_debug['A'])
            pstype = &saved_stype;
    }
    if (imem->cc && (byte *) ptr + rounded_size == imem->cc->cbot) {
        alloc_trace(":-o ", imem, cname, pstype, size, ptr);
        gs_alloc_fill(ptr, gs_alloc_fill_free, size);
        imem->cc->cbot = (byte *) pp;
        /* IFF this object is adjacent to (or below) the byte after the
         * highest free object, do the consolidation within this clump. */
        if ((byte *)pp <= imem->cc->int_freed_top) {
            consolidate_clump_free(imem->cc, imem);
        }
        return;
    }
    if (pp->o_alone) {
                /*
                 * We gave this object its own clump.  Free the entire clump,
                 * unless it belongs to an older save level, in which case
                 * we mustn't overwrite it.
                 */
        clump_locator_t cl;

#ifdef DEBUG
        {
            clump_locator_t cld;

            cld.memory = imem;
            cld.cp = 0;
            if (gs_debug_c('a'))
                alloc_trace(
                            (clump_locate_ptr(ptr, &cld) ? ":-oL" : ":-o~"),
                               imem, cname, pstype, size, ptr);
        }
#endif
        cl.memory = imem;
        cl.cp = 0;
        if (clump_locate_ptr(ptr, &cl)) {
            if (!imem->is_controlled)
                alloc_free_clump(cl.cp, imem);
            return;
        }
        /* Don't overwrite even if gs_alloc_debug is set. */
    }
    if (rounded_size >= sizeof(obj_header_t *)) {
        /*
         * Put the object on a freelist, unless it belongs to
         * an older save level, in which case we mustn't
         * overwrite it.
         */
        imem->cfreed.memory = imem;
        if (clump_locate(ptr, &imem->cfreed)) {
            obj_header_t **pfl;

            if (size > max_freelist_size) {
                pfl = &imem->freelists[LARGE_FREELIST_INDEX];
                if (rounded_size > imem->largest_free_size)
                    imem->largest_free_size = rounded_size;
            } else {
                pfl = &imem->freelists[(size + obj_align_mask) >>
                                      log2_obj_align_mod];
            }
            /* keep track of highest object on a freelist */
            /* If we're dealing with a block in the currently open clump
               (in imem->cc) update that, otherwise, update the clump in
               the clump list (in imem->cfreed.cp)
             */
            if (imem->cc && imem->cfreed.cp->chead == imem->cc->chead) {
                if ((byte *)pp >= imem->cc->int_freed_top) {
                    imem->cc->int_freed_top = (byte *)ptr + rounded_size;
                }
            }
            else {
                if ((byte *)pp >= imem->cfreed.cp->int_freed_top) {
                    imem->cfreed.cp->int_freed_top = (byte *)ptr + rounded_size;
                }
            }
            pp->o_type = &st_free;	/* don't confuse GC */
            o_set_unmarked(pp);
            gs_alloc_fill(ptr, gs_alloc_fill_free, size);
            *(obj_header_t **) ptr = *pfl;
            *pfl = (obj_header_t *) ptr;
            alloc_trace((size > max_freelist_size ? ":-oF" : ":-of"),
                        imem, cname, pstype, size, ptr);
            return;
        }
        /* Don't overwrite even if gs_alloc_debug is set. */
    } else {
        pp->o_type = &st_free;	/* don't confuse GC */
        gs_alloc_fill(ptr, gs_alloc_fill_free, size);
    }
    alloc_trace(":-o#", imem, cname, pstype, size, ptr);
    imem->lost.objects += obj_size_round(size);
}
static byte *
i_alloc_string(gs_memory_t * mem, size_t nbytes, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    byte *str;
    clump_splay_walker sw;

    /*
     * Cycle through the clumps at the current save level, starting
     * with the currently open one.
     */
    clump_t *cp = clump_splay_walk_init_mid(&sw, imem->cc);

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif
    if (cp == 0) {
        /* Open an arbitrary clump. */
        imem->cc = clump_splay_walk_init(&sw, imem);
        alloc_open_clump(imem);
    }
top:
    if (imem->cc && !imem->cc->c_alone && imem->cc->ctop - imem->cc->cbot > nbytes) {
        if_debug4m('A', mem, "[a%d:+> ]%s(%"PRIuSIZE") = "PRI_INTPTR"\n",
                   alloc_trace_space(imem), client_name_string(cname), nbytes,
                   (intptr_t)(imem->cc->ctop - nbytes));
        str = imem->cc->ctop -= nbytes;
        gs_alloc_fill(str, gs_alloc_fill_alloc, nbytes);
        return str;
    }
    /* Try the next clump. */
    cp = clump_splay_walk_fwd(&sw);

    if (cp != NULL)
    {
        alloc_close_clump(imem);
        imem->cc = cp;
        alloc_open_clump(imem);
        goto top;
    }
    if (nbytes > string_space_quanta(SIZE_MAX - sizeof(clump_head_t)) *
        string_data_quantum
        ) {			/* Can't represent the size in a uint! */
        return 0;
    }
    if (nbytes >= imem->large_size) {	/* Give it a clump all its own. */
        return i_alloc_string_immovable(mem, nbytes, cname);
    } else {			/* Add another clump. */
        cp = alloc_acquire_clump(imem, (ulong) imem->clump_size, true, "clump");

        if (cp == 0)
            return 0;
        alloc_close_clump(imem);
        imem->cc = clump_splay_walk_init_mid(&sw, cp);
        gs_alloc_fill(imem->cc->cbase, gs_alloc_fill_free,
                      imem->cc->climit - imem->cc->cbase);
        goto top;
    }
}
static byte *
i_alloc_string_immovable(gs_memory_t * mem, size_t nbytes, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    byte *str;
    size_t asize;
    clump_t *cp;

#ifdef MEMENTO
    if (Memento_failThisEvent())
        return NULL;
#endif
    /* Give it a clump all its own. */
    asize = string_clump_space(nbytes) + sizeof(clump_head_t);
    cp = alloc_acquire_clump(imem, asize, true, "large string clump");

    if (cp == 0)
        return 0;
    cp->c_alone = true;

    str = cp->ctop = cp->climit - nbytes;
    if_debug4m('a', mem, "[a%d|+>L]%s(%"PRIuSIZE") = "PRI_INTPTR"\n",
               alloc_trace_space(imem), client_name_string(cname), nbytes,
               (intptr_t)str);
    gs_alloc_fill(str, gs_alloc_fill_alloc, nbytes);

    return Memento_label(str, cname);
}

static byte *
i_resize_string(gs_memory_t * mem, byte * data, size_t old_num, size_t new_num,
                client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    byte *ptr;

    if (old_num == new_num)	/* same size returns the same string */
        return data;

    if ( imem->cc && data == imem->cc->ctop &&	/* bottom-most string */
        (new_num < old_num ||
         imem->cc->ctop - imem->cc->cbot > new_num - old_num)
        ) {			/* Resize in place. */
        ptr = data + old_num - new_num;
        if_debug6m('A', mem, "[a%d:%c> ]%s(%"PRIuSIZE"->%"PRIuSIZE") "PRI_INTPTR"\n",
                   alloc_trace_space(imem),
                   (new_num > old_num ? '>' : '<'),
                   client_name_string(cname), old_num, new_num,
                   (intptr_t)ptr);
        imem->cc->ctop = ptr;
        memmove(ptr, data, min(old_num, new_num));
#ifdef DEBUG
        if (new_num > old_num)
            gs_alloc_fill(ptr + old_num, gs_alloc_fill_alloc,
                          new_num - old_num);
        else
            gs_alloc_fill(data, gs_alloc_fill_free, old_num - new_num);
#endif
    } else
        if (new_num < old_num) {
            /* trim the string and create a free space hole */
            ptr = data;
            imem->lost.strings += old_num - new_num;
            gs_alloc_fill(data + new_num, gs_alloc_fill_free,
                          old_num - new_num);
            if_debug5m('A', mem, "[a%d:<> ]%s(%"PRIuSIZE"->%"PRIuSIZE") "PRI_INTPTR"\n",
                       alloc_trace_space(imem), client_name_string(cname),
                       old_num, new_num, (intptr_t)ptr);
        } else {			/* Punt. */
            ptr = gs_alloc_string(mem, new_num, cname);
            if (ptr == 0)
                return 0;
            memcpy(ptr, data, min(old_num, new_num));
            gs_free_string(mem, data, old_num, cname);
        }

    return ptr;
}

static void
i_free_string(gs_memory_t * mem, byte * data, size_t nbytes,
              client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;

    if (data) {
        if (imem->cc && data == imem->cc->ctop) {
            if_debug4m('A', mem, "[a%d:-> ]%s(%"PRIuSIZE") "PRI_INTPTR"\n",
                       alloc_trace_space(imem), client_name_string(cname), nbytes,
                       (intptr_t)data);
            imem->cc->ctop += nbytes;
        } else {
            if_debug4m('A', mem, "[a%d:->#]%s(%"PRIuSIZE") "PRI_INTPTR"\n",
                       alloc_trace_space(imem), client_name_string(cname), nbytes,
                       (intptr_t)data);
            imem->lost.strings += nbytes;
        }
        gs_alloc_fill(data, gs_alloc_fill_free, nbytes);
    }
}

static gs_memory_t *
i_stable(gs_memory_t *mem)
{
    return mem->stable_memory;
}

static void
i_status(gs_memory_t * mem, gs_memory_status_t * pstat)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    size_t unused = imem->lost.refs + imem->lost.strings;
    size_t inner = 0;
    clump_splay_walker sw;
    clump_t *cp;

    alloc_close_clump(imem);
    /* Add up unallocated space within each clump. */
    /* Also keep track of space allocated to inner clumps, */
    /* which are included in previous_status.allocated. */
    for (cp = clump_splay_walk_init(&sw, imem); cp != NULL; cp = clump_splay_walk_fwd(&sw))
    {
        unused += cp->ctop - cp->cbot;
        if (cp->outer)
            inner += cp->cend - (byte *) cp->chead;
    }
    unused += compute_free_objects(imem);
    pstat->used = imem->allocated + inner - unused +
        imem->previous_status.used;
    pstat->allocated = imem->allocated +
        imem->previous_status.allocated;
    pstat->max_used = 0;		/* unknown for this allocator */
    pstat->is_thread_safe = false;	/* this allocator is not thread safe */
}

static void
i_enable_free(gs_memory_t * mem, bool enable)
{
    if (enable)
        mem->procs.free_object = i_free_object,
            mem->procs.free_string = i_free_string;
    else
        mem->procs.free_object = gs_ignore_free_object,
            mem->procs.free_string = gs_ignore_free_string;
}

static void i_set_object_type(gs_memory_t *mem, void *ptr, gs_memory_type_ptr_t type)
{
    obj_header_t *pp;

    if (ptr == 0)
        return;
    pp = (obj_header_t *) ptr - 1;
    pp->o_type = type;
}

static void i_defer_frees(gs_memory_t *mem, int defer)
{
}

/* ------ Internal procedures ------ */

/* Compute the amount of free object space by scanning free lists. */
static size_t
compute_free_objects(gs_ref_memory_t * mem)
{
    size_t unused = mem->lost.objects;
    int i;

    /* Add up space on free lists. */
    for (i = 0; i < num_freelists; i++) {
        const obj_header_t *pfree;

        for (pfree = mem->freelists[i]; pfree != 0;
             pfree = *(const obj_header_t * const *)pfree
            )
            unused += obj_align_round(pfree[-1].o_size);
    }
    return unused;
}

/* Allocate an object from the large-block freelist. */
static obj_header_t *	/* rets obj if allocated, else 0 */
large_freelist_alloc(gs_ref_memory_t *mem, obj_size_t size)
{
    /* Scan large object freelist. We'll grab an object up to 1/8 bigger */
    /* right away, else use best fit of entire scan. */
    obj_size_t aligned_size = obj_align_round(size);
    size_t aligned_min_size = aligned_size + sizeof(obj_header_t);
    size_t aligned_max_size =
        aligned_min_size + obj_align_round(aligned_min_size / 8);
    obj_header_t *best_fit = 0;
    obj_header_t **best_fit_prev = NULL; /* Initialize against indeterminism. */
    obj_size_t best_fit_size = (obj_size_t)SIZE_MAX;
    obj_header_t *pfree;
    obj_header_t **ppfprev = &mem->freelists[LARGE_FREELIST_INDEX];
    size_t largest_size = 0;

    if (aligned_size > mem->largest_free_size)
        return 0;		/* definitely no block large enough */

    while ((pfree = *ppfprev) != 0) {
        obj_size_t free_size = obj_align_round(pfree[-1].o_size);

        if (free_size == aligned_size ||
            (free_size >= aligned_min_size && free_size < best_fit_size)
            ) {
            best_fit = pfree;
            best_fit_prev = ppfprev;
            best_fit_size = pfree[-1].o_size;
            if (best_fit_size <= aligned_max_size)
                break;	/* good enough fit to spare scan of entire list */
        }
        ppfprev = (obj_header_t **) pfree;
        if (free_size > largest_size)
            largest_size = free_size;
    }
    if (best_fit == 0) {
        /*
         * No single free clump is large enough, but since we scanned the
         * entire list, we now have an accurate updated value for
         * largest_free_size.
         */
        mem->largest_free_size = largest_size;
        return 0;
    }

    /* Remove from freelist & return excess memory to free */
    *best_fit_prev = *(obj_header_t **)best_fit;
    trim_obj(mem, best_fit, aligned_size, (clump_t *)0);

    /* Pre-init block header; o_alone & o_type are already init'd */
    best_fit[-1].o_size = size;

    return best_fit;
}

/* Allocate an object.  This handles all but the fastest, simplest case. */
static obj_header_t *
alloc_obj(gs_ref_memory_t *mem, obj_size_t lsize, gs_memory_type_ptr_t pstype,
          alloc_flags_t flags, client_name_t cname)
{
    obj_header_t *ptr;

    if (lsize >= mem->large_size || (flags & ALLOC_IMMOVABLE)) {
        /*
         * Give the object a clump all its own.  Note that this case does
         * not occur if is_controlled is true.
         */
        obj_size_t asize =
            ((lsize + obj_align_mask) & -obj_align_mod) +
            sizeof(obj_header_t);
        clump_t *cp =
            alloc_acquire_clump(mem, asize + sizeof(clump_head_t), false,
                                "large object clump");

        if (asize < lsize)
            return 0;
        if (cp == 0)
            return 0;
        cp->c_alone = true;
        ptr = (obj_header_t *) cp->cbot;
        cp->cbot += asize;
        ptr->o_pad = 0;
        ptr->o_alone = 1;
        ptr->o_size = (obj_size_t)lsize;
    } else {
        /*
         * Cycle through the clumps at the current save level, starting
         * with the currently open one.
         */
        clump_splay_walker sw;
        clump_t *cp = clump_splay_walk_init_mid(&sw, mem->cc);
        obj_size_t asize = obj_size_round(lsize);
        bool allocate_success = false;

        if (lsize > max_freelist_size && (flags & ALLOC_DIRECT)) {
            /* We haven't checked the large block freelist yet. */
            if ((ptr = large_freelist_alloc(mem, lsize)) != 0) {
                --ptr;			/* must point to header */
                goto done;
            }
        }

        if (cp == 0) {
            /* Open an arbitrary clump. */
            mem->cc = clump_splay_walk_init(&sw, mem);
            alloc_open_clump(mem);
        }

#define CAN_ALLOC_AT_END(cp)\
  ((cp) && !((cp)->c_alone) && (cp)->ctop - (byte *) (ptr = (obj_header_t *) (cp)->cbot)\
   > asize + sizeof(obj_header_t))

        do {
            if (CAN_ALLOC_AT_END(mem->cc)) {
                allocate_success = true;
                break;
            } else if (mem->is_controlled) {
                /* Try consolidating free space. */
                gs_consolidate_free((gs_memory_t *)mem);
                if (CAN_ALLOC_AT_END(mem->cc)) {
                    allocate_success = true;
                    break;
                }
            }
            /* No luck, go on to the next clump. */
            cp = clump_splay_walk_fwd(&sw);
            if (cp == NULL)
                break;

            alloc_close_clump(mem);
            mem->cc = cp;
            alloc_open_clump(mem);
        }
        while (1);

#ifdef CONSOLIDATE_BEFORE_ADDING_CLUMP
        if (!allocate_success) {
            /*
             * Try consolidating free space before giving up.
             * It's not clear this is a good idea, since it requires quite
             * a lot of computation and doesn't seem to improve things much.
             */
            if (!mem->is_controlled) { /* already did this if controlled */
                clump_t *cp;

                alloc_close_clump(mem);
                for (cp = clump_splay_walk_init_mid(&sw, cp_orig); cp != NULL; cp = clump_splay_walk_fwd(&sw))
                {
                    consolidate_clump_free(cp, mem);
                    if (CAN_ALLOC_AT_END(cp)) {
                        mem->cc = cp;
                        alloc_open_clump(mem);
                        allocate_success = true;
                        break;
                    }
                }
            }
        }
#endif

#undef CAN_ALLOC_AT_END

        if (!allocate_success) {
            /* Add another clump. */
            clump_t *cp =
                alloc_add_clump(mem, mem->clump_size, "clump");

            if (cp) {
                /* mem->cc == cp */
                ptr = (obj_header_t *)cp->cbot;
                allocate_success = true;
            }
        }

        /*
         * If no success, try to scavenge from low free memory. This is
         * only enabled for controlled memory (currently only async
         * renderer) because it's too much work to prevent it from
         * examining outer save levels in the general case.
         */
        if (allocate_success)
            mem->cc->cbot = (byte *) ptr + asize;
        else if (!mem->is_controlled ||
                 (ptr = scavenge_low_free(mem, lsize)) == 0)
            return 0;	/* allocation failed */
        ptr->o_pad = 0;
        ptr->o_alone = 0;
        ptr->o_size = lsize;
    }
done:
    ptr->o_type = pstype;
#   if IGC_PTR_STABILITY_CHECK
        ptr->d.o.space_id = mem->space_id;
#   endif
    ptr++;
    gs_alloc_fill(ptr, gs_alloc_fill_alloc, lsize);
    return Memento_label(ptr, cname);
}

/*
 * Consolidate free objects contiguous to free space at cbot onto the cbot
 * area. Also keep track of end of highest internal free object
 * (int_freed_top).
 */
static void
consolidate_clump_free(clump_t *cp, gs_ref_memory_t *mem)
{
    obj_header_t *begin_free = 0;

    cp->int_freed_top = cp->cbase;	/* below all objects in clump */
    SCAN_CLUMP_OBJECTS(cp)
    DO_ALL
        if (pre->o_type == &st_free) {
            if (begin_free == 0)
                begin_free = pre;
        } else {
            if (begin_free)
                cp->int_freed_top = (byte *)pre; /* first byte following internal free */
            begin_free = 0;
        }
    END_OBJECTS_SCAN
    if (begin_free) {
        /* We found free objects at the top of the object area. */
        /* Remove the free objects from the freelists. */
        remove_range_from_freelist(mem, begin_free, cp->cbot);
        if_debug4m('a', (const gs_memory_t *)mem,
                   "[a]resetting clump "PRI_INTPTR" cbot from "PRI_INTPTR" to "PRI_INTPTR" (%lu free)\n",
                   (intptr_t)cp, (intptr_t)cp->cbot, (intptr_t)begin_free,
                   (intptr_t)((byte *)cp->cbot - (byte *)begin_free));
        cp->cbot = (byte *) begin_free;
    }
}

static splay_app_result_t
consolidate(clump_t *cp, void *arg)
{
    gs_ref_memory_t *mem = (gs_ref_memory_t *)arg;

    consolidate_clump_free(cp, mem);
    if (cp->cbot == cp->cbase && cp->ctop == cp->climit) {
        /* The entire clump is free. */
        if (!mem->is_controlled) {
            alloc_free_clump(cp, mem);
            if (mem->cc == cp)
                mem->cc = NULL;
        }
    }

    return SPLAY_APP_CONTINUE;
}

/* Consolidate free objects. */
void
ialloc_consolidate_free(gs_ref_memory_t *mem)
{
    alloc_close_clump(mem);

    /* We used to visit clumps in reverse order to encourage LIFO behavior,
     * but with binary trees this is not possible (unless you want to
     * either change the tree during the process, recurse, or otherwise
     * hold the state). */
    clump_splay_app(mem->root, mem, consolidate, mem);

    /* NOTE: Previously, if we freed the current clump, we'd move to whatever the
     * bigger of it's children was. We now just move to the root. */
    if (mem->cc == NULL)
        mem->cc = mem->root;

    alloc_open_clump(mem);
}
static void
i_consolidate_free(gs_memory_t *mem)
{
    ialloc_consolidate_free((gs_ref_memory_t *)mem);
}

typedef struct
{
    uint need_free;
    obj_header_t *found_pre;
    gs_ref_memory_t *mem;
    obj_size_t request_size;
} scavenge_data;

static splay_app_result_t
scavenge(clump_t *cp, void *arg)
{
    scavenge_data *sd = (scavenge_data *)arg;
    obj_header_t *begin_free = NULL;
    obj_size_t found_free = 0;

    sd->found_pre = NULL;

    SCAN_CLUMP_OBJECTS(cp)
    DO_ALL
        if (pre->o_type == &st_free) {
            if (begin_free == 0) {
                found_free = 0;
                begin_free = pre;
            }
            found_free += pre_obj_rounded_size(pre);
            if (begin_free != 0 && found_free >= sd->need_free)
                break;
        } else
            begin_free = 0;
    END_OBJECTS_SCAN_NO_ABORT

    if (begin_free != 0 && found_free >= sd->need_free) {
        /* Fish found pieces out of various freelists */
        remove_range_from_freelist(sd->mem, (char*)begin_free,
                                   (char*)begin_free + found_free);

        /* Prepare found object */
        sd->found_pre = begin_free;
        sd->found_pre->o_type = &st_free;  /* don't confuse GC if gets lost */
        sd->found_pre->o_size = found_free - sizeof(obj_header_t);

        /* Chop off excess tail piece & toss it back into free pool */
        trim_obj(sd->mem, sd->found_pre + 1, sd->request_size, cp);
        return SPLAY_APP_STOP;
    }

    return SPLAY_APP_CONTINUE;
}

/* try to free-up given amount of space from freespace below clump base */
static obj_header_t *	/* returns uninitialized object hdr, NULL if none found */
scavenge_low_free(gs_ref_memory_t *mem, obj_size_t request_size)
{
    /* find 1st range of memory that can be glued back together to fill request */
    scavenge_data sd;
    obj_size_t request_size_rounded = obj_size_round(request_size);

    sd.found_pre = 0;
    sd.need_free = request_size_rounded + sizeof(obj_header_t);    /* room for GC's dummy hdr */
    sd.mem = mem;
    sd.request_size = request_size;

    clump_splay_app(mem->root, mem, scavenge, &sd);
    return sd.found_pre;
}

/* Remove range of memory from a mem's freelists */
static void
remove_range_from_freelist(gs_ref_memory_t *mem, void* bottom, void* top)
{
    int num_free[num_freelists];
    int smallest = num_freelists, largest = -1;
    const obj_header_t *cur;
    obj_size_t size;
    int i;
    obj_size_t removed = 0;

    /*
     * Scan from bottom to top, a range containing only free objects,
     * counting the number of objects of each size.
     */

    for (cur = bottom; cur != top;
         cur = (const obj_header_t *)
             ((const byte *)cur + obj_size_round(size))
        ) {
        size = cur->o_size;
        i = (size > max_freelist_size ? LARGE_FREELIST_INDEX :
             (size + obj_align_mask) >> log2_obj_align_mod);
        if (i < smallest) {
            /*
             * 0-length free blocks aren't kept on any list, because
             * they don't have room for a pointer.
             */
            if (i == 0)
                continue;
            if (smallest < num_freelists)
                memset(&num_free[i], 0, (smallest - i) * sizeof(int));
            else
                num_free[i] = 0;
            smallest = i;
        }
        if (i > largest) {
            if (largest >= 0)
                memset(&num_free[largest + 1], 0, (i - largest) * sizeof(int));
            largest = i;
        }
        num_free[i]++;
    }

    /*
     * Remove free objects from the freelists, adjusting lost.objects by
     * subtracting the size of the region being processed minus the amount
     * of space reclaimed.
     */

    for (i = smallest; i <= largest; i++) {
        int count = num_free[i];
        obj_header_t *pfree;
        obj_header_t **ppfprev;

        if (!count)
            continue;
        ppfprev = &mem->freelists[i];
        for (;;) {
            pfree = *ppfprev;
            if (PTR_GE(pfree, bottom) && PTR_LT(pfree, top)) {
                /* We're removing an object. */
                *ppfprev = *(obj_header_t **) pfree;
                removed += obj_align_round(pfree[-1].o_size);
                if (!--count)
                    break;
            } else
                ppfprev = (obj_header_t **) pfree;
        }
    }
    mem->lost.objects -= (char*)top - (char*)bottom - removed;
}

/* Trim a memory object down to a given size */
static void
trim_obj(gs_ref_memory_t *mem, obj_header_t *obj, obj_size_t size, clump_t *cp)
/* Obj must have rounded size == req'd size, or have enough room for */
/* trailing dummy obj_header */
{
    obj_size_t rounded_size = obj_align_round(size);
    obj_header_t *pre_obj = obj - 1;
    obj_header_t *excess_pre = (obj_header_t*)((char*)obj + rounded_size);
    obj_size_t old_rounded_size = obj_align_round(pre_obj->o_size);
    obj_size_t excess_size = old_rounded_size - rounded_size - sizeof(obj_header_t);

    /* trim object's size to desired */
    pre_obj->o_size = size;
    if (old_rounded_size == rounded_size)
        return;	/* nothing more to do here */
    /*
     * If the object is alone in its clump, move cbot to point to the end
     * of the object.
     */
    if (pre_obj->o_alone) {
        if (!cp) {
            mem->cfreed.memory = mem;
            if (clump_locate(obj, &mem->cfreed)) {
                cp = mem->cfreed.cp;
            }
        }
        if (cp) {
#ifdef DEBUG
            if (cp->cbot != (byte *)obj + old_rounded_size) {
                lprintf3("resizing "PRI_INTPTR", old size %u, new size %u, cbot wrong!\n",
                         (intptr_t)obj, old_rounded_size, size);
                /* gs_abort */
            } else
#endif
                {
                    cp->cbot = (byte *)excess_pre;
                    return;
                }
        }
        /*
         * Something very weird is going on.  This probably shouldn't
         * ever happen, but if it does....
         */
        pre_obj->o_pad = 0;
        pre_obj->o_alone = 0;
    }
    /* make excess into free obj */
    excess_pre->o_type = &st_free;  /* don't confuse GC */
    excess_pre->o_size = excess_size;
    excess_pre->o_pad = 0;
    excess_pre->o_alone = 0;
    if (excess_size >= obj_align_mod) {
        /* Put excess object on a freelist */
        obj_header_t **pfl;

        if (mem->cc && (byte *)excess_pre >= mem->cc->int_freed_top)
            mem->cc->int_freed_top = (byte *)excess_pre + excess_size;
        if (excess_size <= max_freelist_size)
            pfl = &mem->freelists[(excess_size + obj_align_mask) >>
                                 log2_obj_align_mod];
        else {
            uint rounded_size = obj_align_round(excess_size);

            pfl = &mem->freelists[LARGE_FREELIST_INDEX];
            if (rounded_size > mem->largest_free_size)
                mem->largest_free_size = rounded_size;
        }
        *(obj_header_t **) (excess_pre + 1) = *pfl;
        *pfl = excess_pre + 1;
        mem->cfreed.memory = mem;
    } else {
        /* excess piece will be "lost" memory */
        mem->lost.objects += excess_size + sizeof(obj_header_t);
    }
}

/* ================ Roots ================ */

/* Register a root. */
static int
i_register_root(gs_memory_t * mem, gs_gc_root_t ** rpp, gs_ptr_type_t ptype,
                void **up, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    gs_gc_root_t *rp;

    if (rpp == NULL || *rpp == NULL) {
        rp = gs_raw_alloc_struct_immovable(imem->non_gc_memory, &st_gc_root_t,
                                           "i_register_root");
        if (rp == 0)
            return_error(gs_error_VMerror);
        rp->free_on_unregister = true;
        if (rpp && *rpp == NULL)
            *rpp = rp;
    } else {
        rp = *rpp;
        rp->free_on_unregister = false;
    }
    if_debug3m('8', mem, "[8]register root(%s) "PRI_INTPTR" -> "PRI_INTPTR"\n",
               client_name_string(cname), (intptr_t)rp, (intptr_t)up);
    rp->ptype = ptype;
    rp->p = up;
    rp->next = imem->roots;
    imem->roots = rp;
    return 0;
}

/* Unregister a root. */
static void
i_unregister_root(gs_memory_t * mem, gs_gc_root_t * rp, client_name_t cname)
{
    gs_ref_memory_t * const imem = (gs_ref_memory_t *)mem;
    gs_gc_root_t **rpp = &imem->roots;

    if_debug2m('8', mem, "[8]unregister root(%s) "PRI_INTPTR"\n",
               client_name_string(cname), (intptr_t)rp);
    while (*rpp != rp)
        rpp = &(*rpp)->next;
    *rpp = (*rpp)->next;
    if (rp->free_on_unregister)
        gs_free_object(imem->non_gc_memory, rp, "i_unregister_root");
}

/* ================ clumps ================ */

public_st_clump();

/* Insert a clump in the chain.  This is exported for the GC and for */
/* the forget_save operation. */
void
alloc_link_clump(clump_t * cp, gs_ref_memory_t * imem)
{
    splay_insert(cp, imem);
    SANITY_CHECK(cp);
}

/* Add a clump for ordinary allocation. */
static clump_t *
alloc_add_clump(gs_ref_memory_t * mem, size_t csize, client_name_t cname)
{
    clump_t *cp = alloc_acquire_clump(mem, csize, true, cname);

    if (cp) {
        alloc_close_clump(mem);
        mem->cc = cp;
        gs_alloc_fill(mem->cc->cbase, gs_alloc_fill_free,
                      mem->cc->climit - mem->cc->cbase);
    }
    return cp;
}

/* Acquire a clump.  If we would exceed MaxLocalVM (if relevant), */
/* or if we would exceed the VMThreshold and psignal is NULL, */
/* return 0; if we would exceed the VMThreshold but psignal is valid, */
/* just set the signal and return successfully. */
static clump_t *
alloc_acquire_clump(gs_ref_memory_t * mem, size_t csize, bool has_strings,
                    client_name_t cname)
{
    gs_memory_t *parent = mem->non_gc_memory;
    clump_t *cp;
    byte *cdata;

#if ARCH_SIZEOF_LONG > ARCH_SIZEOF_INT
    /* If csize is larger than max_uint, punt. */
    if (csize != (uint) csize)
        return 0;
#endif
    cp = gs_raw_alloc_struct_immovable(parent, &st_clump, cname);

    /* gc_status.signal_value is initialised to zero when the
     * allocator is created, only the Postscript interpreter
     * (which implement garbage collection) takes the action to set
     * it to anything other than zero
     */
    if( mem->gc_status.signal_value != 0) {
        /* we have a garbage collector */
        if (mem->allocated >= mem->limit) {
            mem->gc_status.requested += csize;
            if (mem->limit >= mem->gc_status.max_vm) {
                gs_free_object(parent, cp, cname);
                return 0;
            }
            if_debug4m('0', (const gs_memory_t *)mem,
                       "[0]signaling space=%d, allocated=%"PRIdSIZE", limit=%"PRIdSIZE", requested=%"PRIdSIZE"\n",
                       mem->space, mem->allocated,
                       mem->limit, mem->gc_status.requested);
            mem->gs_lib_ctx->gcsignal = mem->gc_status.signal_value;
        }
    }
    cdata = gs_alloc_bytes_immovable(parent, csize, cname);
    if (cp == 0 || cdata == 0) {
        gs_free_object(parent, cdata, cname);
        gs_free_object(parent, cp, cname);
        mem->gc_status.requested = csize;
        return 0;
    }
    alloc_init_clump(cp, cdata, cdata + csize, has_strings, (clump_t *) 0);
    alloc_link_clump(cp, mem);
    mem->allocated += st_clump.ssize + csize;
    SANITY_CHECK(cp);
    return cp;
}

/* Initialize the pointers in a clump.  This is exported for save/restore. */
/* The bottom pointer must be aligned, but the top pointer need not */
/* be aligned. */
void
alloc_init_clump(clump_t * cp, byte * bot, byte * top, bool has_strings,
                 clump_t * outer)
{
    byte *cdata = bot;

    if (outer != 0)
        outer->inner_count++;
    cp->chead = (clump_head_t *) cdata;
    cdata += sizeof(clump_head_t);
    cp->cbot = cp->cbase = cp->int_freed_top = cdata;
    cp->cend = top;
    cp->rcur = 0;
    cp->rtop = 0;
    cp->outer = outer;
    cp->inner_count = 0;
    cp->has_refs = false;
    cp->sbase = cdata;
    cp->c_alone = false; /* should be set correctly by caller */
    if (has_strings && top - cdata >= string_space_quantum + sizeof(long) - 1) {
        /*
         * We allocate a large enough string marking and reloc table
         * to cover the entire clump.
         */
        uint nquanta = string_space_quanta(top - cdata);

        cp->climit = cdata + nquanta * string_data_quantum;
        cp->smark = cp->climit;
        cp->smark_size = string_quanta_mark_size(nquanta);
        cp->sreloc =
            (string_reloc_offset *) (cp->smark + cp->smark_size);
        cp->sfree1 = (uint *) cp->sreloc;
    } else {
        /* No strings, don't need the string GC tables. */
        cp->climit = cp->cend;
        cp->sfree1 = 0;
        cp->smark = 0;
        cp->smark_size = 0;
        cp->sreloc = 0;
    }
    cp->ctop = cp->climit;
    alloc_init_free_strings(cp);
}

/* Initialize the string freelists in a clump. */
void
alloc_init_free_strings(clump_t * cp)
{
    if (cp->sfree1)
        memset(cp->sfree1, 0, STRING_FREELIST_SPACE(cp));
    cp->sfree = 0;
}

/* Close up the current clump. */
/* This is exported for save/restore and the GC. */
void
alloc_close_clump(gs_ref_memory_t * mem)
{
#ifdef DEBUG
    if (gs_debug_c('A')) {
        dmlprintf1((const gs_memory_t *)mem, "[a%d]", alloc_trace_space(mem));
        dmprintf_clump((const gs_memory_t *)mem, "closing clump", mem->cc);
    }
#endif
}

/* Reopen the current clump after a GC or restore. */
void
alloc_open_clump(gs_ref_memory_t * mem)
{
#ifdef DEBUG
    if (gs_debug_c('A')) {
        dmlprintf1((const gs_memory_t *)mem, "[a%d]", alloc_trace_space(mem));
        dmprintf_clump((const gs_memory_t *)mem, "opening clump", mem->cc);
    }
#endif
}

#ifdef DEBUG
static splay_app_result_t
check_in_clump(clump_t *cp, void *arg)
{
    clump_t **cpp = (clump_t **)arg;

    if (*cpp != cp)
        return SPLAY_APP_CONTINUE;
    *cpp = NULL;

    return SPLAY_APP_STOP;
}
#endif

/* Remove a clump from the chain.  This is exported for the GC. */
void
alloc_unlink_clump(clump_t * cp, gs_ref_memory_t * mem)
{
    SANITY_CHECK_MID(cp);
#ifdef DEBUG
    if (gs_alloc_debug) {	/* Check to make sure this clump belongs to this allocator. */
        clump_t *found = cp;
        clump_splay_app(mem->root, mem, check_in_clump, &found);

        if (found != NULL) {
            mlprintf2((const gs_memory_t *)mem, "unlink_clump "PRI_INTPTR" not owned by memory "PRI_INTPTR"!\n",
                      (intptr_t)cp, (intptr_t)mem);
            return;		/*gs_abort(); */
        }
    }
#endif
    (void)clump_splay_remove(cp, mem);
    if (mem->cc == cp) {
        mem->cc = NULL;
    }
}

/*
 * Free a clump.  This is exported for the GC.  Since we eventually use
 * this to free the clump containing the allocator itself, we must be
 * careful not to reference anything in the allocator after freeing the
 * clump data.
 */
void
alloc_free_clump(clump_t * cp, gs_ref_memory_t * mem)
{
    gs_memory_t *parent = mem->non_gc_memory;
    byte *cdata = (byte *)cp->chead;
    ulong csize = (byte *)cp->cend - cdata;

    alloc_unlink_clump(cp, mem);
    mem->allocated -= st_clump.ssize;
    if (mem->cfreed.cp == cp)
        mem->cfreed.cp = 0;
    if (cp->outer == 0) {
        mem->allocated -= csize;
        gs_free_object(parent, cdata, "alloc_free_clump(data)");
    } else {
        cp->outer->inner_count--;
        gs_alloc_fill(cdata, gs_alloc_fill_free, csize);
    }
    gs_free_object(parent, cp, "alloc_free_clump(clump struct)");
}

/* Find the clump for a pointer. */
/* Note that this only searches the current save level. */
/* Since a given save level can't contain both a clump and an inner clump */
/* of that clump, we can stop when is_within_clump succeeds, and just test */
/* is_in_inner_clump then. */
bool
clump_locate_ptr(const void *ptr, clump_locator_t * clp)
{
    clump_t *cp = clp->memory->root;

    while (cp)
    {
        if (PTR_LT(ptr, cp->cbase))
        {
            cp = cp->left;
            continue;
        }
        if (PTR_GE(ptr, cp->cend))
        {
            cp = cp->right;
            continue;
        }
        /* Found it! */
        splay_move_to_root(cp, clp->memory);
        clp->cp = cp;
        return !ptr_is_in_inner_clump(ptr, cp);
    }
    return false;
}

bool ptr_is_within_mem_clumps(const void *ptr, gs_ref_memory_t *mem)
{
    clump_t *cp = mem->root;

    while (cp)
    {
        if (PTR_LT(ptr, cp->cbase))
        {
            cp = cp->left;
            continue;
        }
        if (PTR_GE(ptr, cp->cend))
        {
            cp = cp->right;
            continue;
        }
        /* Found it! */
        splay_move_to_root(cp, mem);
        return true;
    }
    return false;
}

/* ------ Debugging ------ */

#ifdef DEBUG

#include "string_.h"

static inline bool
obj_in_control_region(const void *obot, const void *otop,
                      const dump_control_t *pdc)
{
    return
        ((pdc->bottom == NULL || PTR_GT(otop, pdc->bottom)) &&
         (pdc->top == NULL || PTR_LT(obot, pdc->top)));
}

const dump_control_t dump_control_default =
{
    dump_do_default, NULL, NULL
};
const dump_control_t dump_control_all =
{
    dump_do_strings | dump_do_type_addresses | dump_do_pointers |
    dump_do_pointed_strings | dump_do_contents, NULL, NULL
};

const dump_control_t dump_control_no_contents =
{
    dump_do_strings | dump_do_type_addresses | dump_do_pointers |
    dump_do_pointed_strings, NULL, NULL
};

/*
 * Internal procedure to dump a block of memory, in hex and optionally
 * also as characters.
 */
static void
debug_indent(const gs_memory_t *mem, int indent)
{
    int i;

    for (i = indent; i > 0; --i)
        dmputc(mem, ' ');
}
static void
debug_dump_contents(const gs_memory_t *mem, const byte * bot,
                    const byte * top, int indent, bool as_chars)
{
    const byte *block;

#define block_size 16

    if (bot >= top)
        return;
    for (block = bot - ((bot - (byte *) 0) & (block_size - 1));
         block < top; block += block_size
        ) {
        int i;
        char label[12];

        /* Check for repeated blocks. */
        if (block >= bot + block_size &&
            block <= top - (block_size * 2) &&
            !memcmp(block, block - block_size, block_size) &&
            !memcmp(block, block + block_size, block_size)
            ) {
            if (block < bot + block_size * 2 ||
                memcmp(block, block - block_size * 2, block_size)
                ) {
                debug_indent(mem, indent);
                dmputs(mem, "  ...\n");
            }
            continue;
        }
        gs_sprintf(label, PRI_INTPTR":", (intptr_t)block);
        debug_indent(mem, indent);
        dmputs(mem, label);
        for (i = 0; i < block_size; ++i) {
            const char *sepr = ((i & 3) == 0 && i != 0 ? "  " : " ");

            dmputs(mem, sepr);
            if (block + i >= bot && block + i < top)
                dmprintf1(mem, "%02x", block[i]);
            else
                dmputs(mem, "  ");
        }
        dmputc(mem, '\n');
        if (as_chars) {
            debug_indent(mem, indent + strlen(label));
            for (i = 0; i < block_size; ++i) {
                byte ch;

                if ((i & 3) == 0 && i != 0)
                    dmputc(mem, ' ');
                if (block + i >= bot && block + i < top &&
                    (ch = block[i]) >= 32 && ch <= 126
                    )
                    dmprintf1(mem, "  %c", ch);
                else
                    dmputs(mem, "   ");
            }
            dmputc(mem, '\n');
        }
    }
#undef block_size
}

/* Print one object with the given options. */
/* Relevant options: type_addresses, no_types, pointers, pointed_strings, */
/* contents. */
void
debug_print_object(const gs_memory_t *mem, const void *obj, const dump_control_t * control)
{
    const obj_header_t *pre = ((const obj_header_t *)obj) - 1;
    ulong size = pre_obj_contents_size(pre);
    const gs_memory_struct_type_t *type = pre->o_type;
    dump_options_t options = control->options;

    dmprintf3(mem, "  pre="PRI_INTPTR"(obj="PRI_INTPTR") size=%lu",
              (intptr_t) pre, (intptr_t) obj, size);
    switch (options & (dump_do_type_addresses | dump_do_no_types)) {
    case dump_do_type_addresses + dump_do_no_types:	/* addresses only */
        dmprintf1(mem, " type="PRI_INTPTR"", (intptr_t) type);
        break;
    case dump_do_type_addresses:	/* addresses & names */
        dmprintf2(mem, " type=%s("PRI_INTPTR")", struct_type_name_string(type),
                 (intptr_t)type);
        break;
    case 0:		/* names only */
        dmprintf1(mem, " type=%s", struct_type_name_string(type));
    case dump_do_no_types:	/* nothing */
        ;
    }
    if (options & dump_do_marks) {
        dmprintf2(mem, " smark/back=%u (0x%x)", pre->o_smark, pre->o_smark);
    }
    dmputc(mem, '\n');
    if (type == &st_free)
        return;
    if (options & dump_do_pointers) {
        struct_proc_enum_ptrs((*proc)) = type->enum_ptrs;
        uint index = 0;
        enum_ptr_t eptr;
        gs_ptr_type_t ptype;

        if (proc != gs_no_struct_enum_ptrs) {
            if (proc != 0) {
                for (; (ptype = (*proc)(mem, pre + 1, size, index, &eptr, type, NULL)) != 0;
                     ++index
                     ) {
                    const void *ptr = eptr.ptr;

                    dmprintf1(mem, "    ptr %u: ", index);
                    if (ptype == ptr_string_type || ptype == ptr_const_string_type) {
                        const gs_const_string *str = (const gs_const_string *)&eptr;
                        if (!str)
                            dmprintf(mem, "0x0");
                        else
                            dmprintf2(mem, PRI_INTPTR "(%u)", (intptr_t)str->data, str->size);
                        if (options & dump_do_pointed_strings) {
                            dmputs(mem, " =>\n");
                            if (!str)
                                dmprintf(mem, "(null)\n");
                            else
                                debug_dump_contents(mem, str->data, str->data + str->size, 6,
                                                    true);
                        } else {
                            dmputc(mem, '\n');
                        }
                    } else {
                        dmprintf1(mem, (PTR_BETWEEN(ptr, obj, (const byte *)obj + size) ?
                                  "("PRI_INTPTR")\n" : PRI_INTPTR "\n"), (intptr_t) ptr);
                    }
                }
            } else { /* proc == 0 */
                dmprintf(mem, "previous line should be a ref\n");
            }
        } /* proc != gs_no_struct_enum_ptrs */
    }
    if (options & dump_do_contents) {
        debug_dump_contents(mem, (const byte *)obj, (const byte *)obj + size,
                            0, false);
    }
}

/* Print the contents of a clump with the given options. */
/* Relevant options: all. */
void
debug_dump_clump(const gs_memory_t *mem, const clump_t * cp, const dump_control_t * control)
{
    dmprintf1(mem, "clump at "PRI_INTPTR":\n", (intptr_t) cp);
    dmprintf3(mem, "   chead="PRI_INTPTR"  cbase="PRI_INTPTR" sbase="PRI_INTPTR"\n",
              (intptr_t)cp->chead, (intptr_t)cp->cbase, (intptr_t)cp->sbase);
    dmprintf3(mem, "    rcur="PRI_INTPTR"   rtop="PRI_INTPTR"  cbot="PRI_INTPTR"\n",
              (intptr_t)cp->rcur, (intptr_t)cp->rtop, (intptr_t)cp->cbot);
    dmprintf4(mem, "    ctop="PRI_INTPTR" climit="PRI_INTPTR" smark="PRI_INTPTR", size=%u\n",
              (intptr_t)cp->ctop, (intptr_t)cp->climit, (intptr_t)cp->smark,
              cp->smark_size);
    dmprintf2(mem, "  sreloc="PRI_INTPTR"   cend="PRI_INTPTR"\n",
              (intptr_t)cp->sreloc, (intptr_t)cp->cend);
    dmprintf6(mem, "left="PRI_INTPTR" right="PRI_INTPTR" parent="PRI_INTPTR" outer="PRI_INTPTR" inner_count=%u has_refs=%s\n",
              (intptr_t)cp->left, (intptr_t)cp->right, (intptr_t)cp->parent, (intptr_t)cp->outer,
              cp->inner_count, (cp->has_refs ? "true" : "false"));

    dmprintf2(mem, "  sfree1="PRI_INTPTR"   sfree="PRI_INTPTR"\n",
              (intptr_t)cp->sfree1, (intptr_t)cp->sfree);
    if (control->options & dump_do_strings) {
        debug_dump_contents(mem, (control->bottom == 0 ? cp->ctop :
                             max(control->bottom, cp->ctop)),
                            (control->top == 0 ? cp->climit :
                             min(control->top, cp->climit)),
                            0, true);
    }
    SCAN_CLUMP_OBJECTS(cp)
        DO_ALL
        if (obj_in_control_region(pre + 1,
                                  (const byte *)(pre + 1) + size,
                                  control)
        )
        debug_print_object(mem, pre + 1, control);
    END_OBJECTS_SCAN_NO_ABORT
}
void
debug_print_clump(const gs_memory_t *mem, const clump_t * cp)
{
    dump_control_t control;

    control = dump_control_default;
    debug_dump_clump(mem, cp, &control);
}

/* Print the contents of all clumps managed by an allocator. */
/* Relevant options: all. */
void
debug_dump_memory(const gs_ref_memory_t * mem, const dump_control_t * control)
{
    const clump_t *cp;
    clump_splay_walker sw;

    for (cp = clump_splay_walk_init(&sw, mem); cp != NULL; cp = clump_splay_walk_fwd(&sw))
    {
        if (obj_in_control_region(cp->cbase, cp->cend, control))
            debug_dump_clump((const gs_memory_t *)mem, cp, control);
    }
}

void
debug_dump_allocator(const gs_ref_memory_t *mem)
{
    debug_dump_memory(mem, &dump_control_no_contents);
}

/* Find all the objects that contain a given pointer. */
void
debug_find_pointers(const gs_ref_memory_t *mem, const void *target)
{
    clump_splay_walker sw;
    dump_control_t control;
    const clump_t *cp;

    control.options = 0;
    for (cp = clump_splay_walk_init(&sw, mem); cp; cp = clump_splay_walk_fwd(&sw))
    {
        SCAN_CLUMP_OBJECTS(cp);
        DO_ALL
            struct_proc_enum_ptrs((*proc)) = pre->o_type->enum_ptrs;
            uint index = 0;
            enum_ptr_t eptr;

            if (proc)		/* doesn't trace refs NB fix me. */
                for (; (*proc)((const gs_memory_t *)mem, pre + 1, size, index,
                               &eptr, pre->o_type, NULL);
                     ++index)
                    if (eptr.ptr == target) {
                        dmprintf1((const gs_memory_t *)mem, "Index %d in", index);
                        debug_print_object((const gs_memory_t *)mem, pre + 1, &control);
                    }
        END_OBJECTS_SCAN_NO_ABORT
    }
}
static void ddct(const gs_memory_t *mem, clump_t *cp, clump_t *parent, int depth)
{
    int i;

    if (cp == NULL)
        return;
    for (i = 0; i < depth; i++)
        dmlprintf(mem, " ");

    dmlprintf7(mem, "Clump "PRI_INTPTR":"PRI_INTPTR" parent="PRI_INTPTR" left="PRI_INTPTR":"PRI_INTPTR" right="PRI_INTPTR":"PRI_INTPTR"\n",
        (intptr_t)cp, (intptr_t)cp->cbase, (intptr_t)cp->parent,
        (intptr_t)cp->left, (intptr_t)(cp->left ? cp->left->cbase : NULL),
        (intptr_t)cp->right, (intptr_t)(cp->right ? cp->right->cbase : NULL));
    if (cp->parent != parent)
        dmlprintf(mem, "Parent pointer mismatch!\n");
    ddct(mem, cp->left, cp, depth+1);
    ddct(mem, cp->right, cp, depth+1);
}
void
debug_dump_clump_tree(const gs_ref_memory_t *mem)
{
    ddct((const gs_memory_t *)mem, mem->root, NULL, 0);
}

#endif /* DEBUG */
