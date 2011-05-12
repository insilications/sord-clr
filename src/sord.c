/*
  Copyright 2011 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

// C99
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GLib
#include <glib.h>

#include "sord-config.h"
#include "sord_internal.h"

#define SORD_LOG(prefix, ...) fprintf(stderr, "[Sord::" prefix "] " __VA_ARGS__)

#ifdef SORD_DEBUG_ITER
#    define SORD_ITER_LOG(...) SORD_LOG("iter", __VA_ARGS__)
#else
#    define SORD_ITER_LOG(...)
#endif
#ifdef SORD_DEBUG_SEARCH
#    define SORD_FIND_LOG(...) SORD_LOG("search", __VA_ARGS__)
#else
#    define SORD_FIND_LOG(...)
#endif
#ifdef SORD_DEBUG_WRITE
#    define SORD_WRITE_LOG(...) SORD_LOG("write", __VA_ARGS__)
#else
#    define SORD_WRITE_LOG(...)
#endif

#define NUM_ORDERS          12
#define STATEMENT_LEN       3
#define TUP_LEN             STATEMENT_LEN + 1
#define DEFAULT_ORDER       SPO
#define DEFAULT_GRAPH_ORDER GSPO

#define TUP_FMT         "(%s %s %s %s)"
#define TUP_FMT_ELEM(e) ((e) ? sord_node_get_string(e) : "*")
#define TUP_FMT_ARGS(t) \
	TUP_FMT_ELEM((t)[0]), \
	TUP_FMT_ELEM((t)[1]), \
	TUP_FMT_ELEM((t)[2]), \
	TUP_FMT_ELEM((t)[3])

#define TUP_S 0
#define TUP_P 1
#define TUP_O 2
#define TUP_G 3

/** Triple ordering */
typedef enum {
	SPO, ///<         Subject,   Predicate, Object
	SOP, ///<         Subject,   Object,    Predicate
	OPS, ///<         Object,    Predicate, Subject
	OSP, ///<         Object,    Subject,   Predicate
	PSO, ///<         Predicate, Subject,   Object
	POS, ///<         Predicate, Object,    Subject
	GSPO, ///< Graph,  Subject,   Predicate, Object
	GSOP, ///< Graph,  Subject,   Object,    Predicate
	GOPS, ///< Graph,  Object,    Predicate, Subject
	GOSP, ///< Graph,  Object,    Subject,   Predicate
	GPSO, ///< Graph,  Predicate, Subject,   Object
	GPOS, ///< Graph,  Predicate, Object,    Subject
} SordOrder;

/** String name of each ordering (array indexed by SordOrder) */
static const char* const order_names[NUM_ORDERS] = {
	"spo",  "sop",  "ops",  "osp",  "pso",  "pos",
	"gspo", "gsop", "gops", "gosp", "gpso", "gpos"
};

/** Quads of indices for each order, from most to least significant
 * (array indexed by SordOrder)
 */
static const int orderings[NUM_ORDERS][TUP_LEN] = {
	{  0,1,2,3}, {  0,2,1,3}, {  2,1,0,3}, {  2,0,1,3}, {  1,0,2,3}, {  1,2,0,3},
	{3,0,1,2  }, {3,0,2,1  }, {3,2,1,0  }, {3,2,0,1  }, {3,1,0,2  }, {3,1,2,0  }
};

/** World */
struct SordWorldImpl {
	GHashTable* names;    ///< URI or blank node identifier string => ID
	GHashTable* langs;    ///< Language tag => Interned language tag
	GHashTable* literals; ///< Literal => ID
	size_t      n_nodes;  ///< Number of nodes
};

/** Store */
struct SordModelImpl {
	SordWorld* world;

	/** Index for each possible triple ordering (may or may not exist).
	 * If an index for e.g. SPO exists, it is a dictionary with
	 * (S P O) keys (as a SordTuplrID), and ignored values.
	 */
	GSequence* indices[NUM_ORDERS];

	size_t n_quads;
};

/** Mode for searching or iteration */
typedef enum {
	ALL,          ///< Iterate to end of store, returning all results, no filtering
	SINGLE,       ///< Iteration over a single element (exact search)
	RANGE,        ///< Iterate over range with equal prefix
	FILTER_RANGE, ///< Iterate over range with equal prefix, filtering
	FILTER_ALL    ///< Iterate to end of store, filtering
} SearchMode;

