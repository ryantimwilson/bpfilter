/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 */

#include "bpfilter/cgen/cgen.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "bpfilter/cgen/dump.h"
#include "bpfilter/cgen/prog/link.h"
#include "bpfilter/cgen/program.h"
#include "bpfilter/ctx.h"
#include "bpfilter/opts.h"
#include "core/chain.h"
#include "core/counter.h"
#include "core/dump.h"
#include "core/front.h"
#include "core/helper.h"
#include "core/hook.h"
#include "core/list.h"
#include "core/logger.h"
#include "core/marsh.h"
#include "core/ns.h"
#include "core/rule.h"

int bf_cgen_new(struct bf_cgen **cgen, enum bf_front front,
                struct bf_chain **chain)
{
    bf_assert(cgen && chain && *chain);

    *cgen = malloc(sizeof(struct bf_cgen));
    if (!*cgen)
        return -ENOMEM;

    (*cgen)->front = front;
    (*cgen)->program = NULL;
    (*cgen)->chain = TAKE_PTR(*chain);

    return 0;
}

int bf_cgen_new_from_marsh(struct bf_cgen **cgen, const struct bf_marsh *marsh)
{
    _cleanup_bf_cgen_ struct bf_cgen *_cgen = NULL;
    _cleanup_bf_program_ struct bf_program *program = NULL;
    _cleanup_bf_chain_ struct bf_chain *chain = NULL;
    struct bf_marsh *marsh_elem = NULL;
    enum bf_front front;
    int r;

    bf_assert(cgen);
    bf_assert(marsh);

    if (!(marsh_elem = bf_marsh_next_child(marsh, marsh_elem)))
        return -EINVAL;
    memcpy(&front, marsh_elem->data, sizeof(front));

    if (!(marsh_elem = bf_marsh_next_child(marsh, marsh_elem)))
        return -EINVAL;

    r = bf_chain_new_from_marsh(&chain, marsh_elem);
    if (r < 0)
        return r;

    r = bf_cgen_new(&_cgen, front, &chain);
    if (r)
        return bf_err_r(r, "failed to allocate codegen object");

    if (!(marsh_elem = bf_marsh_next_child(marsh, marsh_elem)))
        return -EINVAL;
    if (!bf_marsh_is_empty(marsh_elem)) {
        r = bf_program_unmarsh(marsh_elem, &_cgen->program, _cgen->chain);
        if (r < 0)
            return r;
    }

    if (bf_marsh_next_child(marsh, marsh_elem))
        bf_warn("codegen marsh has more children than expected");

    *cgen = TAKE_PTR(_cgen);

    return 0;
}

void bf_cgen_free(struct bf_cgen **cgen)
{
    bf_assert(cgen);

    if (!*cgen)
        return;

    bf_program_free(&(*cgen)->program);
    bf_chain_free(&(*cgen)->chain);

    free(*cgen);
    *cgen = NULL;
}

int bf_cgen_marsh(const struct bf_cgen *cgen, struct bf_marsh **marsh)
{
    _cleanup_bf_marsh_ struct bf_marsh *_marsh = NULL;
    int r;

    bf_assert(cgen);
    bf_assert(marsh);

    r = bf_marsh_new(&_marsh, NULL, 0);
    if (r)
        return r;

    r = bf_marsh_add_child_raw(&_marsh, &cgen->front, sizeof(cgen->front));
    if (r < 0)
        return bf_err_r(r, "failed to serialize codegen");

    {
        // Serialize cgen.chain
        _cleanup_bf_marsh_ struct bf_marsh *chain_elem = NULL;

        r = bf_chain_marsh(cgen->chain, &chain_elem);
        if (r < 0)
            return r;

        r = bf_marsh_add_child_obj(&_marsh, chain_elem);
        if (r < 0)
            return r;
    }

    {
        _cleanup_bf_marsh_ struct bf_marsh *prog_elem = NULL;

        if (cgen->program) {
            r = bf_program_marsh(cgen->program, &prog_elem);
            if (r < 0)
                return r;
        } else {
            r = bf_marsh_new(&prog_elem, NULL, 0);
            if (r < 0)
                return r;
        }

        r = bf_marsh_add_child_obj(&_marsh, prog_elem);
        if (r)
            return r;
    }

    *marsh = TAKE_PTR(_marsh);

    return 0;
}

void bf_cgen_dump(const struct bf_cgen *cgen, prefix_t *prefix)
{
    bf_assert(cgen);
    bf_assert(prefix);

    DUMP(prefix, "struct bf_cgen at %p", cgen);

    bf_dump_prefix_push(prefix);
    DUMP(prefix, "front: %s", bf_front_to_str(cgen->front));

    // Chain
    DUMP(prefix, "chain: struct bf_chain *");
    bf_dump_prefix_push(prefix);
    bf_chain_dump(cgen->chain, bf_dump_prefix_last(prefix));
    bf_dump_prefix_pop(prefix);

    // Programs
    if (cgen->program) {
        DUMP(bf_dump_prefix_last(prefix), "program: struct bf_program *");
        bf_dump_prefix_push(prefix);
        bf_program_dump(cgen->program, bf_dump_prefix_last(prefix));
        bf_dump_prefix_pop(prefix);
    } else {
        DUMP(bf_dump_prefix_last(prefix), "program: (struct bf_program *)NULL");
    }

    bf_dump_prefix_pop(prefix);
}

