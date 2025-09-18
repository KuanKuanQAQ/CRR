/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RERAND_H
#define _LINUX_RERAND_H

#define __tramp(fn_name) __section(".tramp.text." #fn_name) noinline
#define __rand(fn_name) __section(".rand.text." #fn_name) noinline

#define __tramp_ptr(fn_name) \
    static void* __tramp_ptr_##fn_name __attribute__((section(".data..tramp_ptr_tbl"))) __used = &fn_name
#define __rand_ptr(fn_name) \
    static void* __rand_ptr_##fn_name __attribute__((section(".data..rand_ptr_tbl"))) __used = &fn_name

#define TRAMP_FN(ret_type, fn_name, ...)           \
    __tramp_ptr(fn_name);                          \
    ret_type __tramp(fn_name) fn_name(__VA_ARGS__)

#define RAND_FN(ret_type, fn_name, ...)            \
    __rand_ptr(fn_name##_real);                     \
    ret_type __rand(fn_name##_real) fn_name##_real(__VA_ARGS__)

extern char __tramp_text_start[];
extern char __tramp_text_end[];

extern char __rand_text_start[];
extern char __rand_text_end[];

extern void* __start_tramp_ptr_tbl[];
extern void* __end_tramp_ptr_tbl[];

extern void* __start_rand_ptr_tbl[];
extern void* __end_rand_ptr_tbl[];



/*
#include "linux/types.h"
#include <linux/module.h>

#define MAX_RANDMOD_MODULES 10
#define MODULE_NAME_LEN MAX_PARAM_PREFIX_LEN

// names of modules maneged by randmod
extern char rand_modules[MAX_RANDMOD_MODULES][MODULE_NAME_LEN];
// num of modules managed by randmod
extern int module_count;
// every rerandmoize module have a randmod thread list
extern struct list_head randmod_thread_list[MAX_RANDMOD_MODULES];
// lock for operating each list
extern spinlock_t randmod_thread_lock;
// variable used to indicate index of module doing rerandomizing
extern volatile int randomize_module_index;

int find_module_index_by_name(const char *name);
void add_rand_curr(const char *name);
void del_rand_curr(const char *name);

*/

#endif /* _LINUX_RERAND_H */

