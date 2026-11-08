// Calls the real PrehKeyTec compiler: int WINAPI mwxc(const char* mwf, const char* mwx, char* out, void* progress_cb)
// Build:  i686-w64-mingw32-gcc mwxc_loader.c -o mwxc_loader.exe   (must be 32-bit, mwxc32.dll is x86)
// Run:    WINEDEBUG=-all wine mwxc_loader.exe in.mwf out.mwx   (cwd = folder with mwxc32.dll; ANSI paths; callback may be NULL)
#include <windows.h>
#include <stdio.h>
typedef int (WINAPI *mwxc_t)(const char*, const char*, char*, void*);
int main(int argc,char**argv){
  HMODULE h=LoadLibraryA("mwxc32.dll");
  if(!h){printf("LoadLibrary failed %lu\n",GetLastError());return 2;}
  mwxc_t f=(mwxc_t)GetProcAddress(h,"mwxc");
  printf("mwxc=%p\n",(void*)f); fflush(stdout);
  if(!f) return 3;
  static char out[1000]; memset(out,0,sizeof out);
  int r=f(argv[1],argv[2],out,NULL);
  printf("rc=%d out='%s'\n",r,out);
  return 0;
}
