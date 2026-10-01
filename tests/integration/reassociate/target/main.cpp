struct Ped { int a, b, c; int state; };
void g(int v);

int Sum(Ped* p)
{
    return p->a + (p->b + p->c);
}
