#include "port/types.h"
#include "port/replay_disk.h"
#include "port/disk_layouts.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(){
    for(int type=0;type<3;type++)for(int controller=0;controller<2;controller++) {
        ReplayStream header{};header.type=type;header.laneCapacity=3;header.samplesPerLane=5;header.laneCount=2;
        header.headingFrom=-123;header.headingTo=456;header.field_0x10c=8;
        const size_t record=type==0?0x114c:0x5c, sample=type==2?16:4;
        const size_t payload=3*(record+5*sample+2);
        std::vector<BYTE> input(0x110+payload);
        Disk_Pack(input.data(),&header,&g_diskLayout_ReplayStream);
        uint32_t flag=controller?0xdeadbeef:0;memcpy(input.data(),&flag,4);
        for(size_t i=0;i<payload;i++)input[0x110+i]=(BYTE)(i*17+13);
        const size_t counts=0x110+3*(record+5*sample);
        const short lengths[3]={5,3,0};memcpy(input.data()+counts,lengths,sizeof(lengths));
        auto original=input;
        ReplayStream *p=ReplayDisk_Read(input.data(),input.size());REQUIRE(p);
        REQUIRE(input==original && p->type==type && p->laneCapacity==3 && p->samplesPerLane==5 && p->laneCount==2);
        REQUIRE(p->steeringModeOption==flag && p->headingFrom==-123 && p->headingTo==456 && p->field_0x10c==8);
        REQUIRE((BYTE *)p->pLaneSamples==(BYTE *)p+sizeof(*p)+counts-0x110 && !memcmp(p->pLaneSamples,lengths,sizeof(lengths)));
        REQUIRE(!memcmp((BYTE *)p+sizeof(*p),input.data()+0x110,payload));
        REQUIRE(type==0?(BYTE *)p->pInputs==(BYTE *)p+sizeof(*p)&&!p->pStates:(BYTE *)p->pStates==(BYTE *)p+sizeof(*p)&&!p->pInputs);
        std::vector<BYTE> output;REQUIRE(ReplayDisk_Write(p,output));REQUIRE(output.size()==input.size());
        for(int i=0;i<g_diskLayout_ReplayStream.pointerCount;i++){
            auto &field=g_diskLayout_ReplayStream.pointers[i];uint32_t value=0;memcpy(original.data()+field.disk,&value,4);
        }
        REQUIRE(output==original);ReplayStream *again=ReplayDisk_Read(output.data(),output.size());REQUIRE(again);
        free(again);free(p);
        for(size_t size:{size_t(0),size_t(4),size_t(0x10f),input.size()-1,input.size()+1}) {
            // Enlarged length refers to allocated guard bytes, never an invalid test pointer.
            auto guarded=input;guarded.resize(input.size()+1);REQUIRE(!ReplayDisk_Read(guarded.data(),size));
        }
        input=original;short invalid=-1;memcpy(input.data()+counts,&invalid,2);REQUIRE(!ReplayDisk_Read(input.data(),input.size()));
        input=original;invalid=6;memcpy(input.data()+counts,&invalid,2);REQUIRE(!ReplayDisk_Read(input.data(),input.size()));
        input=original;invalid=-1;memcpy(input.data()+0xfc,&invalid,2);REQUIRE(!ReplayDisk_Read(input.data(),input.size()));
        input=original;invalid=4;memcpy(input.data()+0x100,&invalid,2);REQUIRE(!ReplayDisk_Read(input.data(),input.size()));
        input=original;int badType=3;memcpy(input.data()+0x1c,&badType,4);REQUIRE(!ReplayDisk_Read(input.data(),input.size()));
    }
    puts("replay disk: all three stream types, numeric steering option, scalar/payload round trips, borrowed input and malformed dimensions passed");
}
