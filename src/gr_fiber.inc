/* libSceFiber for Gravity Rush, built on Windows fibers (Linux: ucontext).
 * Included by runtime_extra.c just before the exports[] table.
 * A PS4 fiber runs on the thread that calls sceFiberRun; it gives control back with
 * sceFiberReturnToThread, or hands it to another fiber with sceFiberSwitch. */
#ifdef _WIN32
#include <windows.h>
#ifndef FIBER_FLAG_FLOAT_SWITCH
#define FIBER_FLAG_FLOAT_SWITCH 0x1
#endif
#define GR_FIBER_CALLBACK VOID CALLBACK
#else
/* A context per fiber and one per thread; swapcontext keeps MXCSR and the x87 control word,
 * as FIBER_FLAG_FLOAT_SWITCH does. Stacks come from the low range like guest thread stacks
 * (guest code may pack stack addresses) and are reused once finalized. */
#include <sys/mman.h>
#include <ucontext.h>
#define GR_FIBER_CALLBACK void
#endif

#define GR_FIB_MAGIC 0x52424946u
#define GR_FIB_ERR(n) ((int32_t)(UINT32_C(0x80590000)|(n)))
/* 1 NULL, 2 ALIGNMENT, 3 RANGE, 4 INVALID, 5 PERMISSION, 6 STATE, 7 BUSY, 8 AGAIN, 9 FATAL */

typedef void (ABI *GrFiberEntry)(uint64_t arg_init, uint64_t arg_run);

typedef struct GrFiber {
    uint32_t magic;
    int state;              /* 0 not started, 1 running, 2 suspended, 3 finished */
    void *win;              /* Windows fiber that holds this fiber's stack (Linux: &ctx) */
    void *return_to;        /* Windows fiber of the thread that ran us (Linux: its context) */
    void *guest;            /* the buffer the game gave to sceFiberInitialize */
    GrFiberEntry entry;
    uint64_t arg_init, arg_run;
    uint64_t *out_return;   /* receives the value passed to ReturnToThread */
    uint64_t *resume_out;   /* receives the value when this fiber is resumed */
    size_t stack;
    char name[32];
#ifndef _WIN32
    ucontext_t ctx;
    void *stack_memory;
#endif
} GrFiber;

static __thread GrFiber *gr_current;
static __thread void *gr_thread_fiber;
static unsigned gr_fibers_created, gr_fiber_switches;

#ifdef _WIN32
static void gr_switch(void *from, void *to) { (void)from; SwitchToFiber(to); }
#else
static __thread ucontext_t gr_thread_context;
static void gr_switch(void *from, void *to) { swapcontext(from, to); }

/* Finalized fibers' stacks, for the next ones (the low range is never returned). */
static pthread_mutex_t gr_stack_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *memory; size_t size; } gr_free_stacks[64];
static unsigned gr_free_count;

static void *gr_stack_get(size_t size) {
    void *memory = NULL;
    pthread_mutex_lock(&gr_stack_lock);
    for (unsigned i = 0; i < gr_free_count; ++i) {
        if (gr_free_stacks[i].size == size) {
            memory = gr_free_stacks[i].memory;
            gr_free_stacks[i] = gr_free_stacks[--gr_free_count];
            break;
        }
    }
    pthread_mutex_unlock(&gr_stack_lock);
    return memory ? memory : runtime_low_map(size, PROT_READ | PROT_WRITE);
}

static void gr_stack_put(void *memory, size_t size) {
    pthread_mutex_lock(&gr_stack_lock);
    if (gr_free_count < sizeof(gr_free_stacks) / sizeof(*gr_free_stacks)) {
        gr_free_stacks[gr_free_count].memory = memory;
        gr_free_stacks[gr_free_count++].size = size;
        memory = NULL;
    }
    pthread_mutex_unlock(&gr_stack_lock);
    if (memory) munmap(memory, size);  /* the list is full: the memory goes, not the address range */
}
#endif

static GrFiber *gr_fiber_get(void *guest) {
    if (!guest) return NULL;
    uint64_t *g = guest;
    GrFiber *f = (GrFiber *)(uintptr_t)g[1];
    return (g[0] == GR_FIB_MAGIC && f && f->magic == GR_FIB_MAGIC) ? f : NULL;
}

static void gr_thread_to_fiber(void) {
#ifdef _WIN32
    if (!gr_thread_fiber)
        gr_thread_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
#else
    gr_thread_fiber = &gr_thread_context;  /* filled by the first swapcontext */
#endif
}

static GR_FIBER_CALLBACK gr_fiber_main(void *param) {
    GrFiber *f = param;
    f->entry(f->arg_init, f->arg_run);   /* normally never returns */
    f->state = 3;                        /* it returned: treat as a final ReturnToThread(0) */
    if (f->out_return) *f->out_return = 0;
    gr_current = NULL;
    for (;;) gr_switch(f->win, f->return_to);
}

