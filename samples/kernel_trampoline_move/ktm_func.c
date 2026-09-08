#include "ktm.h"

void (*func1_ptr)(void);
void (*func2_ptr)(void);

unsigned long func1_orig;
unsigned long func2_orig;

unsigned long func1_old;
unsigned long func2_old;

unsigned long func1_new;
unsigned long func2_new;

unsigned long func1_len;
unsigned long func2_len;


inline void wait_for_normal_state(void)
{
    while (atomic_read(&state) == KTM_EXCEPTION) {
        if (preemptible()) {
            wait_event_interruptible(exception_wq, atomic_read(&state) != KTM_EXCEPTION);
            if (signal_pending(current))
                return;
        } else {
            cpu_relax();
        }
    }
}

/* Trampolines */
static void trampoline_in_func1(void)
{
    wait_for_normal_state();

    atomic_inc(&in_count_func1);
    this_cpu_inc(percpu_in1);

    pr_info("trampoline_in_func1: func1_ptr = %px\n", func1_ptr);
    /* call current implementation */
    func1_ptr();

    atomic_dec(&in_count_func1);
    this_cpu_dec(percpu_in1);
}

static void trampoline_in_func2(void)
{
    wait_for_normal_state();

    atomic_inc(&in_count_func2);
    this_cpu_inc(percpu_in2);

    func2_ptr();

    atomic_dec(&in_count_func2);
    this_cpu_dec(percpu_in2);
}

static void trampoline_out_func3(int from)
{
    if (from == 1) {
        if (atomic_read(&in_count_func1) > 0) {
            atomic_dec(&in_count_func1);
            this_cpu_dec(percpu_in1);
        } else {
            pr_warn("trampoline_out_func3: in_count_func1 <= 0\n");
        }
    }
    if (from == 2) {
        if (atomic_read(&in_count_func2) > 0) {
            atomic_dec(&in_count_func2);
            this_cpu_dec(percpu_in2);
        } else {
            pr_warn("trampoline_out_func3: in_count_func2 <= 0\n");
        }
    }

    atomic_inc(&out_count);
    if (atomic_read(&in_count_func1) == 0 && atomic_read(&in_count_func2) == 0)
        wake_up(&zero_wq);

    func3();

    wait_for_normal_state();
    
    if (from == 1) {
        atomic_inc(&in_count_func1);
        this_cpu_inc(percpu_in1);
    }
    if (from == 2) {
        atomic_inc(&in_count_func2);
        this_cpu_inc(percpu_in2);
    }
}

void func0(void)
{
    pr_info("func0: calling func1 via trampoline\n");
    trampoline_in_func1();
    dse(0);
}

/* error: asm 会出现在 prologue 之后和 epilogue 之前, 所以不能准确测得函数长度  */
void func1(void)
{
    asm(".global func1_start\nfunc1_start:");
    pr_info("func1: %px\n", func1);
    pr_info("func1: inside -> calling func2 and func3\n");
    trampoline_in_func2();
    dse(1);
    trampoline_out_func3(1);
    dse(1);
    asm(".global func1_end\nfunc1_end:");
}

void func2(void)
{
    asm(".global func2_start\nfunc2_start:");
    pr_info("func2: %px\n", func1);
    pr_info("func2: inside -> calling func3\n");
    trampoline_out_func3(2);
    dse(2);
    asm(".global func2_end\nfunc2_end:");
}

void func3(void)
{
    pr_info("func3: doing work (simulate)\n");
    msleep(1000);
    dse(3);
}

void dse(int x)
{
    pr_info("dse: I am doing something else in func%d!\n", x);
}
