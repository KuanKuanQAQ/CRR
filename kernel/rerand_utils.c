/*
#include <linux/string.h>
#include <linux/sched.h>
#include <asm/randmod.h>

// names of modules maneged by randmod
char rand_modules[MAX_RANDMOD_MODULES][MODULE_NAME_LEN];
EXPORT_SYMBOL(rand_modules);
// num of modules managed by randmod
int module_count;
EXPORT_SYMBOL(module_count);
// every rerandmoize module have a randmod thread list
struct list_head randmod_thread_list[MAX_RANDMOD_MODULES];
EXPORT_SYMBOL(randmod_thread_list);
// lock for operating each list
spinlock_t randmod_thread_lock;
EXPORT_SYMBOL(randmod_thread_lock);
// variable used to indicate index of module doing rerandomizing
volatile int randomize_module_index;
EXPORT_SYMBOL(randomize_module_index);

static void add_rand_thread(struct sched_entity *se, const char *name)
{
	int index = -1;

	while (true) {
		spin_lock(&randmod_thread_lock);
		index = find_module_index_by_name(name);
		if (index == -1 || index >= module_count) {
			pr_err("randmod: not found moudle %s\n", name);
			spin_unlock(&randmod_thread_lock);
			break;
		}
		if (index != randomize_module_index) {
			list_add(&se->randmod_node, &randmod_thread_list[index]);
			spin_unlock(&randmod_thread_lock);
			break;
		}
		spin_unlock(&randmod_thread_lock);
	}
}

static void del_rand_thread(struct sched_entity *se, const char *name)
{
	int index = -1;

	while (true) {
		spin_lock(&randmod_thread_lock);
		index = find_module_index_by_name(name);
		if (index == -1 || index >= module_count) {
			pr_err("randmod: not found moudle %s\n", name);
			spin_unlock(&randmod_thread_lock);
			break;
		}
		if (index != randomize_module_index) {
			list_del(&se->randmod_node);
			spin_unlock(&randmod_thread_lock);
			break;
		}
		// actually this case will not happend, since when module
		// is rerandmizing, it will not execut any more
		pr_err("randmod: fatal error for randomizing module still exec: %s\n", name);
		spin_unlock(&randmod_thread_lock);
	}
}

int find_module_index_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < module_count; i++) {
		if (strcmp(name, rand_modules[i]) == 0)
			return i;
	}
	return -1;
}

noinline void add_rand_curr(const char *name)
{
	add_rand_thread(&current->se, name);
}
EXPORT_SYMBOL(add_rand_curr);


noinline void del_rand_curr(const char *name)
{
	del_rand_thread(&current->se, name);
}
EXPORT_SYMBOL(del_rand_curr);
*/