#ifndef _WIN32
/* makecontext passes only ints: the starting fiber is gr_current (set before the switch). */
static void gr_fiber_entry(void) { gr_fiber_main(gr_current); }
#endif

static int gr_fiber_start(GrFiber *f, uint64_t arg) {
    f->arg_run = arg;
#ifdef _WIN32
    f->win = CreateFiberEx(f->stack, f->stack, FIBER_FLAG_FLOAT_SWITCH, gr_fiber_main, f);
#else
    if (!f->stack_memory && !(f->stack_memory = gr_stack_get(f->stack))) return 0;
    if (getcontext(&f->ctx)) return 0;
    f->ctx.uc_stack.ss_sp = f->stack_memory;
    f->ctx.uc_stack.ss_size = f->stack;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, gr_fiber_entry, 0);
    f->win = &f->ctx;
#endif
    return f->win != NULL;
}

static ABI int32_t gr_fiber_initialize(void *guest, const char *name, GrFiberEntry entry, uint64_t arg_init,
                                       void *addr_context, uint64_t size_context, const void *opt, uint32_t flags, uint32_t build) {
    (void)addr_context; (void)opt; (void)flags; (void)build;
    if (!guest || !entry) return GR_FIB_ERR(1);
    if ((uintptr_t)guest & 15) return GR_FIB_ERR(2);
    GrFiber *f = calloc(1, sizeof(*f));
    if (!f) return GR_FIB_ERR(9);
    f->magic = GR_FIB_MAGIC; f->guest = guest; f->entry = entry; f->arg_init = arg_init;
    size_t s = (size_t)size_context * 2;
    if (s < 256 * 1024) s = 256 * 1024;
    if (s > 8u * 1024 * 1024) s = 8u * 1024 * 1024;
    f->stack = s;
    if (name) snprintf(f->name, sizeof(f->name), "%s", name);
    uint64_t *g = guest;
    g[0] = GR_FIB_MAGIC; g[1] = (uint64_t)(uintptr_t)f;
    ++gr_fibers_created;
    return 0;
}

static ABI int32_t gr_fiber_finalize(void *guest) {
    GrFiber *f = gr_fiber_get(guest);
    if (!f) return GR_FIB_ERR(1);
    if (f->state == 1) return GR_FIB_ERR(6);
#ifdef _WIN32
    if (f->win) DeleteFiber(f->win);
#else
    if (f->stack_memory) gr_stack_put(f->stack_memory, f->stack);
#endif
    uint64_t *g = guest;
    g[0] = 0; g[1] = 0; f->magic = 0;
    free(f);
    return 0;
}

static ABI int32_t gr_fiber_run(void *guest, uint64_t arg, uint64_t *out_return) {
    GrFiber *f = gr_fiber_get(guest);
    if (!f) return GR_FIB_ERR(1);
    if (gr_current) return GR_FIB_ERR(5);               /* Run belongs to a thread, not to a fiber */
    if (f->state == 1 || f->state == 3) return GR_FIB_ERR(6);
    gr_thread_to_fiber();
    f->return_to = gr_thread_fiber;
    f->out_return = out_return;
    if (f->state == 0) { if (!gr_fiber_start(f, arg)) return GR_FIB_ERR(9); }
    else if (f->resume_out) *f->resume_out = arg;
    f->state = 1; gr_current = f; ++gr_fiber_switches;
    gr_switch(gr_thread_fiber, f->win);
    gr_current = NULL;                                   /* back on the thread */
    return 0;
}

static ABI int32_t gr_fiber_switch(void *guest, uint64_t arg, uint64_t *out_on_run) {
    GrFiber *to = gr_fiber_get(guest), *from = gr_current;
    if (!to) return GR_FIB_ERR(1);
    if (!from) return GR_FIB_ERR(5);
    if (to == from || to->state == 1 || to->state == 3) return GR_FIB_ERR(6);
    to->return_to = from->return_to; to->out_return = from->out_return;
    if (to->state == 0) { if (!gr_fiber_start(to, arg)) return GR_FIB_ERR(9); }
    else if (to->resume_out) *to->resume_out = arg;
    from->resume_out = out_on_run; from->state = 2;
    to->state = 1; gr_current = to; ++gr_fiber_switches;
    gr_switch(from->win, to->win);
    return 0;                                            /* resumed: our value is already stored */
}

static ABI int32_t gr_fiber_return_to_thread(uint64_t arg, uint64_t *out_on_run) {
    GrFiber *f = gr_current;
    if (!f) return GR_FIB_ERR(5);
    f->resume_out = out_on_run; f->state = 2;
    if (f->out_return) *f->out_return = arg;
    gr_current = NULL; ++gr_fiber_switches;
    gr_switch(f->win, f->return_to);
    return 0;
}

static ABI int32_t gr_fiber_get_self(void **out) {
    if (!out) return GR_FIB_ERR(1);
    if (!gr_current) return GR_FIB_ERR(5);
    *out = gr_current->guest;
    return 0;
}