/** Iterator over some range of a store */
struct SordIterImpl {
	const SordModel* sord;              ///< Store this is an iterator for
	GSequenceIter*   cur;               ///< Current DB cursor
	SordQuad         pat;               ///< Iteration pattern (in ordering order)
	int              ordering[TUP_LEN]; ///< Store ordering
	SearchMode       mode;              ///< Iteration mode
	int              n_prefix;          ///< Length of range prefix (RANGE, FILTER_RANGE)
	bool             end;               ///< True iff reached end
	bool             skip_graphs;       ///< True iff iteration should ignore graphs
};

static unsigned
sord_literal_hash(const void* n)
{
	SordNode* node = (SordNode*)n;
	return g_str_hash(node->buf) + (node->lang ? g_str_hash(node->lang) : 0);
}

static gboolean
sord_literal_equal(const void* a, const void* b)
{
	SordNode* a_node = (SordNode*)a;
	SordNode* b_node = (SordNode*)b;
	return (a_node == b_node)
		|| (g_str_equal(sord_node_get_string(a_node),
		                sord_node_get_string(b_node))
		    && (a_node->lang == b_node->lang)
		    && (a_node->datatype == b_node->datatype));
}

SordWorld*
sord_world_new(void)
{
	SordWorld* world = malloc(sizeof(struct SordWorldImpl));
	world->names    = g_hash_table_new_full(g_str_hash, g_str_equal, 0, 0);
	world->langs    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
	world->literals = g_hash_table_new_full(sord_literal_hash, sord_literal_equal, 0, 0);
	world->n_nodes  = 0;
	return world;
}

void
sord_world_free(SordWorld* world)
{
	g_hash_table_unref(world->names);
	g_hash_table_unref(world->langs);
	g_hash_table_unref(world->literals);
	free(world);
}

static inline int
sord_node_compare(const SordNode* a, const SordNode* b)
{
	if (a == b) {
		return 0;
	} else if (!a || !b) {
		return a - b;
	} else if (a->type != b->type) {
		return a->type - b->type;
	}

	int cmp;
	switch ((SordNodeType)a->type) {
	case SORD_URI:
	case SORD_BLANK:
		return strcmp((const char*)a->buf, (const char*)b->buf);
	case SORD_LITERAL:
		cmp = strcmp((const char*)sord_node_get_string(a),
		             (const char*)sord_node_get_string(b));
		if (cmp == 0) {
			cmp = sord_node_compare(a->datatype, b->datatype);
		}
		if (cmp == 0) {
			if (!a->lang || !b->lang) {
				cmp = a->lang - b->lang;
			} else {
				cmp = strcmp(a->lang, b->lang);
			}
		}
		return cmp;
	}
	assert(false);
	return 0;
}

bool
sord_node_equals(const SordNode* a, const SordNode* b)
{
	if (!a || !b) {
		return (a == b);
	} else {
		// FIXME: nodes are interned, this can be much faster
		return (a == b) || (sord_node_compare(a, b) == 0);
	}
}

/** Compare two IDs (dereferencing if necessary).
 * The null ID, 0, is treated as a minimum (it is less than every other
 * possible ID, except itself).  This allows it to be used as a wildcard
 * when searching, ensuring the search will reach the minimum possible
 * key in the tree and iteration from that point will produce the entire
 * result set.
 */
static inline int
sord_id_compare(SordModel* sord, const SordNode* a, const SordNode* b)
{
	if (a == b || !a || !b) {
		return (const char*)a - (const char*)b;
	} else {
		return sord_node_compare(a, b);
	}
}

/** Return true iff IDs are equivalent, or one is a wildcard */
static inline bool
sord_id_match(const SordNode* a, const SordNode* b)
{
	return !a || !b || (a == b);
}

static inline bool
sord_quad_match_inline(const SordQuad x, const SordQuad y)
{
	return sord_id_match(x[0], y[0])
		&& sord_id_match(x[1], y[1])
		&& sord_id_match(x[2], y[2])
		&& sord_id_match(x[3], y[3]);
}

bool
sord_quad_match(const SordQuad x, const SordQuad y)
{
	return sord_quad_match_inline(x, y);
}

