#include "ktm.h"
#include <linux/kdebug.h>
#include <linux/string.h>

static int ktm_die_notifier(struct notifier_block *nb, unsigned long val, void *data)
{
    struct die_args *args = (struct die_args *)data;
    struct pt_regs *regs;

    if (!args) {
        return NOTIFY_DONE;
    }

    regs = args->regs;
    if (!regs || !args->str) {
        return NOTIFY_DONE;
    }
    if (strstr(args->str, "undefined instruction") == NULL) {
        return NOTIFY_DONE;
    }

    if (regs->pc >= func1_old && regs->pc < func1_old + func1_len) {
        unsigned long offset = func1_new - func1_old;
        pr_info("KTM: redirecting PC from old func1 %llx -> %llx\n",
                (unsigned long long)regs->pc, (unsigned long long)(regs->pc + offset));
        regs->pc += offset;
        return NOTIFY_STOP;
    }

    if (regs->pc >= func2_old && regs->pc < func2_old + func2_len) {
        unsigned long offset = func2_new - func2_old;
        pr_info("KTM: redirecting PC from old func2 %llx -> %llx\n",
                (unsigned long long)regs->pc, (unsigned long long)(regs->pc + offset));
        regs->pc += offset;
        return NOTIFY_STOP;
    }

    return NOTIFY_DONE;
}

static struct notifier_block ktm_nb = {
    .notifier_call = ktm_die_notifier,
};

int ktm_register_die_notifier(void)
{
    return register_die_notifier(&ktm_nb);
}

void ktm_unregister_die_notifier(void)
{
    unregister_die_notifier(&ktm_nb);
}
