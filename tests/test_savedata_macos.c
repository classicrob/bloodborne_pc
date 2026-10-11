/* Exercise actual save and guest-file implementations without the GPU. */
#include "../src/runtime_file.c"
#define lock savedata_lock
#define exports savedata_exports
static int metadata_fail_sync, metadata_fail_rename;
static int test_save_fsync(int fd) { if (metadata_fail_sync) { errno=EIO; return -1; } return fsync(fd); }
static int test_save_rename(const char *a,const char *b) { if (metadata_fail_rename) { errno=EIO; return -1; } return rename(a,b); }
#define fsync test_save_fsync
#define rename test_save_rename
#include "../src/runtime_savedata.c"
#undef fsync
#undef rename
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
static void restart_phase(const char *directory,const char *mode,int generation) {
    runtime_file_configure(directory,directory);
    runtime_savedata_configure("CUSA03173");
    assert(!save_initialize(NULL));
    DirName name={"RESTART"};
    int writing=strcmp(mode,"read")!=0;
    Mount2 request={.user=1,.dir=&name,.mode=writing ? MODE_CREATE2 : MODE_RDONLY};
    MountResult result;
    assert(!save_mount2(&request,&result));
    char guest[64]; snprintf(guest,sizeof(guest),"%s/userdata",result.point.data);
    Param param={.title="Restart",.user_param=(uint32_t)generation};
    Icon icon={.buffer=&generation,.buffer_size=sizeof(generation),.data_size=sizeof(generation)};
    assert(!memory_setup(1,4096,NULL));
    if (writing) {
        int fd=(int)do_open(guest,0x602,0644); assert(fd>=3);
        assert(do_write(fd,&generation,sizeof(generation))==sizeof(generation));
        assert(!do_fsync(fd) && !do_close(fd));
        assert(!save_set_param(&result.point,0,&param,sizeof(param)));
        assert(!save_icon(&result.point,&icon));
        assert(!memory_set(1,&generation,sizeof(generation),123));
        assert(!save_umount(&result.point));
        if (!strcmp(mode,"crash")) { kill(getpid(),SIGKILL); _exit(99); }
        assert(!save_terminate());
    } else {
        int got=0,fd=(int)do_open(guest,0,0); assert(fd>=3);
        assert(do_read(fd,&got,sizeof(got))==sizeof(got) && got==generation && !do_close(fd));
        Param found; assert(!read_param(slots[0].meta,&found) && found.user_param==(uint32_t)generation);
        char path[800]; snprintf(path,sizeof(path),"%s/icon0.png",slots[0].meta);
        FILE *f=fopen(path,"rb"); assert(f && fread(&got,sizeof(got),1,f)==1 && got==generation && !fclose(f));
        assert(!memory_get(1,&got,sizeof(got),123) && got==generation);
        assert(!save_umount(&result.point) && !save_terminate());
    }
}
static void child_phase(const char *exe,const char *directory,const char *mode,const char *generation) {
    fflush(NULL);
    pid_t child=fork(); assert(child>=0);
    if (!child) { execl(exe,exe,directory,mode,generation,NULL); _exit(127); }
    int status; assert(waitpid(child,&status,0)==child);
    if (!strcmp(mode,"crash")) assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    else assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(int argc,char **argv) {
    assert(!setenv("BB_SOUND_HACK","0",1));
    if (argc==4) { restart_phase(argv[1],argv[2],atoi(argv[3])); return 0; }

    char temporary[]="out/mac-playability/save-test-XXXXXX";
    assert(mkdtemp(temporary));
    char resolved[PATH_MAX]; assert(realpath(temporary,resolved));
    assert(!setenv("BB_SOUND_HACK","0",1));
    puts("Save fixture: fresh-process quit/crash persistence"); fflush(stdout);
    child_phase(argv[0],resolved,"write","1");
    child_phase(argv[0],resolved,"read","1");
    child_phase(argv[0],resolved,"crash","2");
    child_phase(argv[0],resolved,"read","2");
    assert(!remove_tree(resolved) && !mkdir(resolved,0755));
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
    Icon invalid=icon; invalid.data_size=icon.buffer_size+1;
    assert(save_icon(&result.point,&invalid)==ERR_PARAMETER);
    Param changed=param; changed.user_param=99;
    metadata_fail_sync=1;
    assert(save_set_param(&result.point,0,&changed,sizeof(changed))==ERR_INTERNAL);
    metadata_fail_sync=0; metadata_fail_rename=1;
    assert(save_set_param(&result.point,0,&changed,sizeof(changed))==ERR_INTERNAL);
    metadata_fail_rename=0;
    Param preserved; assert(!read_param(slots[0].meta,&preserved) && preserved.user_param==42);
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
    assert(memory_set(1,data,UINT64_MAX,1)==ERR_PARAMETER);
    assert(memory_setup(1,UINT64_MAX,NULL)==ERR_PARAMETER);
    assert(memory_setup(-1,4096,NULL)==ERR_INVALID_USER);
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