/** Compare two quad IDs lexicographically.
 * NULL IDs (equal to 0) are treated as wildcards, always less than every
 * other possible ID, except itself.
 */
static int
sord_quad_compare(const void* x_ptr, const void* y_ptr, void* user_data)
{
	SordModel* const sord = (SordModel*)user_data;
	SordNode** const x    = (SordNode**)x_ptr;
	SordNode** const y    = (SordNode**)y_ptr;

	for (int i = 0; i < TUP_LEN; ++i) {
		const int cmp = sord_id_compare(sord, x[i], y[i]);
		if (cmp)
			return cmp;
	}

	return 0;
}

static inline bool
sord_iter_forward(SordIter* iter)
{
	if (!iter->skip_graphs) {
		iter->cur = g_sequence_iter_next(iter->cur);
		return g_sequence_iter_is_end(iter->cur);
	}

	SordNode** key = (SordNode**)g_sequence_get(iter->cur);
	const SordQuad initial = { key[0], key[1], key[2], key[3] };
	while (true) {
		iter->cur = g_sequence_iter_next(iter->cur);
		if (g_sequence_iter_is_end(iter->cur))
			return true;

		key = (SordNode**)g_sequence_get(iter->cur);
		for (int i = 0; i < 3; ++i)
			if (key[i] != initial[i])
				return false;
	}
	assert(false);
}

/** Seek forward as necessary until @a iter points at a match.
 * @return true iff iterator reached end of valid range.
 */
static inline bool
sord_iter_seek_match(SordIter* iter)
{
	for (iter->end = true;
	     !g_sequence_iter_is_end(iter->cur);
	     sord_iter_forward(iter)) {
		const SordNode** const key = (const SordNode**)g_sequence_get(iter->cur);
		if (sord_quad_match_inline(key, iter->pat))
			return (iter->end = false);
	}
	return true;
}

/** Seek forward as necessary until @a iter points at a match, or the prefix
 * no longer matches iter->pat.
 * @return true iff iterator reached end of valid range.
 */
static inline bool
sord_iter_seek_match_range(SordIter* iter)
{
	if (iter->end)
		return true;

	do {
		const SordNode** key = (const SordNode**)g_sequence_get(iter->cur);

		if (sord_quad_match_inline(key, iter->pat))
			return false; // Found match

		for (int i = 0; i < iter->n_prefix; ++i) {
			if (!sord_id_match(key[i], iter->pat[i])) {
				iter->end = true; // Reached end of valid range
				return true;
			}
		}
	} while (!sord_iter_forward(iter));

	return (iter->end = true); // Reached end
}

static SordIter*
sord_iter_new(const SordModel* sord, GSequenceIter* cur, const SordQuad pat,
              SordOrder order, SearchMode mode, int n_prefix)
{
	const int* ordering = orderings[order];

	SordIter* iter = malloc(sizeof(struct SordIterImpl));
	iter->sord        = sord;
	iter->cur         = cur;
	iter->mode        = mode;
	iter->n_prefix    = n_prefix;
	iter->end         = false;
	iter->skip_graphs = order < GSPO;
	for (int i = 0; i < TUP_LEN; ++i) {
		iter->pat[i]      = pat[ordering[i]];
		iter->ordering[i] = ordering[i];
	}

	switch (iter->mode) {
	case ALL:
	case SINGLE:
	case RANGE:
		assert(
			sord_quad_match_inline((const SordNode**)g_sequence_get(iter->cur),
			                       iter->pat));
		break;
	case FILTER_RANGE:
		sord_iter_seek_match_range(iter);
		break;
	case FILTER_ALL:
		sord_iter_seek_match(iter);
		break;
	}

#ifdef SORD_DEBUG_ITER
	SordQuad value;
	sord_iter_get(iter, value);
	SORD_ITER_LOG("New %p pat=" TUP_FMT " cur=" TUP_FMT " end=%d skipgraphs=%d\n",
	              (void*)iter, TUP_FMT_ARGS(pat), TUP_FMT_ARGS(value),
	              iter->end, iter->skip_graphs);
#endif
	return iter;
}

const SordModel*
sord_iter_get_model(SordIter* iter)
{
	return iter->sord;
}

void
sord_iter_get(const SordIter* iter, SordQuad id)
{
	SordNode** key = (SordNode**)g_sequence_get(iter->cur);
	id[iter->ordering[0]] = key[0];
	id[iter->ordering[1]] = key[1];
	id[iter->ordering[2]] = key[2];
	id[iter->ordering[3]] = key[3];
}

