/* Memory diagnostics (included by runtime_thread.c on Windows). GOW3_MEM_CMD=<file>: commands
 * read again when the file changes, one per line:
 *   scan <tag> [pos_tol axis_tol]  every guest thread paused; guest memory searched for the camera of the
 *                      last frame (gow3_camera_world, from the video core): its position, its axes
 *                      (also negated, also with a stride of 4 floats) and its rotation quaternion.
 *   watch <a>[,<a>..]  hardware write breakpoints (up to 4, 4 bytes each) on every guest thread;
 *                      each writing instruction is counted and printed with the stack's code
 *                      addresses the first time, the counts again every 2 s.
 *   unwatch            removes them. */
extern volatile float gow3_camera_world[12]; /* position, right, up, forward (world space) */
extern volatile unsigned gow3_camera_seq;

#define MW_TABLE 512
typedef struct { volatile LONG64 rip; volatile LONG count, slot, printed; uint64_t stack[8], regs[6]; float value; } MwHit;
static MwHit mw_hits[MW_TABLE];
static uint64_t mw_addr[4];
static int mw_count;
static volatile LONG mw_armed;
/* trace <addr> <len> [reg [rip]]: at each write to <addr> by one instruction (<rip>, the address
 * after the write, or the first one seen), <len>
 * bytes at the register (rbx by default; rdi, rsi, r14, r15, rax) are appended to <cmd file>.bin
 * as records {u64 microseconds, u64 base, bytes}. */
static int mw_trace_len, mw_trace_reg=1;
static uint64_t mw_trace_rip;
static unsigned char *mw_trace_buf;
static volatile LONG64 mw_trace_used;
#define MW_TRACE_CAP (64u<<20)

static LONG CALLBACK mw_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c=info->ContextRecord;
    DWORD64 dr6=c->Dr6;
    if (!(dr6&0xf)) return EXCEPTION_CONTINUE_SEARCH;
    int slot=0;
    while (slot<3 && !(dr6&(1u<<slot))) ++slot;
    uint64_t rip=c->Rip;
    for (unsigned k=0;k<MW_TABLE;++k) {
        MwHit *h=&mw_hits[(rip*0x9e3779b1u+k)%MW_TABLE];
        LONG64 seen=h->rip;
        if (!seen && InterlockedCompareExchange64(&h->rip,(LONG64)rip,0)==0) {
            h->slot=slot;
            h->regs[0]=c->Rax; h->regs[1]=c->Rbx; h->regs[2]=c->Rdi; h->regs[3]=c->Rsi; h->regs[4]=c->R14; h->regs[5]=c->R15;
            memcpy(&h->value,(void *)(uintptr_t)mw_addr[slot],4);
            /* Guest code keeps frame pointers: the rbp chain gives the callers. */
            const unsigned char *image=runtime_image_base();
            uint64_t fp=c->Rbp;
            int n=0;
            while (n<8 && fp>c->Rsp && fp-c->Rsp<0x100000 && !(fp&7)) {
                const uint64_t ret=((const uint64_t *)(uintptr_t)fp)[1];
                if (!image || ret-(uintptr_t)image>=GUEST_SPAN) break;
                h->stack[n++]=ret;
                fp=((const uint64_t *)(uintptr_t)fp)[0];
            }
            seen=(LONG64)rip;
        }
        if ((uint64_t)seen==rip) { InterlockedIncrement(&h->count); break; }
    }
    if (mw_trace_len && slot==0) {
        if (!mw_trace_rip) mw_trace_rip=rip;
        if (rip==mw_trace_rip) {
            const uint64_t regs[6]={c->Rax,c->Rbx,c->Rdi,c->Rsi,c->R14,c->R15};
            const uint64_t base=regs[mw_trace_reg];
            const LONG64 size=16+mw_trace_len;
            const LONG64 at=InterlockedExchangeAdd64(&mw_trace_used,size);
            if (mw_trace_buf && at+size<=(LONG64)MW_TRACE_CAP) {
                LARGE_INTEGER t, fq; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&fq);
                const uint64_t us=(uint64_t)(t.QuadPart/(fq.QuadPart/1000000));
                memcpy(mw_trace_buf+at,&us,8); memcpy(mw_trace_buf+at+8,&base,8);
                memcpy(mw_trace_buf+at+16,(const void *)(uintptr_t)base,(size_t)mw_trace_len);
            }
        }
    }
    c->Dr6=0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static size_t mw_threads(HANDLE *handles, size_t max) {
    size_t count=0;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t && count<max;t=t->next)
        if (!t->finished && t->win && t!=current) handles[count++]=t->win;
    pthread_mutex_unlock(&lock);
    return count;
}

