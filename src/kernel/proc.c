#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/container.h>

struct proc root_proc;
extern struct container root_container;

void kernel_entry();
void proc_entry();

static SpinLock plock;

define_early_init(plock) {
    init_spinlock(&plock);
}

void set_parent_to_this(struct proc* proc)
{
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    _acquire_spinlock(&plock);
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    _release_spinlock(&plock);

}

NO_RETURN void exit(int code)
{
    // TODO
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the rootproc of the container, and notify the it if there is zombie
    // 4. notify the parent
    // 5. sched(ZOMBIE)
    // NOTE: be careful of concurrency
    _acquire_spinlock(&plock);
    auto this = thisproc();
    ASSERT(this != this->container->rootproc && !this->idle);
    this -> exitcode = code;

    free_pgdir(&(this->pgdir));

    while (!_empty_list(&(this -> children))) {
        ListNode* child_node = (this -> children).next; 
        _detach_from_list(child_node);
        
        proc* child_proc = container_of(child_node, proc, ptnode);
        // printk("child_node: %p, child_proc: %p, pid: %d\n", child_node, child_proc, child_proc->pid);
        child_proc -> parent = child_proc -> container -> rootproc;
        _insert_into_list(&(root_proc.children), &(child_proc -> ptnode));

        if (is_zombie(child_proc)) {
            post_sem(&(root_proc.childexit));
        }
    }
    // _release_spinlock(&plock);
    post_sem(&(this -> parent ->childexit));
    _acquire_sched_lock();
    _release_spinlock(&plock);
    _sched(ZOMBIE);
    // _release_sched_lock();
    PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int* exitcode, int* pid)
{
    // TODO
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its local pid and exitcode
    // NOTE: be careful of concurrency
    auto this = thisproc();
    if (_empty_list(&(this -> children))) {
        // _release_spinlock(&plock);
        return -1;
    }

    bool sig = wait_sem(&(this -> childexit));
    if (sig == false) {
        return -2;
    }

    _acquire_spinlock(&plock);
    int w_pid = 0;
    _for_in_list(child_node, &(this -> children)) {
        if (child_node == &(this -> children)) {
            continue;
        }

        proc* child_proc = container_of(child_node, proc, ptnode);

        if (is_zombie(child_proc)) {
            
            _detach_from_list(child_node);

            *exitcode = child_proc -> exitcode;
            *pid = child_proc -> pid;
    
            w_pid = child_proc -> localpid;

            free_pid(&child_proc->container->localpidmap, child_proc -> localpid);
            free_pid(&pidmap, child_proc -> pid);
            kfree_page(child_proc -> kstack);
            kfree(child_proc);
            break;
            // _release_spinlock(&plock);
        }
    }
    _release_spinlock(&plock);
    return w_pid;
}

proc* traverse_proc(proc *p, int pid) {  
    _for_in_list(child_node, &p -> children) {
        if (child_node == &p -> children) continue;
        proc* child = container_of(child_node, proc, ptnode);
        if (child -> pid == pid) 
            return child;
        else {
            proc* obj = traverse_proc(child, pid);
            if (obj != NULL) {
                return obj;
            }
        }    
    }
    return NULL;
}

int kill(int pid) {
    // TODO
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).
    _acquire_spinlock(&plock);
    proc* target_proc = traverse_proc(&root_proc, pid);
    if (target_proc == NULL || is_unused(target_proc)) {
        _release_spinlock(&plock);
        return -1;
    }

    target_proc -> killed = true;
    alert_proc(target_proc);

    _release_spinlock(&plock);
    return 0;
}

int start_proc(struct proc* p, void(*entry)(u64), u64 arg)
{
    // TODO
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its local pid
    // NOTE: be careful of concurrency
    if (p -> parent == NULL) {
        _acquire_spinlock(&plock);
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
        _release_spinlock(&plock);
    }
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    p->localpid = alloc_pid(&p->container->localpidmap);
    // printk("pid:%d\n", p->localpid);
    activate_proc(p);
    return p->localpid;
}

void init_proc(struct proc* p)
{
    // TODO
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency
    memset(p, 0, sizeof(*p));
    p->killed = false;
    p->pid = alloc_pid(&pidmap);
    // printk("PID:%d\n", p->pid);
    p->state = UNUSED;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_schinfo(&p->schinfo, false);
    init_pgdir(&(p->pgdir));
    p->container = &root_container;
    // printk("%p=====%d\n", p->container, (p->container==NULL));
    p->kstack = kalloc_page();
    p->kcontext = (KernelContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(KernelContext) - sizeof(UserContext));
    p->ucontext = (UserContext*)((u64)p->kstack + PAGE_SIZE - 16 -sizeof(UserContext));
}

struct proc* create_proc()
{
    // printk("ooo\n");
    struct proc* p = kalloc(sizeof(struct proc));
    // printk("ooo\n");
    init_proc(p);
    // printk("ooo\n");
    return p;
}

define_init(root_proc)
{
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}