bool
sord_iter_next(SordIter* iter)
{
	if (iter->end)
		return true;

	const SordNode** key;
	iter->end = sord_iter_forward(iter);
	if (!iter->end) {
		switch (iter->mode) {
		case ALL:
			// At the end if the cursor is (assigned above)
			break;
		case SINGLE:
			iter->end = true;
			SORD_ITER_LOG("%p reached single end\n", (void*)iter);
			break;
		case RANGE:
			SORD_ITER_LOG("%p range next\n", (void*)iter);
			// At the end if the MSNs no longer match
			key = (const SordNode**)g_sequence_get(iter->cur);
			assert(key);
			for (int i = 0; i < iter->n_prefix; ++i) {
				if (!sord_id_match(key[i], iter->pat[i])) {
					iter->end = true;
					SORD_ITER_LOG("%p reached non-match end\n", (void*)iter);
					break;
				}
			}
			break;
		case FILTER_RANGE:
			// Seek forward to next match, stopping if prefix changes
			sord_iter_seek_match_range(iter);
			break;
		case FILTER_ALL:
			// Seek forward to next match
			sord_iter_seek_match(iter);
			break;
		}
	} else {
		SORD_ITER_LOG("%p reached index end\n", (void*)iter);
	}

	if (iter->end) {
		SORD_ITER_LOG("%p Reached end\n", (void*)iter);
		return true;
	} else {
#ifdef SORD_DEBUG_ITER
		SordQuad tup;
		sord_iter_get(iter, tup);
		SORD_ITER_LOG("%p Increment to " TUP_FMT "\n", (void*)iter, TUP_FMT_ARGS(tup));
#endif
		return false;
	}
}

bool
sord_iter_end(const SordIter* iter)
{
	return !iter || iter->end;
}

void
sord_iter_free(SordIter* iter)
{
	SORD_ITER_LOG("%p Free\n", (void*)iter);
	if (iter) {
		free(iter);
	}
}

/** Return true iff @a sord has an index for @a order.
 * If @a graph_search is true, @a order will be modified to be the
 * corresponding order with a G prepended (so G will be the MSN).
 */
static inline bool
sord_has_index(SordModel* sord, SordOrder* order, int* n_prefix, bool graph_search)
{
	if (graph_search) {
		*order    += GSPO;
		*n_prefix += 1;
	}

	return sord->indices[*order];
}

/** Return the best available index for a pattern.
 * @param pat Pattern in standard (S P O G) order
 * @param mode Set to the (best) iteration mode for iterating over results
 * @param n_prefix Set to the length of the range prefix
 *        (for @a mode == RANGE and @a mode == FILTER_RANGE)
 */
static inline SordOrder
sord_best_index(SordModel* sord, const SordQuad pat, SearchMode* mode, int* n_prefix)
{
	const bool graph_search = (pat[TUP_G] != 0);

	const unsigned sig
		= (pat[0] ? 1 : 0) * 0x100
		+ (pat[1] ? 1 : 0) * 0x010
		+ (pat[2] ? 1 : 0) * 0x001;

	SordOrder good[2];

	// Good orderings that don't require filtering
	*mode     = RANGE;
	*n_prefix = 0;
	switch (sig) {
	case 0x000: *mode = ALL; return graph_search ? DEFAULT_GRAPH_ORDER : DEFAULT_ORDER;
	case 0x001: *mode = RANGE; good[0] = OPS; good[1] = OSP; *n_prefix = 1; break;
	case 0x010: *mode = RANGE; good[0] = POS; good[1] = PSO; *n_prefix = 1; break;
	case 0x011: *mode = RANGE; good[0] = OPS; good[1] = POS; *n_prefix = 2; break;
	case 0x100: *mode = RANGE; good[0] = SPO; good[1] = SOP; *n_prefix = 1; break;
	case 0x101: *mode = RANGE; good[0] = SOP; good[1] = OSP; *n_prefix = 2; break;
	case 0x110: *mode = RANGE; good[0] = SPO; good[1] = PSO; *n_prefix = 2; break;
	case 0x111: *mode = SINGLE; return graph_search ? DEFAULT_GRAPH_ORDER : DEFAULT_ORDER;
	}

	if (sord_has_index(sord, &good[0], n_prefix, graph_search)) {
		return good[0];
	} else if (sord_has_index(sord, &good[1], n_prefix, graph_search)) {
		return good[1];
	}

	// Not so good orderings that require filtering, but can
	// still be constrained to a range
	switch (sig) {
	case 0x011: *mode = FILTER_RANGE; good[0] = OSP; good[1] = PSO; *n_prefix = 1; break;
	case 0x101: *mode = FILTER_RANGE; good[0] = SPO; good[1] = OPS; *n_prefix = 1; break;
	case 0x110: *mode = FILTER_RANGE; good[0] = SOP; good[1] = POS; *n_prefix = 1; break;
	default: break;
	}

	if (*mode == FILTER_RANGE) {
		if (sord_has_index(sord, &good[0], n_prefix, graph_search)) {
			return good[0];
		} else if (sord_has_index(sord, &good[1], n_prefix, graph_search)) {
			return good[1];
		}
	}

	if (graph_search) {
		*mode = FILTER_RANGE;
		*n_prefix = 1;
		return DEFAULT_GRAPH_ORDER;
	} else {
		*mode = FILTER_ALL;
		return DEFAULT_ORDER;
	}
}

