// Host-side tests for kernel/fat32.cpp. Run with: make test-fat32
// (builds a fragmented FAT32 image with tools/mkfat32.py, then runs these checks).
#include "fat32.hpp"
extern "C" {
void* fopen(const char*, const char*); int fseek(void*, long, int); size_t fread(void*, size_t, size_t, void*);
size_t fwrite(const void*, size_t, size_t, void*); int fclose(void*); int printf(const char*, ...);
int memcmp(const void*, const void*, size_t); long ftell(void*); void* malloc(size_t); void free(void*);
}
namespace ata { bool read_sectors(uint32_t, uint32_t, void*) { return false; } }
static unsigned char* g_img; static size_t g_sectors;
static bool rd(uint32_t lba, uint32_t n, void* b){ if((uint64_t)lba+n>g_sectors) return false; for(size_t i=0;i<n*512;i++)((unsigned char*)b)[i]=g_img[(size_t)lba*512+i]; return true; }
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); ++fails; } }while(0)

static unsigned char* load(const char* p, size_t* sectors){ void*f=fopen(p,"rb"); fseek(f,0,2); long n=ftell(f); fseek(f,0,0);
  unsigned char* b=(unsigned char*)malloc(n); fread(b,1,n,f); fclose(f); *sectors=n/512; return b; }
static void cb(const fat32::DirEntryInfo& e, void* ctx){ int* c=(int*)ctx; ++*c; printf("   %s%s %u\n", e.name, e.is_dir?"/":"", e.size); }
int main(int, char** argv){
  size_t sec; g_img=load(argv[1],&sec); g_sectors=sec;
  CHECK(fat32::mount(&rd,0)); CHECK(fat32::self_test());
  static char buf[8192]; size_t got=0;
  CHECK(fat32::file_size("BIG.TXT")==3201);
  CHECK(fat32::read("big.txt",buf,sizeof buf,&got) && got==3201);
  bool pat=true; for(int i=0;i<3200;i++) if(buf[i]!="0123456789abcdef"[i%16]) pat=false; CHECK(pat && buf[3200]=='\n');
  CHECK(fat32::read("/DOCS/NOTES.TXT",buf,sizeof buf,&got) && got==27);
  CHECK(fat32::read("BIG.TXT",buf,100,&got) && got==100);
  int c=0; CHECK(fat32::list("/",cb,&c)); CHECK(c==3); c=0; CHECK(fat32::list("DOCS",cb,&c)); CHECK(c==3);   // ., .., NOTES.TXT
  CHECK(!fat32::list("FATTEST.TXT",cb,&c)); CHECK(fat32::file_size("DOCS")==-1); CHECK(fat32::file_size("MISSING")==-1);
  CHECK(!fat32::read("TOOLONGNAME9.TXT",buf,8,&got)); CHECK(!fat32::read("A.B.C",buf,8,&got));
  // --- corruption: each must be rejected or fail cleanly, never hang ---
  auto restore=[&](size_t off,const unsigned char*v,size_t n){ for(size_t i=0;i<n;i++) g_img[off+i]=v[i]; };
  auto mutate=[&](size_t off,unsigned char v,unsigned char* old){ *old=g_img[off]; g_img[off]=v; };
  unsigned char o;
  mutate(510,0,&o); CHECK(!fat32::mount(&rd,0)); g_img[510]=o;
  mutate(13,3,&o);  CHECK(!fat32::mount(&rd,0)); g_img[13]=o;          // sectors/cluster not a power of two
  mutate(17,1,&o);  CHECK(!fat32::mount(&rd,0)); g_img[17]=o;          // FAT12/16 root entry count
  mutate(44,0,&o);  CHECK(!fat32::mount(&rd,0)); g_img[44]=o;          // root cluster 0
  mutate(12,4,&o);  CHECK(!fat32::mount(&rd,0)); g_img[12]=o;          // 1024-byte sectors
  CHECK(fat32::mount(&rd,0));
  // chain loop / truncation / bad cluster in the FAT of BIG.TXT
  unsigned fatoff=32*512; unsigned first=0; // find BIG.TXT's first cluster from root dir (cluster 2 -> data_start)
  unsigned fat_sectors=g_img[36]|(g_img[37]<<8); unsigned data=32+2*fat_sectors; unsigned char* root=g_img+(size_t)data*512;
  for(int i=0;i<16;i++){ if(memcmp(root+i*32,"BIG     TXT",11)==0) first=root[i*32+26]|(root[i*32+27]<<8); }
  CHECK(first>=2);
  unsigned char* ent=g_img+fatoff+first*4; unsigned char save[4]; for(int i=0;i<4;i++) save[i]=ent[i];
  unsigned nxt=ent[0]|(ent[1]<<8)|(ent[2]<<16)|(ent[3]<<24);
  ent[0]=first&255; ent[1]=(first>>8)&255; ent[2]=0; ent[3]=0;           // self-loop
  fat32::mount(&rd,0); CHECK(!fat32::read("BIG.TXT",buf,sizeof buf,&got)); restore(fatoff+first*4,save,4);
  ent[0]=0xF8;ent[1]=0xFF;ent[2]=0xFF;ent[3]=0x0F;                       // premature end of chain
  fat32::mount(&rd,0); CHECK(!fat32::read("BIG.TXT",buf,sizeof buf,&got)); CHECK(got<3201); restore(fatoff+first*4,save,4);
  ent[0]=0xF7;ent[1]=0xFF;ent[2]=0xFF;ent[3]=0x0F;                       // bad-cluster marker
  fat32::mount(&rd,0); CHECK(!fat32::read("BIG.TXT",buf,sizeof buf,&got)); restore(fatoff+first*4,save,4);
  ent[0]=0;ent[1]=0;ent[2]=0;ent[3]=0;                                    // free cluster in the middle
  fat32::mount(&rd,0); CHECK(!fat32::read("BIG.TXT",buf,sizeof buf,&got)); restore(fatoff+first*4,save,4);
  fat32::mount(&rd,0); CHECK(fat32::read("BIG.TXT",buf,sizeof buf,&got) && got==3201 && nxt!=0);
  printf(fails?"RESULT: %d FAILURES\n":"RESULT: ALL PASS\n",fails); return fails!=0;
}