static void mw_apply(void) {
    HANDLE handles[DUMP_MAX_THREADS];
    size_t count=mw_threads(handles,DUMP_MAX_THREADS);
    DWORD64 dr7=0;
    for (int i=0;i<mw_count;++i) dr7|=(DWORD64)1<<(i*2) | (DWORD64)0xd<<(16+i*4); /* write, 4 bytes */
    for (size_t i=0;i<count;++i) {
        if (SuspendThread(handles[i])==(DWORD)-1) continue;
        CONTEXT c; memset(&c,0,sizeof(c));
        c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(handles[i],&c)) {
            c.Dr0=mw_count>0 ? mw_addr[0] : 0; c.Dr1=mw_count>1 ? mw_addr[1] : 0;
            c.Dr2=mw_count>2 ? mw_addr[2] : 0; c.Dr3=mw_count>3 ? mw_addr[3] : 0;
            c.Dr7=dr7; c.Dr6=0;
            SetThreadContext(handles[i],&c);
        }
        ResumeThread(handles[i]);
    }
}

static void mw_report(void) {
    char label[160];
    for (unsigned i=0;i<MW_TABLE;++i) {
        MwHit *h=&mw_hits[i];
        if (!h->rip) continue;
        LONG count=h->count;
        if (count==h->printed) continue;
        if (!code_label((uint64_t)h->rip,label,sizeof(label))) snprintf(label,sizeof(label),"0x%llx",(unsigned long long)h->rip);
        printf("Memwatch: [%llx] written before %s: %ld times",(unsigned long long)mw_addr[h->slot],label,count);
        if (!h->printed) {
            printf(" (first value %g; callers",h->value);
            for (int k=0;k<8 && h->stack[k];++k) if (code_label(h->stack[k],label,sizeof(label))) printf(" %s",label);
            printf("; rax %llx rbx %llx rdi %llx rsi %llx r14 %llx r15 %llx)",(unsigned long long)h->regs[0],
                   (unsigned long long)h->regs[1],(unsigned long long)h->regs[2],(unsigned long long)h->regs[3],
                   (unsigned long long)h->regs[4],(unsigned long long)h->regs[5]);
        }
        printf("\n");
        h->printed=count;
    }
}

static int mw_near(float a, float b, float tol) { return fabsf(a-b)<=tol; }