SordModel*
sord_new(SordWorld* world, unsigned indices, bool graphs)
{
	SordModel* sord = (SordModel*)malloc(sizeof(struct SordModelImpl));
	sord->world   = world;
	sord->n_quads = 0;

	for (unsigned i = 0; i < (NUM_ORDERS / 2); ++i) {
		if (indices & (1 << i)) {
			sord->indices[i] = g_sequence_new(free);
			if (graphs) {
				sord->indices[i + (NUM_ORDERS / 2)] = g_sequence_new(free);
			} else {
				sord->indices[i + (NUM_ORDERS / 2)] = NULL;
			}
		} else {
			sord->indices[i] = NULL;
			sord->indices[i + (NUM_ORDERS / 2)] = NULL;
		}
	}

	if (!sord->indices[DEFAULT_ORDER]) {
		sord->indices[DEFAULT_ORDER] = g_sequence_new(free);
	}

	return sord;
}

static void
sord_node_free_internal(SordWorld* world, SordNode* node)
{
	assert(node->refs == 0);
	if (node->type == SORD_LITERAL) {
		if (!g_hash_table_remove(world->literals, node)) {
			fprintf(stderr, "Failed to remove literal from hash.\n");
			return;
		}
		sord_node_free(world, node->datatype);
	} else {
		if (!g_hash_table_remove(world->names, node->buf)) {
			fprintf(stderr, "Failed to remove resource from hash.\n");
			return;
		}
	}
	g_free(node->buf);
	free(node);
}

static void
sord_add_quad_ref(SordModel* sord, const SordNode* node, SordQuadIndex i)
{
	if (node) {
		assert(node->refs > 0);
		++((SordNode*)node)->refs;
		if (i == SORD_OBJECT) {
			++((SordNode*)node)->refs_as_obj;
		}
	}
}

static void
sord_drop_quad_ref(SordModel* sord, const SordNode* node, SordQuadIndex i)
{
	if (!node) {
		return;
	}

	assert(node->refs > 0);
	if (i == SORD_OBJECT) {
		assert(node->refs_as_obj > 0);
		--((SordNode*)node)->refs_as_obj;
	}
	if (--((SordNode*)node)->refs == 0) {
		sord_node_free_internal(sord_get_world(sord), (SordNode*)node);
	}
}

void
sord_free(SordModel* sord)
{
	if (!sord)
		return;

	// Free nodes
	SordQuad tup;
	SordIter* i = sord_begin(sord);
	for (; !sord_iter_end(i); sord_iter_next(i)) {
		sord_iter_get(i, tup);
		for (int i = 0; i < TUP_LEN; ++i) {
			sord_drop_quad_ref(sord, (SordNode*)tup[i], i);
		}
	}
	sord_iter_free(i);

	for (unsigned i = 0; i < NUM_ORDERS; ++i)
		if (sord->indices[i])
			g_sequence_free(sord->indices[i]);

	free(sord);
}

SordWorld*
sord_get_world(SordModel* sord)
{
	return sord->world;
}