int bf_cgen_get_counter(const struct bf_cgen *cgen,
                        enum bf_counter_type counter_idx,
                        struct bf_counter *counter)
{
    bf_assert(cgen && counter);

    /* There are two more counter than rules. The special counters must
     * be accessed via the specific values, to avoid confusion. */
    enum bf_counter_type rule_count = bf_list_size(&cgen->chain->rules);
    if (counter_idx == BF_COUNTER_POLICY) {
        counter_idx = rule_count;
    } else if (counter_idx == BF_COUNTER_ERRORS) {
        counter_idx = rule_count + 1;
    } else if (counter_idx < 0 || counter_idx >= rule_count) {
        return -EINVAL;
    }

    return bf_program_get_counter(cgen->program, counter_idx, counter);
}

int bf_cgen_load(struct bf_cgen *cgen)
{
    _cleanup_bf_program_ struct bf_program *prog = NULL;
    int r;

    bf_assert(cgen);

    bf_info("load %s", cgen->chain->name);
    bf_cgen_dump(cgen, EMPTY_PREFIX);

    r = bf_program_new(&prog, cgen->chain);
    if (r < 0)
        return r;

    r = bf_program_generate(prog);
    if (r < 0)
        return bf_err_r(r, "failed to generate bf_program");

    r = bf_program_load(prog);
    if (r < 0)
        return bf_err_r(r, "failed to load the chain");

    cgen->program = TAKE_PTR(prog);

    return r;
}

int bf_cgen_attach(struct bf_cgen *cgen, const struct bf_ns *ns,
                   struct bf_hookopts **hookopts)
{
    int r;

    bf_assert(cgen && ns && hookopts);

    bf_info("attaching %s to %s", cgen->chain->name,
            bf_hook_to_str(cgen->chain->hook));
    bf_hookopts_dump(*hookopts, EMPTY_PREFIX);

    r = bf_ns_set(ns, bf_ctx_get_ns());
    if (r)
        return bf_err_r(r, "failed to switch to the client's namespaces");

    r = bf_program_attach(cgen->program, hookopts);
    if (r < 0)
        bf_err_r(r, "failed to load and attach the chain");

    if (bf_ns_set(bf_ctx_get_ns(), ns))
        bf_abort("failed to restore previous namespaces, aborting");

    return r;
}

int bf_cgen_update(struct bf_cgen *cgen, struct bf_chain **new_chain)
{
    _cleanup_bf_program_ struct bf_program *new_prog = NULL;
    struct bf_link *link = cgen->program->link;
    int r;

    bf_assert(cgen && new_chain);

    r = bf_program_new(&new_prog, *new_chain);
    if (r < 0)
        return bf_err_r(r, "failed to create a new bf_program");

    r = bf_program_generate(new_prog);
    if (r < 0) {
        return bf_err_r(r,
                        "failed to generate the bytecode for a new bf_program");
    }

    r = bf_program_load(new_prog);
    if (r)
        return bf_err_r(r, "failed to load new program");

    if (cgen->program->link->hookopts) {
        r = bf_link_update(link, cgen->chain->hook, new_prog->runtime.prog_fd);
        if (r)
            return bf_err_r(r,
                            "failed to update bf_link object with new program");
    }

    bf_swap(cgen->program, new_prog);
    bf_swap(cgen->program->link, new_prog->link);

    // new_prog is now the previous program with an empty link
    if (bf_opts_persist())
        bf_program_unload(cgen->program);

    bf_chain_free(&cgen->chain);
    cgen->chain = TAKE_PTR(*new_chain);

    return 0;
}

void bf_cgen_detach(struct bf_cgen *cgen)
{
    bf_assert(cgen);

    bf_program_detach(cgen->program);
}

void bf_cgen_unload(struct bf_cgen *cgen)
{
    bf_assert(cgen);

    bf_program_unload(cgen->program);
}

int bf_cgen_get_counters(const struct bf_cgen *cgen, bf_list *counters)
{
    bf_list _counters = bf_list_default_from(*counters);
    int r;

    bf_assert(cgen && counters);

    for (ssize_t i = BF_COUNTER_POLICY;
         i < (ssize_t)bf_list_size(&cgen->chain->rules); ++i) {
        _cleanup_bf_counter_ struct bf_counter *counter = NULL;

        r = bf_counter_new(&counter, 0, 0);
        if (r)
            return r;

        r = bf_cgen_get_counter(cgen, i, counter);
        if (r)
            return r;

        r = bf_list_add_tail(&_counters, counter);
        if (r)
            return r;

        TAKE_PTR(counter);
    }

    *counters = bf_list_move(_counters);

    return 0;
}
