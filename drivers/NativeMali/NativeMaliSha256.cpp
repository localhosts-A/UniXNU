/* SPDX-License-Identifier: MIT
 * Derived from NativeAdrenoSha256.cpp at NativeAdreno b686b9054f57d393c80195b82568a09f8ca106a2.
 * Copyright (c) 2026 NativeAdreno contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "NativeMaliSha256.h"
#include <string.h>
namespace native_mali {
static const uint32_t K[64]={
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
static inline uint32_t rotr(uint32_t v,unsigned n) { return (v>>n)|(v<<(32-n)); }
static void compress(uint32_t s[8],const uint8_t block[64]) {
    uint32_t w[64];
    for(unsigned i=0;i<16;++i) w[i]=uint32_t(block[i*4])<<24|uint32_t(block[i*4+1])<<16|uint32_t(block[i*4+2])<<8|block[i*4+3];
    for(unsigned i=16;i<64;++i) {
        const uint32_t a=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        const uint32_t b=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+a+w[i-7]+b;
    }
    uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
    for(unsigned i=0;i<64;++i) {
        const uint32_t t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+K[i]+w[i];
        const uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}
void sha256(const uint8_t *data,size_t bytes,uint8_t digest[32]) {
    uint32_t s[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    size_t at=0;
    while(bytes-at>=64) { compress(s,data+at);at+=64; }
    uint8_t tail[64]{};const size_t rest=bytes-at;
    if(rest)memcpy(tail,data+at,rest);tail[rest]=0x80;
    if(rest>=56) { compress(s,tail);memset(tail,0,64); }
    const uint64_t bits=uint64_t(bytes)*8;
    for(unsigned i=0;i<8;++i)tail[63-i]=bits>>(i*8);
    compress(s,tail);
    for(unsigned i=0;i<8;++i) for(unsigned j=0;j<4;++j)digest[i*4+j]=s[i]>>(24-j*8);
}
} // namespace native_mali
