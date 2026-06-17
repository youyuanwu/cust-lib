/* std::list — intrusive doubly-linked list (kernel `list_head` shape).
 *
 * Purely structural — owns nothing. `std_list_head` is the
 * prev/next wiring; the surrounding object is the caller's.
 *
 * Ownership contract:
 *   - DOES NOT allocate. No `_new_in` constructor, no allocator
 *     field. The list never sees a `std_alloc`.
 *   - DOES NOT free. `_del` unwires a node from the chain; the
 *     caller owns the surrounding object's lifetime.
 *   - DOES NOT iterate destructively. Walking the list to free
 *     every element is a caller-side loop.
 *   - DOES NOT interact with refcounts. If list membership
 *     should imply a `std_rc` / `std_arc` ref, wrap `_add` /
 *     `_del` in helpers that bump / drop the count.
 *
 * Consumer pattern: embed `struct std_list_head` as a field in
 * your own type and recover the owning struct via offsetof,
 * exactly like `std_rc` / `std_arc`.
 *
 *   struct task {
 *       struct std_list_head link;
 *       i32                  priority;
 *       ... payload ...
 *   };
 *
 *   struct std_list_head ready_queue;
 *   std_list_init(&ready_queue);
 *
 *   struct task *t = my_task_new_in(alloc);     // caller's alloc
 *   std_list_add_tail(&t->link, &ready_queue);  // O(1), no alloc
 *
 *   // walk
 *   for (struct std_list_head *p = ready_queue.next;
 *        p != &ready_queue;
 *        p = p->next) {
 *       struct task *cur = (struct task *)
 *           ((u8 *)p - offsetof(struct task, link));
 *       ...
 *   }
 *
 *   std_list_del(&t->link);                     // unlink, no free
 *   my_task_free(t);                            // caller's free
 *
 * The sentinel-points-at-itself empty-list representation comes
 * straight from `<linux/list.h>` and lets `_add` / `_del` avoid
 * any branching on "is the head empty" — every node always has
 * a valid prev/next, the head's prev/next pointing back at the
 * head is just the natural base case.
 *
 * An element can live in multiple lists by holding multiple
 * `std_list_head` fields. Each list operates on its own field
 * independently; the kernel relies on this constantly.
 */

#cust use crate::types;

struct [[cust::pub_repr]] std_list_head {
    struct std_list_head *next;
    struct std_list_head *prev;
};

/* Initialise as an empty list — both pointers reference the
 * head itself. This is the sentinel invariant the rest of the
 * API depends on; ALL `std_list_head` values must be init'd
 * before use (no implicit-zero shortcut, because zero is not
 * a self-reference). */
[[cust::pub]] void std_list_init(struct std_list_head *h) {
    h->next = h;
    h->prev = h;
}

[[cust::pub]] bool std_list_is_empty(const struct std_list_head *h) {
    return h->next == h;
}

/* Internal: splice `new_` between `prev` and `next`, both of
 * which must already be valid (non-null and connected). Not
 * exported — callers use `_add` / `_add_tail`. */
static void list_link(struct std_list_head *new_,
                      struct std_list_head *prev,
                      struct std_list_head *next) {
    new_->next = next;
    new_->prev = prev;
    next->prev = new_;
    prev->next = new_;
}

/* Insert `new_` AFTER `head`. When `head` is the list anchor
 * this is push-front. Precondition: `new_` must not currently
 * be in any list (caller's responsibility). */
[[cust::pub]] void std_list_add(struct std_list_head *new_,
                                struct std_list_head *head) {
    list_link(new_, head, head->next);
}

/* Insert `new_` BEFORE `head`. When `head` is the list anchor
 * this is push-back. Precondition: same as `_add`. */
[[cust::pub]] void std_list_add_tail(struct std_list_head *new_,
                                     struct std_list_head *head) {
    list_link(new_, head->prev, head);
}

/* Unlink `node` from whatever list it's currently in.
 * Precondition: `node` must currently be linked (i.e. was
 * `_add`/`_add_tail`-ed and not since `_del`'d). After this
 * call, `node->next` / `node->prev` are NULL — touching them
 * (e.g. iterating off a deleted node) trips a null deref
 * instantly rather than wandering into freed memory.
 *
 * Use `std_list_del_init` instead if you want to re-add the
 * node to a list later without an explicit re-init. */
[[cust::pub]] void std_list_del(struct std_list_head *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = (struct std_list_head *)0;
    node->prev = (struct std_list_head *)0;
}

/* Like `_del` but leaves the node in the empty-list state
 * (self-referential), so it can be re-added without a fresh
 * `_init`. Mirrors the kernel's `list_del_init`. */
[[cust::pub]] void std_list_del_init(struct std_list_head *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    std_list_init(node);
}

/* ─── unit tests ────────────────────────────────────────── */

#include <stddef.h>   /* offsetof for the consumer recovery */

/* A trivial consumer type — `i32` payload with one list link. */
struct list_item {
    struct std_list_head link;
    i32                  value;
};

/* Helper: recover the item from a link pointer. Each consumer
 * writes one of these for its own type. */
static struct list_item *to_item(struct std_list_head *p) {
    return (struct list_item *)((u8 *)p - offsetof(struct list_item, link));
}

[[cust::test]] int test_list_empty_after_init(void) {
    struct std_list_head head;
    std_list_init(&head);
    cust_assert(std_list_is_empty(&head));
    /* Sentinel invariant: empty list points at itself. */
    cust_assert(head.next == &head);
    cust_assert(head.prev == &head);
    return 0;
}