static void mw_scan(const char *tag, float pos_tol, float axis_tol) {
    HANDLE handles[DUMP_MAX_THREADS];
    size_t count=mw_threads(handles,DUMP_MAX_THREADS);
    for (size_t i=0;i<count;++i) SuspendThread(handles[i]);
    float w[12];
    unsigned seq=gow3_camera_seq;
    for (int i=0;i<12;++i) w[i]=gow3_camera_world[i];
    /* Quaternion of the camera basis (columns right, up, back: a rotation). */
    const float m00=w[3],m10=w[4],m20=w[5],m01=w[6],m11=w[7],m21=w[8],m02=-w[9],m12=-w[10],m22=-w[11];
    float q[4]; /* x y z w */
    const float tr=m00+m11+m22;
    if (tr>0) { float s=sqrtf(tr+1.0f)*2; q[3]=0.25f*s; q[0]=(m21-m12)/s; q[1]=(m02-m20)/s; q[2]=(m10-m01)/s; }
    else if (m00>m11 && m00>m22) { float s=sqrtf(1.0f+m00-m11-m22)*2; q[3]=(m21-m12)/s; q[0]=0.25f*s; q[1]=(m01+m10)/s; q[2]=(m02+m20)/s; }
    else if (m11>m22) { float s=sqrtf(1.0f+m11-m00-m22)*2; q[3]=(m02-m20)/s; q[0]=(m01+m10)/s; q[1]=0.25f*s; q[2]=(m12+m21)/s; }
    else { float s=sqrtf(1.0f+m22-m00-m11)*2; q[3]=(m10-m01)/s; q[0]=(m02+m20)/s; q[1]=(m12+m21)/s; q[2]=0.25f*s; }
    printf("Memscan %s: camera #%u pos %g %g %g right %g %g %g up %g %g %g fwd %g %g %g quat %g %g %g %g\n",tag,seq,
           w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],w[8],w[9],w[10],w[11],q[0],q[1],q[2],q[3]);
    static const char *names[]={"pos","right","up","fwd"};
    unsigned hits[8]={0};
    const unsigned char *image=runtime_image_base();
    uint64_t scanned=0;
    for (uint64_t a=0x800000000ull;a<0xfc00000000ull;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a,&mi,sizeof(mi))) break;
        uint64_t end=(uint64_t)(uintptr_t)mi.BaseAddress+mi.RegionSize;
        const DWORD p=mi.Protect&0xff;
        if (mi.State==MEM_COMMIT && !(mi.Protect&(PAGE_GUARD|PAGE_NOCACHE|PAGE_WRITECOMBINE)) &&
            (p==PAGE_READWRITE || p==PAGE_READONLY || p==PAGE_EXECUTE_READWRITE || p==PAGE_WRITECOPY)) {
            const float *f=(const float *)(uintptr_t)a;
            const size_t n=(size_t)((end-a)/4);
            scanned+=end-a;
            for (size_t i=0;i+12<n;++i) {
                const float v=f[i];
                for (int k=0;k<4;++k) {
                    const float tol=k ? axis_tol : pos_tol;
                    for (int sign=1;sign>=(k ? -1 : 1);sign-=2) {
                        const float x=sign*w[k*3],y=sign*w[k*3+1],z=sign*w[k*3+2];
                        if (!mw_near(v,x,tol)) continue;
                        for (int stride=1;stride<=4;stride+=3) {
                            if (!mw_near(f[i+stride],y,tol) || !mw_near(f[i+2*stride],z,tol)) continue;
                            const uint64_t at=(uint64_t)(uintptr_t)(f+i);
                            if (hits[k]++<48)
                                printf("Memscan %s: %s%s%s at 0x%llx%s+0x%llx\n",tag,sign<0 ? "-" : "",names[k],stride==4 ? " (stride 4)" : "",
                                       (unsigned long long)at,image && at-(uintptr_t)image<GUEST_SPAN ? " eboot" : "",
                                       (unsigned long long)(image && at-(uintptr_t)image<GUEST_SPAN ? at-(uintptr_t)image : 0));
                        }
                    }
                }
                for (int sign=1;sign>=-1;sign-=2) {
                    if (mw_near(v,sign*q[0],axis_tol) && mw_near(f[i+1],sign*q[1],axis_tol) && mw_near(f[i+2],sign*q[2],axis_tol) &&
                        mw_near(f[i+3],sign*q[3],axis_tol) && hits[4]++<48)
                        printf("Memscan %s: quat xyzw at 0x%llx\n",tag,(unsigned long long)(uintptr_t)(f+i));
                    if (mw_near(v,sign*q[3],axis_tol) && mw_near(f[i+1],sign*q[0],axis_tol) && mw_near(f[i+2],sign*q[1],axis_tol) &&
                        mw_near(f[i+3],sign*q[2],axis_tol) && hits[5]++<48)
                        printf("Memscan %s: quat wxyz at 0x%llx\n",tag,(unsigned long long)(uintptr_t)(f+i));
                }
            }
        }
        a=end;
    }
    for (size_t i=0;i<count;++i) ResumeThread(handles[i]);
    printf("Memscan %s: %.0f MB scanned; hits pos %u right %u up %u fwd %u quat %u/%u\n",tag,scanned/1048576.0,
           hits[0],hits[1],hits[2],hits[3],hits[4],hits[5]);
    fflush(stdout);
}

/* Addresses: 0x<hex> (absolute), e0x<hex> (game image offset), [<addr>]+0x<hex> (the 64-bit
 * value at <addr>, plus an offset). */
static uint64_t mw_resolve(const char *s) {
    const unsigned char *image=runtime_image_base();
    char *end;
    if (*s=='[') {
        uint64_t inner=mw_resolve(s+1);
        const char *close=strchr(s,']');
        if (!inner || !close) return 0;
        uint64_t base=0;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)inner,&mi,sizeof(mi)) || mi.State!=MEM_COMMIT) return 0;
        memcpy(&base,(void *)(uintptr_t)inner,8);
        return base+(close[1]=='+' ? strtoull(close+2,NULL,0) : 0);
    }
    if (*s=='e') return image ? (uint64_t)(uintptr_t)image+strtoull(s+1,&end,0) : 0;
    return strtoull(s,&end,0);
}

