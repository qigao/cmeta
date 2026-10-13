/* A C11 cross-platform DLL/dylib loader, no borrowed pointer after unload. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HMODULE image;
static image open_image(const char *p) {return LoadLibraryA(p);}
static void *open_symbol(image im,const char *n) {
    return (void *)(uintptr_t)GetProcAddress(im,n);
}
static int close_image(image im){return FreeLibrary(im)!=0;}
#else
#  include <dlfcn.h>
typedef void *image;
static image open_image(const char *p) {
    return dlopen(p,RTLD_NOW|RTLD_LOCAL);
}
static void *open_symbol(image im,const char *n) {return dlsym(im,n);}
static int close_image(image im){return dlclose(im)==0;}
#endif
typedef struct probe_snapshot {
    uintptr_t address,container;
    unsigned min_arity,max_arity,category;
    char stable_id[128];
} probe_snapshot;
typedef int (*read_fn)(probe_snapshot *);
typedef const void *(*borrow_fn)(void);
typedef int (*equal_fn)(const void *);
static int functions(image im,read_fn *r,borrow_fn *b,equal_fn *e){
    void *p=open_symbol(im,"salts_cstl_probe_read"); if(!p) return 0;
    *(void **)r=p;
    p=open_symbol(im,"salts_cstl_probe_borrow"); if(!p) return 0;
    *(void **)b=p;
    p=open_symbol(im,"salts_cstl_probe_equal"); if(!p) return 0;
    *(void **)e=p;
    return 1;
}
int main(int argc,char **argv){
    image first,second;
    read_fn ra=NULL,rb=NULL;
    borrow_fn ba=NULL,bb=NULL;
    equal_fn ea=NULL,eb=NULL;
    probe_snapshot a={0},b={0},survivor={0};
    const void *pa,*pb;
    if(argc!=3){fputs("usage: cstl_loader A B\n",stderr);return 2;}
    first=open_image(argv[1]);if(!first)return 3;
    second=open_image(argv[2]);if(!second)return 4;
    if(!functions(first,&ra,&ba,&ea)||!functions(second,&rb,&bb,&eb))
        return 5;
    if(!ra(&a)||!rb(&b)||!a.address||!b.address||
       a.address==b.address||!a.container||!b.container||
       strcmp(a.stable_id,b.stable_id)!=0||a.min_arity!=b.min_arity||
       a.max_arity!=b.max_arity||a.category!=b.category)return 6;
    pa=ba();pb=bb();
    if((uintptr_t)pa!=a.address||(uintptr_t)pb!=b.address||
       ea(pb)!=1||eb(pa)!=1)return 7;
    puts("CSTL_CROSS_PUBLIC_GENERIC_SEMANTIC_EQUAL=1");
    puts("CSTL_CROSS_IMAGE_LOCAL_OWNER=1");
    if(!close_image(second))return 8;
    /* Never touch pb (second image's borrowed pointer) after its unload. */
    if(!ra(&survivor)||survivor.address!=a.address||
       survivor.container!=a.container||
       strcmp(survivor.stable_id,a.stable_id)!=0||ea(pa)!=1)return 9;
    puts("CSTL_CROSS_LIVE_OWNER_SURVIVED_UNLOAD=1");
    if(!close_image(first))return 10;
    puts("CSTL_CROSS_LOAD_UNLOAD_OK");
    return 0;
}