[[cust::test]] int test_list_add_push_front_order(void) {
    struct std_list_head head;
    std_list_init(&head);

    struct list_item a = { .value = 1 };
    struct list_item b = { .value = 2 };
    struct list_item c = { .value = 3 };

    /* `_add` is push-front: c, b, a after three adds. */
    std_list_add(&a.link, &head);
    std_list_add(&b.link, &head);
    std_list_add(&c.link, &head);

    cust_assert(!std_list_is_empty(&head));

    /* Walk forward — expect 3, 2, 1. */
    i32 expected[3] = { 3, 2, 1 };
    usize i = 0;
    for (struct std_list_head *p = head.next; p != &head; p = p->next) {
        cust_assert_eq(to_item(p)->value, expected[i]);
        i++;
    }
    cust_assert_eq(i, (usize)3);
    return 0;
}

[[cust::test]] int test_list_add_tail_push_back_order(void) {
    struct std_list_head head;
    std_list_init(&head);

    struct list_item a = { .value = 1 };
    struct list_item b = { .value = 2 };
    struct list_item c = { .value = 3 };

    std_list_add_tail(&a.link, &head);
    std_list_add_tail(&b.link, &head);
    std_list_add_tail(&c.link, &head);

    /* Walk forward — expect 1, 2, 3. */
    i32 expected[3] = { 1, 2, 3 };
    usize i = 0;
    for (struct std_list_head *p = head.next; p != &head; p = p->next) {
        cust_assert_eq(to_item(p)->value, expected[i]);
        i++;
    }
    cust_assert_eq(i, (usize)3);

    /* And backward through prev — expect 3, 2, 1. */
    i32 expected_rev[3] = { 3, 2, 1 };
    i = 0;
    for (struct std_list_head *p = head.prev; p != &head; p = p->prev) {
        cust_assert_eq(to_item(p)->value, expected_rev[i]);
        i++;
    }
    cust_assert_eq(i, (usize)3);
    return 0;
}

[[cust::test]] int test_list_del_unlinks_and_poisons(void) {
    struct std_list_head head;
    std_list_init(&head);

    struct list_item a = { .value = 1 };
    struct list_item b = { .value = 2 };
    struct list_item c = { .value = 3 };
    std_list_add_tail(&a.link, &head);
    std_list_add_tail(&b.link, &head);
    std_list_add_tail(&c.link, &head);

    /* Pull out the middle. Expect 1, 3 remaining; b's pointers
     * poisoned to NULL. */
    std_list_del(&b.link);
    cust_assert(b.link.next == (void *)0);
    cust_assert(b.link.prev == (void *)0);

    i32 expected[2] = { 1, 3 };
    usize i = 0;
    for (struct std_list_head *p = head.next; p != &head; p = p->next) {
        cust_assert_eq(to_item(p)->value, expected[i]);
        i++;
    }
    cust_assert_eq(i, (usize)2);
    cust_assert(!std_list_is_empty(&head));

    /* Remove the rest; head must report empty again. */
    std_list_del(&a.link);
    std_list_del(&c.link);
    cust_assert(std_list_is_empty(&head));
    return 0;
}

[[cust::test]] int test_list_del_init_allows_readd(void) {
    struct std_list_head head_a;
    struct std_list_head head_b;
    std_list_init(&head_a);
    std_list_init(&head_b);

    struct list_item x = { .value = 42 };
    std_list_add_tail(&x.link, &head_a);

    /* Move x from list A to list B via del_init + add. The
     * del_init leaves x in a valid empty-list state, so the
     * subsequent _add doesn't trip the "must not be linked"
     * precondition. */
    std_list_del_init(&x.link);
    cust_assert(std_list_is_empty(&head_a));
    /* x is its own degenerate empty list right now. */
    cust_assert(x.link.next == &x.link);
    cust_assert(x.link.prev == &x.link);

    std_list_add_tail(&x.link, &head_b);
    cust_assert(!std_list_is_empty(&head_b));
    cust_assert(head_b.next == &x.link);
    cust_assert_eq(to_item(head_b.next)->value, (i32)42);
    return 0;
}

/* An element can sit in TWO lists at once by carrying two
 * `std_list_head` fields. This is the composition property
 * the kernel relies on. */
struct dual_item {
    struct std_list_head ready_link;
    struct std_list_head dirty_link;
    i32                  value;
};

static struct dual_item *to_dual_ready(struct std_list_head *p) {
    return (struct dual_item *)((u8 *)p - offsetof(struct dual_item, ready_link));
}
static struct dual_item *to_dual_dirty(struct std_list_head *p) {
    return (struct dual_item *)((u8 *)p - offsetof(struct dual_item, dirty_link));
}

[[cust::test]] int test_list_element_in_two_lists(void) {
    struct std_list_head ready;
    struct std_list_head dirty;
    std_list_init(&ready);
    std_list_init(&dirty);

    struct dual_item a = { .value = 1 };
    struct dual_item b = { .value = 2 };

    std_list_add_tail(&a.ready_link, &ready);
    std_list_add_tail(&b.ready_link, &ready);
    std_list_add_tail(&b.dirty_link, &dirty);   /* b is in BOTH */

    /* ready: [a, b] */
    cust_assert_eq(to_dual_ready(ready.next)->value, (i32)1);
    cust_assert_eq(to_dual_ready(ready.next->next)->value, (i32)2);
    /* dirty: [b] */
    cust_assert_eq(to_dual_dirty(dirty.next)->value, (i32)2);
    cust_assert(dirty.next->next == &dirty);

    /* Unlinking from ready leaves dirty membership intact —
     * the two list_head fields are independent. */
    std_list_del(&b.ready_link);
    cust_assert(!std_list_is_empty(&dirty));
    cust_assert_eq(to_dual_dirty(dirty.next)->value, (i32)2);
    return 0;
}