/* Called every 100 ms by the watcher thread. */
static void mw_poll(void) {
    static const char *path; static int checked; static long long last_size=-1; static time_t last_time;
    static ULONGLONG next_report;
    if (!checked) { path=getenv("GOW3_MEM_CMD"); checked=1; }
    if (!path || !*path) return;
    if (mw_armed) {
        ULONGLONG now=GetTickCount64();
        if (now>=next_report) { next_report=now+2000; mw_apply(); mw_report(); }
    }
    struct stat st;
    if (stat(path,&st)!=0 || (st.st_size==last_size && st.st_mtime==last_time)) return;
    last_size=st.st_size; last_time=st.st_mtime;
    FILE *f=fopen(path,"r");
    if (!f) return;
    char line[512];
    while (fgets(line,sizeof(line),f)) {
        char cmd[16]={0}, arg[480]={0};
        if (sscanf(line,"%15s %479s",cmd,arg)<1) continue;
        if (!strcmp(cmd,"scan")) {
            float pos_tol=0.05f, axis_tol=2e-3f;
            sscanf(line,"%*s %*s %f %f",&pos_tol,&axis_tol);
            mw_scan(arg[0] ? arg : "-",pos_tol,axis_tol);
        }
        else if (!strcmp(cmd,"peek")) {
            char arg2[64]={0};
            sscanf(line,"%*s %*s %63s",arg2);
            uint64_t a=mw_resolve(arg), n=arg2[0] ? strtoull(arg2,NULL,0) : 64;
            MEMORY_BASIC_INFORMATION mi;
            if (!a || !VirtualQuery((void *)(uintptr_t)a,&mi,sizeof(mi)) || mi.State!=MEM_COMMIT) { printf("Memscan: peek %s: not mapped\n",arg); continue; }
            for (uint64_t o=0;o<n;o+=16) {
                const uint32_t *u=(const uint32_t *)(uintptr_t)(a+o);
                const float *f=(const float *)u;
                printf("Memscan: peek 0x%llx: %08x %08x %08x %08x | %g %g %g %g\n",(unsigned long long)(a+o),u[0],u[1],u[2],u[3],f[0],f[1],f[2],f[3]);
            }
        }
        else if (!strcmp(cmd,"watch")) {
            mw_count=0;
            for (char *p=arg;*p && mw_count<4;) {
                char *end=strchr(p,',');
                if (end) *end=0;
                uint64_t v=mw_resolve(p);
                if (!v) break;
                mw_addr[mw_count++]=v&~3ull;
                if (!end) break;
                p=end+1;
            }
            for (int i=0;i<mw_count;++i) printf("Memwatch: slot %d at 0x%llx\n",i,(unsigned long long)mw_addr[i]);
            memset((void *)mw_hits,0,sizeof(mw_hits));
            static int handler;
            if (!handler) { AddVectoredExceptionHandler(1,mw_handler); handler=1; }
            mw_armed=1;
            mw_apply();
            printf("Memwatch: %d addresses armed\n",mw_count);
        } else if (!strcmp(cmd,"trace")) {
            char len_s[32]={0}, reg_s[16]={0}, rip_s[32]={0};
            sscanf(line,"%*s %*s %31s %15s %31s",len_s,reg_s,rip_s);
            static const char *reg_names[]={"rax","rbx","rdi","rsi","r14","r15"};
            mw_trace_reg=1;
            for (int i=0;i<6;++i) if (!strcmp(reg_s,reg_names[i])) mw_trace_reg=i;
            if (!mw_trace_buf) mw_trace_buf=malloc(MW_TRACE_CAP);
            mw_trace_used=0; mw_trace_rip=rip_s[0] ? mw_resolve(rip_s) : 0;
            mw_addr[0]=mw_resolve(arg)&~3ull; mw_count=mw_addr[0] ? 1 : 0;
            memset((void *)mw_hits,0,sizeof(mw_hits));
            mw_trace_len=(int)strtol(len_s,NULL,0);
            static int handler2;
            if (!handler2) { AddVectoredExceptionHandler(1,mw_handler); handler2=1; }
            mw_armed=1;
            mw_apply();
            printf("Memwatch: tracing %d bytes at %s on writes to 0x%llx\n",mw_trace_len,reg_names[mw_trace_reg],(unsigned long long)mw_addr[0]);
        } else if (!strcmp(cmd,"tracesave")) {
            char out[600];
            snprintf(out,sizeof(out),"%s.bin",path);
            const int len=mw_trace_len;
            mw_trace_len=0; mw_count=0; mw_armed=0;
            mw_apply();
            FILE *o=fopen(out,"wb");
            LONG64 used=mw_trace_used;
            if (used>(LONG64)MW_TRACE_CAP) used=MW_TRACE_CAP/(16+len)*(16+len);
            if (o) { fwrite(&len,4,1,o); fwrite(mw_trace_buf,1,(size_t)used,o); fclose(o); }
            printf("Memwatch: trace of %lld records saved (rip %s)\n",(long long)(used/(16+len)),"");
            mw_report();
        } else if (!strcmp(cmd,"unwatch")) {
            mw_report();
            mw_count=0; mw_armed=0;
            mw_apply();
            printf("Memwatch: off\n");
        }
    }
    fclose(f);
    fflush(stdout);
}
