/* Exercise actual save and guest-file implementations without the GPU. */
#include "../src/runtime_file.c"
#define lock savedata_lock
#define exports savedata_exports
#include "../src/runtime_savedata.c"
#undef exports
#undef lock
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void collision(const char *path) {
    fflush(NULL);
    pid_t child=fork(); assert(child>=0);
    if (!child) { do_open(path,0x202,0644); _exit(0); }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
}
int main(void) {
    char temporary[]="out/mac-playability/save-test-XXXXXX";
    assert(mkdtemp(temporary));
    char resolved[PATH_MAX]; assert(realpath(temporary,resolved));
    assert(!setenv("BB_SOUND_HACK","0",1));
    runtime_file_configure(resolved,resolved);
    runtime_savedata_configure("CUSA03173");
    assert(!save_initialize(NULL));
    DirName name={"SPRJ0005"};
    Mount2 request={.user=1,.dir=&name,.mode=MODE_CREATE};
    MountResult result;
    assert(!save_mount2(&request,&result) && result.status==1);
    char guest[64]; snprintf(guest,sizeof(guest),"%s/UserData",result.point.data);
    int fd=(int)do_open(guest,0x202,0644); assert(fd>=3);
    assert(do_write(fd,"persist",7)==7 && !do_close(fd));
    char wrong[64]; snprintf(wrong,sizeof(wrong),"%s/userdata",result.point.data);
    assert(do_open(wrong,0,0)==-ENOENT);
    GuestStat info; assert(do_stat(wrong,&info)==-ENOENT);
    char host[1024]; assert(!translate(guest,host,sizeof(host)));
    char wronghost[1024]; snprintf(wronghost,sizeof(wronghost),"%s/savedata/1/CUSA03173/SPRJ0005/userdata",resolved);
    struct stat st;
    if (!stat(wronghost,&st)) collision(wrong);
    assert(!do_rename(guest,wrong));
    assert(do_open(guest,0,0)==-ENOENT);
    fd=(int)do_open(wrong,0,0); assert(fd>=3);
    char data[16]={0}; assert(do_read(fd,data,7)==7 && !strcmp(data,"persist") && !do_close(fd));
    Param param={.title="Test save",.subtitle="Persistence",.user_param=42};
    assert(!save_set_param(&result.point,0,&param,sizeof(param)));
    const unsigned char png[]={137,80,78,71};
    Icon icon={.buffer=png,.buffer_size=sizeof(png),.data_size=sizeof(png)};
    assert(!save_icon(&result.point,&icon));
    Delete deletion={.user=1,.dir=&name};
    assert(save_delete(&deletion)==ERR_BAD_MOUNTED);
    assert(!save_umount(&result.point));
    request.mode=MODE_RDONLY;
    assert(!save_mount2(&request,&result) && !result.status);
    assert(save_set_param(&result.point,0,&param,sizeof(param))==ERR_BAD_MOUNTED);
    assert(save_icon(&result.point,&icon)==ERR_BAD_MOUNTED);
    assert(do_open(wrong,2,0)==-EROFS);
    assert(do_truncate(wrong,0)==-EROFS);
    assert(path_op(wrong,2,0)==-EROFS);
    assert(do_rename(wrong,guest)==-EROFS);
    fd=(int)do_open(wrong,0,0); assert(fd>=3 && !do_close(fd));
    assert(!save_umount(&result.point));
    DirName lower={"sprj0005"}; request.dir=&lower;
    assert(save_mount2(&request,&result)==ERR_NOT_FOUND);
    request.dir=&name; request.mode=2;
    assert(!save_mount2(&request,&result));
    DirName names[2]; Param params[2];
    SearchCond search={.user=1};
    SearchResult found={.names=names,.names_capacity=2,.params=params};
    assert(!save_search(&search,&found) && found.hits==1 && found.set_count==1);
    assert(!strcmp(names[0].data,name.data) && !strcmp(params[0].title,param.title) && params[0].user_param==42);
    /* Force metadata fopen failure and require the guest-visible error. */
    char metadata[1024]; snprintf(metadata,sizeof(metadata),"%s/param.bin",slots[0].meta);
    assert(!unlink(metadata) && !mkdir(metadata,0755));
    assert(save_set_param(&result.point,0,&param,sizeof(param))==ERR_INTERNAL);
    assert(!rmdir(metadata));
    assert(!save_set_param(&result.point,0,&param,sizeof(param)));
    assert(!memory_setup(1,4096,&param));
    assert(!memory_set(1,"memory",6,123));
    assert(!memory_setup(1,4096,NULL));
    memset(data,0,sizeof(data)); assert(!memory_get(1,data,6,123) && !strcmp(data,"memory"));
    assert(memory_get(1,data,1,4096)==ERR_PARAMETER);
    /* FTW_PHYS must remove the symlink, never the external target. */
    char outside[1024],linkpath[1024]; snprintf(outside,sizeof(outside),"%s/outside",resolved);
    FILE *f=fopen(outside,"wb"); assert(f && fwrite("safe",1,4,f)==4 && !fclose(f));
    snprintf(linkpath,sizeof(linkpath),"%s/link",slots[0].host); assert(!symlink(outside,linkpath));
    assert(!save_umount(&result.point) && !save_delete(&deletion));
    assert(!stat(outside,&st) && st.st_size==4);
    assert(!save_terminate());
    assert(!remove_tree(resolved));
    puts("PASS: save files/metadata/memory persist; readonly, exact spelling, case-only rename, write errors and FTW_PHYS deletion");
}
