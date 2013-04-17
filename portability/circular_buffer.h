/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007-2012 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include "toku_pthread.h"

namespace toku {

template<typename T>
class circular_buffer {
public:
    __attribute__((nonnull))
    void init(T * const array, size_t cap);

    void deinit(void);

    void push(const T &);

    __attribute__((warn_unused_result))
    bool trypush(const T &);

    __attribute__((warn_unused_result))
    T pop(void);

    __attribute__((nonnull, warn_unused_result))
    bool trypop(T * const);

private:
    void lock(void);

    void unlock(void);

    size_t size(void) const;

    bool is_empty(void) const;

    bool is_full(void) const;

    T *get_addr(size_t);

    void push_and_maybe_signal_unlocked(const T &elt);

    T pop_and_maybe_signal_unlocked(void);

    T *m_array;
    size_t m_cap;
    size_t m_begin, m_limit;
    toku_mutex_t m_lock;
    toku_cond_t m_push_cond;
    toku_cond_t m_pop_cond;
    int m_push_waiters, m_pop_waiters;
};

}

#include "circular_buffer.cc"

#endif // CIRCULAR_BUFFER_H