size_t
sord_num_quads(const SordModel* sord)
{
	return sord->n_quads;
}

size_t
sord_num_nodes(const SordWorld* world)
{
	return world->n_nodes;
}

SordIter*
sord_begin(const SordModel* sord)
{
	if (sord_num_quads(sord) == 0) {
		return NULL;
	} else {
		GSequenceIter* cur = g_sequence_get_begin_iter(sord->indices[DEFAULT_ORDER]);
		SordQuad pat = { 0, 0, 0, 0 };
		return sord_iter_new(sord, cur, pat, DEFAULT_ORDER, ALL, 0);
	}
}

static inline GSequenceIter*
index_search(SordModel* sord, GSequence* db, const SordQuad search_key)
{
	return g_sequence_search(
		db, (void*)search_key, sord_quad_compare, sord);
}

static inline GSequenceIter*
index_lower_bound_iter(SordModel* sord, GSequenceIter* i, const SordQuad search_key)
{
	/* i is now at the position where search_key would be inserted,
	   but this is not necessarily a match, and we need the leftmost...
	*/

	if (g_sequence_iter_is_begin(i)) {
		return i;
	} else if (g_sequence_iter_is_end(i)) {
		i = g_sequence_iter_prev(i);
	}

	if (!sord_quad_match_inline(search_key, g_sequence_get(i))) {
		// No match, but perhaps immediately after a match
		GSequenceIter* const prev = g_sequence_iter_prev(i);
		if (!sord_quad_match_inline(search_key, g_sequence_get(prev))) {
			return i; // No match (caller must check)
		} else {
			i = prev;
		}
	}

	/* i now points to some matching element,
	   but not necessarily the first...
	*/
	assert(sord_quad_match_inline(search_key, g_sequence_get(i)));

	while (!g_sequence_iter_is_begin(i)) {
		if (sord_quad_match_inline(search_key, g_sequence_get(i))) {
			GSequenceIter* const prev = g_sequence_iter_prev(i);
			if (sord_quad_match_inline(search_key, g_sequence_get(prev))) {
				i = prev;
				continue;
			}
		}
		break;
	}

	return i;
}

static inline GSequenceIter*
index_lower_bound(SordModel* sord, GSequence* db, const SordQuad search_key)
{
	GSequenceIter* i = g_sequence_search(
		db, (void*)search_key, sord_quad_compare, sord);
	return index_lower_bound_iter(sord, i, search_key);
}

SordIter*
sord_find(SordModel* sord, const SordQuad pat)
{
	if (!pat[0] && !pat[1] && !pat[2] && !pat[3])
		return sord_begin(sord);

	SearchMode          mode;
	int                 prefix_len;
	const SordOrder     index_order = sord_best_index(sord, pat, &mode, &prefix_len);
	const int* const    ordering    = orderings[index_order];

	SORD_FIND_LOG("Find " TUP_FMT "  index=%s  mode=%d  prefix_len=%d ordering=%d%d%d%d\n",
	              TUP_FMT_ARGS(pat), order_names[index_order], mode, prefix_len,
	              ordering[0], ordering[1], ordering[2], ordering[3]);

	// It's easiest to think about this algorithm in terms of (S P O) ordering,
	// assuming (A B C) == (S P O).  For other orderings this is not actually
	// the case, but it works the same way.
	const SordNode* a = pat[ordering[0]]; // Most Significant Node (MSN)
	const SordNode* b = pat[ordering[1]]; // ...
	const SordNode* c = pat[ordering[2]]; // ...
	const SordNode* d = pat[ordering[3]]; // Least Significant Node (LSN)

	if (a && b && c && d)
		mode = SINGLE; // No duplicate quads (Sord is a set)

	SordQuad             search_key = { a, b, c, d };
	GSequence* const     db         = sord->indices[index_order];
	GSequenceIter* const cur        = index_lower_bound(sord, db, search_key);
	if (g_sequence_iter_is_end(cur)) {
		SORD_FIND_LOG("No match found\n");
		return NULL;
	}
	const SordNode** const key = (const SordNode**)g_sequence_get(cur);
	if (!key || ( (mode == RANGE || mode == SINGLE)
	              && !sord_quad_match_inline(search_key, key) )) {
		SORD_FIND_LOG("No match found\n");
		return NULL;
	}

	return sord_iter_new(sord, cur, pat, index_order, mode, prefix_len);
}

