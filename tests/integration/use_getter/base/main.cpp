struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

int Next(Ped* p)
{
    return p->a + 1;
}
