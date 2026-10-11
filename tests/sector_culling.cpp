#include "port/types.h"
#include "Sector.h"
#include "SceneNode.h"
#include "Graphics.h"
#include "high_address.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <initializer_list>
#include <cstdint>
extern void Scene_InitFixedMathTables();
extern void Sector_CullGridAroundViewNode(SceneNode *, void *);
#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "culling: failed line %d: %s\n", __LINE__, #condition); return 1; } } while (0)
int main(){
 Scene_InitFixedMathTables();
 const int rows=8,columns=8,count=64,size=10;
 g_sectorsPerRow=columns;g_sectorRows=rows;g_sectorCount=count;g_sectorSize=size*65536;g_sectorHalfSize=size*32768;g_sectorScale=65536/size;
 // Use a large allocation so a non-PIE Linux executable also exercises high
 // native addresses, rather than its low brk heap. The fixture has 64 cells.
 auto *cells=(Sector *)Test_AllocateHighAddress(2*1024*1024);assert(cells);
 if(sizeof(void *)==8)assert((uintptr_t)cells>UINT32_MAX);
 for(int r: {0,3,4,7})for(int c:{0,3,4,7})for(int yaw=0;yaw<8;yaw++)for(int radius:{20,50,100}){
  for(int i=0;i<count;i++){
   Sector &s=cells[i];memset(&s,0xa5,sizeof(s));s.x=(i%columns)*size*65536;s.y=0;s.z=-(i/columns)*size*65536;
   int loX=s.x-g_sectorHalfSize,hiX=s.x+g_sectorHalfSize,loZ=s.z-g_sectorHalfSize,hiZ=s.z+g_sectorHalfSize;
   int rect[4][2]={{loX,loZ},{hiX,loZ},{hiX,hiZ},{loX,hiZ}};memcpy(s.bounds,rect,sizeof(rect));g_sectors[i]=&s;
  }
  SceneNode node={};node.world.position={c*size*65536,0,-r*size*65536};
  double angle=yaw*3.141592653589793/4;node.world.forward={int(llround(sin(angle)*65536)),0,int(llround(cos(angle)*65536))};
  CGraphics::m_farPlaneFixed=radius*65536;
  Sector original[count];memcpy(original,cells,sizeof(original));
  unsigned short previous[count];int previousCount=-1;
  for(int pass=0;pass<3;pass++){
   Sector_CullGridAroundViewNode(&node,NULL);
   if(pass){assert(previousCount==g_sectorCullEnabled);assert(!memcmp(previous,g_visibleSectorIndices,previousCount*sizeof(short)));}
   previousCount=g_sectorCullEnabled;memcpy(previous,g_visibleSectorIndices,previousCount*sizeof(short));
   bool marked[count]={};for(int j=0;j<g_sectorCullEnabled;j++){int id=g_visibleSectorIndices[j];assert(id>=0&&id<count&&!marked[id]);marked[id]=true;}
   for(int i=0;i<count;i++){
    assert(!memcmp(original[i].bounds,cells[i].bounds,sizeof(cells[i].bounds)));
    Sector expected=original[i];if(marked[i]){
     int dx=expected.x-node.world.position.x,dz=expected.z-node.world.position.z;
     expected.viewDistanceSquared=int((int64_t(dx)*dx>>16)+(int64_t(dz)*dz>>16));expected.field_0x80=0;expected.field_0x84=1;
    }
    assert(!memcmp(&expected,&cells[i],sizeof(expected)));assert(bool(g_sectorVisibleBits[i>>5]&(1u<<(i&31)))==marked[i]);
   }
  }
 }
 Test_FreeHighAddress(cells);puts("PASS 384 native culling cases, three passes each: stable bounds, full records, masks and sector lists");
}
