typedef unsigned int size_t;

void* __aeabi_memcpy(void* dst, const void* src, size_t n)
{
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  while (n--) *d++ = *s++;
  return dst;
}

void* __aeabi_memmove(void* dst, const void* src, size_t n)
{
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (d < s) { while (n--) *d++ = *s++; }
  else { d += n; s += n; while (n--) *--d = *--s; }
  return dst;
}

void* __aeabi_memset(void* p, int v, size_t n)
{
  unsigned char* d = (unsigned char*)p;
  unsigned char c = (unsigned char)v;
  while (n--) *d++ = c;
  return p;
}

void* __aeabi_memclr(void* p, size_t n)
{
  return __aeabi_memset(p, 0, n);
}

void* __aeabi_memcpy4(void* d, const void* s, size_t n) { return __aeabi_memcpy(d, s, n); }
void* __aeabi_memcpy8(void* d, const void* s, size_t n) { return __aeabi_memcpy(d, s, n); }
void* __aeabi_memset4(void* p, int v, size_t n) { return __aeabi_memset(p, v, n); }
void* __aeabi_memset8(void* p, int v, size_t n) { return __aeabi_memset(p, v, n); }
void* __aeabi_memclr4(void* p, size_t n) { return __aeabi_memclr(p, n); }
void* __aeabi_memclr8(void* p, size_t n) { return __aeabi_memclr(p, n); }
