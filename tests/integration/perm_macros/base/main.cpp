struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Run(Ped* p)
{
    PERM_LINESWAP(
    g(p->a);
    g(p->b);
    g(p->c);
    )
    p->state = PERM_GENERAL(1, 2, 3);
}