static SordNode*
sord_lookup_name(SordWorld* world, const uint8_t* str, size_t str_len)
{
	return g_hash_table_lookup(world->names, str);
}

static SordNode*
sord_new_node(SordNodeType type, const uint8_t* data,
              size_t n_bytes, SerdNodeFlags flags)
{
	SordNode* node = malloc(sizeof(struct SordNodeImpl));
	node->type        = type;
	node->n_bytes     = n_bytes;
	node->refs        = 1;
	node->refs_as_obj = 0;
	node->datatype    = 0;
	node->lang        = 0;
	node->flags       = flags;
	node->buf         = (uint8_t*)g_strdup((const char*)data); // TODO: no-copy
	return node;
}

const char*
sord_intern_lang(SordWorld* world, const char* lang)
{
	if (lang) {
		char* ilang = g_hash_table_lookup(world->langs, lang);
		if (ilang) {
			lang = ilang;
		} else {
			ilang = g_strdup(lang);
			g_hash_table_insert(world->langs, ilang, ilang);
		}
	}
	return lang;
}

static SordNode*
sord_new_literal_node(SordWorld* world, SordNode* datatype,
                      const uint8_t* str,  size_t str_len, SerdNodeFlags flags,
                      const char*    lang)
{
	SordNode* node = sord_new_node(SORD_LITERAL, str, str_len + 1, flags);
	node->datatype = sord_node_copy(datatype);
	node->lang     = sord_intern_lang(world, lang);
	return node;
}

static SordNode*
sord_lookup_literal(SordWorld* world, SordNode* type,
                    const uint8_t* str, size_t str_len,
                    const char*    lang)
{
	// Make search key (FIXME: ick)
	struct SordNodeImpl key;
	key.type     = SORD_LITERAL;
	key.n_bytes  = str_len;
	key.refs     = 1;
	key.datatype = type;
	key.lang     = sord_intern_lang(world, lang);
	key.buf      = (uint8_t*)str;
	key.flags    = 0;

	SordNode* id = g_hash_table_lookup(world->literals, &key);
	if (id) {
		return id;
	} else {
		return 0;
	}
}

SordNodeType
sord_node_get_type(const SordNode* ref)
{
	return ref->type;
}

const uint8_t*
sord_node_get_string(const SordNode* ref)
{
	return (const uint8_t*)ref->buf;
}

const uint8_t*
sord_node_get_string_counted(const SordNode* ref, size_t* n_bytes)
{
	*n_bytes = ref->n_bytes;
	return ref->buf;
}

const char*
sord_node_get_language(const SordNode* ref)
{
	return ref->lang;
}

SordNode*
sord_node_get_datatype(const SordNode* ref)
{
	return ref->datatype;
}

SerdNodeFlags
sord_node_get_flags(const SordNode* node)
{
	return node->flags;
}

bool
sord_node_is_inline_object(const SordNode* node)
{
	return (node->type == SORD_BLANK) && (node->refs_as_obj == 1);
}

static void
sord_add_node(SordWorld* world, SordNode* node)
{
	++world->n_nodes;
}

SordNode*
sord_new_uri_counted(SordWorld* world, const uint8_t* str, size_t str_len)
{
	SordNode* node = sord_lookup_name(world, str, str_len);
	if (node) {
		++node->refs;
		return node;
	}

	node = sord_new_node(SORD_URI, str, str_len + 1, 0);
	assert(!g_hash_table_lookup(world->names, node->buf));
	g_hash_table_insert(world->names, node->buf, node);
	sord_add_node(world, node);
	return node;
}

SordNode*
sord_new_uri(SordWorld* world, const uint8_t* str)
{
	return sord_new_uri_counted(world, str, strlen((const char*)str));
}

SordNode*
sord_new_blank_counted(SordWorld* world, const uint8_t* str, size_t str_len)
{
	SordNode* node = sord_lookup_name(world, str, str_len);
	if (node) {
		++node->refs;
		return node;
	}

	node = sord_new_node(SORD_BLANK, str, str_len + 1, 0);
	g_hash_table_insert(world->names, node->buf, node);
	sord_add_node(world, node);
	return node;
}

SordNode*
sord_new_blank(SordWorld* world, const uint8_t* str)
{
	return sord_new_blank_counted(world, str, strlen((const char*)str));
}

