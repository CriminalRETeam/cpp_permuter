struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Check(Ped* p)
{
    if (p->a >= 5)
    {
        g(1);
    }
}
