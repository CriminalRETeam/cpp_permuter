struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Check(Ped* p)
{
    if (p->a > 4)
    {
        g(1);
    }
}