SordNode*
sord_new_literal_counted(SordWorld* world, SordNode* datatype,
                         const uint8_t* str,  size_t  str_len, SerdNodeFlags flags,
                         const char*    lang)
{
	SordNode* node = sord_lookup_literal(world, datatype, str, str_len, lang);
	if (node) {
		++node->refs;
		return node;
	}

	node = sord_new_literal_node(world, datatype, str, str_len, flags, lang);
	g_hash_table_insert(world->literals, node, node);  // FIXME: correct?
	sord_add_node(world, node);
	assert(node->refs == 1);
	return node;
}

SordNode*
sord_new_literal(SordWorld* world, SordNode* datatype,
                 const uint8_t* str, const char* lang)
{
	SerdNodeFlags flags   = 0;
	size_t        n_bytes = 0;
	size_t        n_chars = serd_strlen(str, &n_bytes, &flags);
	return sord_new_literal_counted(world, datatype,
	                                str, n_bytes - 1, flags,
	                                lang);
}

void
sord_node_free(SordWorld* world, SordNode* node)
{
	if (!node) {
		return;
	}

	assert(node->refs > 0);
	if (--node->refs == 0) {
		sord_node_free_internal(world, node);
	}
}

SordNode*
sord_node_copy(const SordNode* node)
{
	SordNode* copy = (SordNode*)node;
	if (copy) {
		++copy->refs;
	}
	return copy;
}

static inline bool
sord_add_to_index(SordModel* sord, const SordQuad tup, SordOrder order)
{
	assert(sord->indices[order]);
	const int* const ordering = orderings[order];
	const SordQuad key = {
		tup[ordering[0]], tup[ordering[1]], tup[ordering[2]], tup[ordering[3]]
	};
	GSequenceIter* const cur   = index_search(sord, sord->indices[order], key);
	GSequenceIter* const match = index_lower_bound_iter(sord, cur, key);
	if (!g_sequence_iter_is_end(match)
	    && !sord_quad_compare(g_sequence_get(match), key, sord)) {
		return false;  // Quad already stored in this index
	}

	// FIXME: would be nice to share quads and just use a different comparator
	// for each index (save significant space overhead per quad)
	SordNode** key_copy = malloc(sizeof(SordQuad));
	memcpy(key_copy, key, sizeof(SordQuad));
	g_sequence_insert_before(cur, key_copy);
	return true;
}

bool
sord_add(SordModel* sord, const SordQuad tup)
{
	SORD_WRITE_LOG("Add " TUP_FMT "\n", TUP_FMT_ARGS(tup));
	if (!tup[0] || !tup[1] || !tup[2]) {
		fprintf(stderr, "Attempt to add quad with NULL field.\n");
		return false;
	}

	for (unsigned i = 0; i < NUM_ORDERS; ++i) {
		if (sord->indices[i]) {
			if (!sord_add_to_index(sord, tup, i)) {
				assert(i == 0); // Assuming index coherency
				return false; // Quad already stored, do nothing
			}
		}
	}

	for (SordQuadIndex i = 0; i < TUP_LEN; ++i)
		sord_add_quad_ref(sord, tup[i], i);

	++sord->n_quads;
	assert(sord->n_quads == (size_t)g_sequence_get_length(sord->indices[SPO]));
	return true;
}

void
sord_remove(SordModel* sord, const SordQuad tup)
{
	SORD_WRITE_LOG("Remove " TUP_FMT "\n", TUP_FMT_ARGS(tup));

	for (unsigned i = 0; i < NUM_ORDERS; ++i) {
		if (sord->indices[i]) {
			const int* const ordering = orderings[i];
			const SordQuad key = {
				tup[ordering[0]], tup[ordering[1]], tup[ordering[2]], tup[ordering[3]]
			};
			GSequenceIter* const cur = index_search(sord, sord->indices[i], key);
			if (!g_sequence_iter_is_end(cur)) {
				g_sequence_remove(cur);
			} else {
				assert(i == 0); // Assuming index coherency
				return; // Quad not found, do nothing
			}
		}
	}

	for (SordQuadIndex i = 0; i < TUP_LEN; ++i)
		sord_drop_quad_ref(sord, tup[i], i);

	--sord->n_quads;
}
