#define DEFINE_GLOBAL(type, name, addr) type name
#define DEFINE_GLOBAL_INIT(type, name, value, addr) type name = value
#define EXTERN_GLOBAL(type, name) extern type name

DEFINE_GLOBAL_INIT(short, gStep, 16, 0x1000);
EXTERN_GLOBAL(short, gBase);
DEFINE_GLOBAL(int, gUnused, 0x1008);

short Advance(short a)
{
    return a + gStep + gBase;
}
